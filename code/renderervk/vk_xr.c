/* Optional OpenXR Vulkan2 presentation. The renderer owns the live runtime;
 * desktop probing uses a separate, short-lived instance in the client. */
#include "tr_local.h"
#include "vk_xr.h"
#include "vk_xr_live.h"
#include "vk_xr_input.h"
#include "vk_xr_refresh.h"
#include "vk_xr_gaze.h"
#include "../vrcommon/vr_float.h"
#include "../vrcommon/vr_render_extent.h"

#if VK_XR_DIRECT_MAX_IMAGES != VK_XRVK_MAX_IMAGES
#error "vk.xr_direct must hold every XR swapchain image"
#endif

static vkXRLive_t live;
static vkXRVk_t xr;
static vkXRInput_t input;
static vkXRGaze_t gaze;
static float gazeDirection[3];
static int gazeValid;
static vkFovCenters_t foveationCenters;
static VkDevice xrDevice;
static VkPhysicalDevice xrPhysical;
static PFN_vkGetInstanceProcAddr getVkProc;
static PFN_vkCmdPipelineBarrier barrier;
static PFN_vkCmdBlitImage blit;
static PFN_vkDeviceWaitIdle waitIdle;
static PFN_vkGetPhysicalDeviceFormatProperties formatProperties;
static struct {
	VkPhysicalDevice physical;
	VkFormat source, destination;
	VkFormatProperties sourceProperties, destinationProperties;
} presentationFormats;
static qboolean requested, active, copied, submitted, failed;
static char failure[256];
static vrScreenAnchor_t screenAnchor;
static vrScreenGeometry_t screenGeometry;
static vec3_t floorOrigin;
static vkXRRefresh_t refresh;
static float refreshRequested = -1;
static float zoomLevel = 1;
static cvar_t *hudScale, *hudDepth, *hudOffset, *screenCurvature;
static cvar_t *refreshRate, *virtualScreenMode, *worldscale, *worldscaleScaler, *foveation, *foveationStrength;
static struct {
	qboolean valid;
	XrTime time;
	float scale, depth, offset;
	float matrix[16];
} hudMatrices[2];
static struct {
	qboolean valid;
	XrTime time;
	float matrix[16];
} screenMatrices[2];
float VK_XR_ScopeScaleY( void ) {
	if ( !VK_XR_Drawing() || !xr.scope || !xr.target.height || !glConfig.vidWidth ) {
		return 1;
	}
	return (float)xr.target.width * glConfig.vidHeight / (xr.target.height * (float)glConfig.vidWidth);
}

qboolean VK_XR_ScopeNeedsBands( void ) {
	return xr.scope;
}

void VK_XR_SetZoom( qboolean zoomed, float *level ) {
	xr.scope = VK_XR_Drawing() && !screenGeometry.visible && zoomed;
	if ( xr.scope ) {
		zoomLevel = MIN( zoomLevel + .05f, 2.5f );
	} else {
		zoomLevel = MAX( zoomLevel - .25f, 1.0f );
	}
	if ( level ) {
		*level = zoomLevel;
	}
}

static void VKXR_UpdateRefresh( void ) {
	float wanted;
	XrResult result;
	if ( !refresh.available || !xr.running ) {
		return;
	}
	if ( !refreshRate ) {
		return;
	}
	wanted = refreshRate ? refreshRate->value : 0;
	if ( !VR_FloatFinite( wanted ) || wanted < 0 ) {
		wanted = 0;
	}
	if ( wanted == refreshRequested ) {
		return;
	}
	refreshRequested = wanted;
	result = VK_XRRefresh_Request( &refresh, wanted );
	if ( XR_FAILED( result ) ) {
		ri.Printf( PRINT_WARNING, "OpenXR refresh request %.1f Hz unavailable (%d)\n", wanted, (int)result );
	}
}

static qboolean VKXR_Check( XrResult result, const char *operation ) {
	if ( XR_FAILED( result ) ) {
		Com_sprintf( failure, sizeof( failure ), "%s failed (%d)", operation, (int)result );
		failed = qtrue;
		return qfalse;
	}
	return qtrue;
}

qboolean VK_XR_PrepareInit( qboolean enabled ) {
	requested = enabled;
	failure[0] = 0;
	failed = qfalse;
	if ( !enabled ) {
		return qtrue;
	}
	if ( live.instance ) {
		return qtrue;
	}
	if ( !VKXR_Check( VK_XRLive_Open( &live ), "OpenXR loader/system" ) ) {
		return qfalse;
	}
	if ( !VKXR_Check( VK_XRVK_Init( &xr, live.instance, live.system, live.getproc, live.formatList ),
					 "OpenXR Vulkan2 initialization" ) ) {
		VK_XRLive_Close( &live );
		return qfalse;
	}
	return qtrue;
}

qboolean VK_XR_Enabled( void ) {
	return requested && live.instance != XR_NULL_HANDLE;
}
qboolean VK_XR_Drawing( void ) {
	return active && xr.renderable && !failed;
}

void VK_XR_SetVirtualScreen( qboolean enabled, refXRFrame_t *frame ) {
	/* Frames run before the session goes active, ahead of the cvar lookup. */
	float curvature = screenCurvature ? screenCurvature->value : VR_SCREEN_REFERENCE_CURVATURE;
	if ( !frame ) {
		return;
	}
	VR_ScreenUpdate( &screenAnchor, &screenGeometry, frame->head.position, frame->head.orientation,
					 enabled && frame->renderable, virtualScreenMode ? virtualScreenMode->integer : 0,
					 curvature );
	frame->screen = screenGeometry;
}
const vrScreenGeometry_t *VK_XR_Screen( void ) {
	return VK_XR_Drawing() && screenGeometry.visible ? &screenGeometry : NULL;
}
void VK_XR_FloorOrigin( vec3_t origin ) {
	VectorCopy( floorOrigin, origin );
}
void VK_XR_ScreenCaptureRect( int eyeWidth, int eyeHeight, int rect[4] ) {
	VR_ScreenCaptureRect( eyeWidth, eyeHeight, eyeWidth, eyeHeight,
						  (xr.views[0].fov.angleUp + xr.views[1].fov.angleUp) * .5f,
						  (xr.views[0].fov.angleDown + xr.views[1].fov.angleDown) * .5f, rect );
}

void VK_XR_EyeMatrix( int eye, float matrix[16] ) {
	const XrPosef *pose = &xr.views[eye].pose;
	const XrQuaternionf *q = &pose->orientation;
	float view[16] = {0}, projection[16] = {0};
	float l = tanf( xr.views[eye].fov.angleLeft ), r = tanf( xr.views[eye].fov.angleRight );
	float b = tanf( xr.views[eye].fov.angleDown ), t = tanf( xr.views[eye].fov.angleUp );
	int i, j, k;
	/* Transpose of the rigid head rotation; translation is -R^T * eye. */
	view[0] = 1 - 2 * (q->y * q->y + q->z * q->z);
	view[1] = 2 * (q->x * q->y - q->w * q->z);
	view[2] = 2 * (q->x * q->z + q->w * q->y);
	view[4] = 2 * (q->x * q->y + q->w * q->z);
	view[5] = 1 - 2 * (q->x * q->x + q->z * q->z);
	view[6] = 2 * (q->y * q->z - q->w * q->x);
	view[8] = 2 * (q->x * q->z - q->w * q->y);
	view[9] = 2 * (q->y * q->z + q->w * q->x);
	view[10] = 1 - 2 * (q->x * q->x + q->y * q->y);
	for ( i = 0; i < 3; i++ ) {
		view[12 + i] =
			-(view[i] * pose->position.x + view[4 + i] * pose->position.y + view[8 + i] * pose->position.z);
	}
	view[15] = 1;
	projection[0] = 2 / (r - l);
	projection[5] = -2 / (t - b);
	projection[8] = (r + l) / (r - l);
	projection[9] = -(t + b) / (t - b);
	projection[11] = -1;
#ifdef USE_REVERSED_DEPTH
	projection[10] = 0.01f / 99.99f;
	projection[14] = 1.0f / 99.99f;
#else
	projection[10] = -100.0f / 99.99f;
	projection[14] = -1.0f / 99.99f;
#endif
	for ( i = 0; i < 4; i++ ) {
		for ( j = 0; j < 4; j++ ) {
			matrix[i * 4 + j] = 0;
			for ( k = 0; k < 4; k++ ) {
				matrix[i * 4 + j] += projection[k * 4 + j] * view[i * 4 + k];
			}
		}
	}
}

uint32_t VK_XR_ApiVersion( uint32_t version ) {
	uint32_t minimum, maximum;
	if ( !VK_XR_Enabled() ) {
		return version;
	}
	minimum = VK_MAKE_VERSION( XR_VERSION_MAJOR( xr.requirements.minApiVersionSupported ),
							   XR_VERSION_MINOR( xr.requirements.minApiVersionSupported ), 0 );
	maximum = VK_MAKE_VERSION( XR_VERSION_MAJOR( xr.requirements.maxApiVersionSupported ),
							   XR_VERSION_MINOR( xr.requirements.maxApiVersionSupported ), 0 );
	/* Fragment-rate attachment support needs Vulkan 1.1 feature queries.
	 * Negotiate only what both the loader and XR runtime permit. */
	if ( version < VK_API_VERSION_1_1 && maximum >= VK_API_VERSION_1_1 ) {
		PFN_vkEnumerateInstanceVersion enumerate = (PFN_vkEnumerateInstanceVersion)ri.VK_GetInstanceProcAddr(
			VK_NULL_HANDLE, "vkEnumerateInstanceVersion" );
		uint32_t supported = VK_API_VERSION_1_0;
		if ( enumerate && enumerate( &supported ) == VK_SUCCESS && supported >= VK_API_VERSION_1_1 ) {
			version = VK_API_VERSION_1_1;
		}
	}
	if ( version < minimum ) {
		version = minimum;
	}
	if ( version > maximum ) {
		version = maximum;
	}
	return version;
}

VkResult VK_XR_CreateInstance( PFN_vkCreateInstance normal, const VkInstanceCreateInfo *info,
							   VkInstance *instance ) {
	VkResult result = VK_ERROR_INITIALIZATION_FAILED;
	if ( !VK_XR_Enabled() ) {
		return normal( info, NULL, instance );
	}
	getVkProc =
		(PFN_vkGetInstanceProcAddr)ri.VK_GetInstanceProcAddr( VK_NULL_HANDLE, "vkGetInstanceProcAddr" );
	if ( !getVkProc || !VKXR_Check( VK_XRVK_CreateInstance( &xr, getVkProc, info, NULL, instance, &result ),
								   "xrCreateVulkanInstanceKHR" ) ) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return result;
}

VkPhysicalDevice VK_XR_PhysicalDevice( VkInstance instance ) {
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	if ( !VKXR_Check( VK_XRVK_GetPhysicalDevice( &xr, instance, &physical ), "xrGetVulkanGraphicsDevice2KHR" ) ) {
		ri.Error( ERR_DROP, "%s", failure );
	}
	return physical;
}

VkResult VK_XR_CreateDevice( PFN_vkCreateDevice normal, VkPhysicalDevice physical,
							 const VkDeviceCreateInfo *info, VkDevice *device ) {
	VkResult result = VK_ERROR_INITIALIZATION_FAILED;
	if ( !VK_XR_Enabled() ) {
		return normal( physical, info, NULL, device );
	}
	if ( !VKXR_Check( VK_XRVK_CreateDevice( &xr, getVkProc, physical, info, NULL, device, &result ),
					 "xrCreateVulkanDeviceKHR" ) ) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return result;
}

void VK_XR_Bind( VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily ) {
	XrGraphicsBindingVulkan2KHR binding;
	VkPhysicalDeviceProperties properties;
	PFN_vkGetPhysicalDeviceProperties getProperties;
	const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
								VK_FORMAT_B8G8R8A8_UNORM};
	if ( !VK_XR_Enabled() ) {
		return;
	}
	/* Handles may be reused by a recreated device; do not retain old queries. */
	Com_Memset( &presentationFormats, 0, sizeof( presentationFormats ) );
	Com_Memset( &binding, 0, sizeof( binding ) );
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
	binding.instance = instance;
	binding.physicalDevice = physical;
	binding.device = device;
	binding.queueFamilyIndex = queueFamily;
	getProperties = (PFN_vkGetPhysicalDeviceProperties)getVkProc( instance, "vkGetPhysicalDeviceProperties" );
	getProperties( physical, &properties );
	xr.maxEyeWidth = MIN( properties.limits.maxFramebufferWidth, properties.limits.maxImageDimension2D );
	xr.maxEyeHeight = MIN( properties.limits.maxFramebufferHeight, properties.limits.maxImageDimension2D );
	xr.renderScale = atof( ri.Cvar_VariableString( "vr_superSampling" ) );
	if ( !VKXR_Check( VK_XRVK_Bind( &xr, &binding, formats, ARRAY_LEN( formats ) ), "OpenXR session/swapchains" ) ) {
		ri.Error( ERR_DROP, "%s", failure );
		return;
	}
	ri.Printf( PRINT_ALL, "OpenXR swapchain %ux%u, 2 layers, %u images, usage 0x%x, format list %s\n",
			   xr.target.width, xr.target.height, xr.target.count, (unsigned)VK_XRVK_TARGET_USAGE,
			   xr.formatList ? "enabled" : "unavailable" );
	xrDevice = device;
	xrPhysical = physical;
	{
		char rates[512];
		XrResult result =
			VK_XRRefresh_Init( &refresh, live.instance, xr.session, live.getproc, live.displayRefresh );
		if ( XR_FAILED( result ) ) {
			ri.Printf( PRINT_WARNING, "OpenXR refresh controls unavailable (%d)\n", (int)result );
		}
		VK_XRRefresh_FormatRates( &refresh, rates, sizeof( rates ) );
		ri.Cvar_Set( "vr_refreshrates", rates );
		refreshRequested = -1;
		VKXR_UpdateRefresh();
	}
	if ( !VKXR_Check( VK_XRInput_Init( &input, live.instance, live.getproc, live.picoInteraction ),
					 "OpenXR input actions" ) ) {
		ri.Error( ERR_DROP, "%s", failure );
		return;
	}
	{
		XrResult result =
			VK_XRGaze_Init( &gaze, live.instance, live.system, input.set, live.getproc, live.eyeGaze );
		if ( XR_FAILED( result ) ) {
			ri.Printf( PRINT_WARNING, "OpenXR gaze unavailable (%d)\n", (int)result );
		}
	}
	if ( !VKXR_Check( VK_XRInput_Attach( &input, xr.session ), "OpenXR input attach" ) ) {
		ri.Error( ERR_DROP, "%s", failure );
		return;
	}
	if ( XR_FAILED( VK_XRGaze_Attach( &gaze, xr.session ) ) ) {
		VK_XRGaze_Shutdown( &gaze );
	}
	barrier = (PFN_vkCmdPipelineBarrier)getVkProc( instance, "vkCmdPipelineBarrier" );
	blit = (PFN_vkCmdBlitImage)getVkProc( instance, "vkCmdBlitImage" );
	waitIdle = (PFN_vkDeviceWaitIdle)getVkProc( instance, "vkDeviceWaitIdle" );
	formatProperties =
		(PFN_vkGetPhysicalDeviceFormatProperties)getVkProc( instance, "vkGetPhysicalDeviceFormatProperties" );
}

void VK_XR_TargetSize( uint32_t *width, uint32_t *height ) {
	if ( !VK_XR_Enabled() || !xr.session || !xr.target.handle ) {
		return;
	}
	*width = xr.target.width;
	*height = xr.target.height;
}

/* Module layouts cache glConfig at registration. A runtime resolution change
 * must rebuild them through vid_restart, not just replace GPU attachments. */
qboolean VK_XR_ResolutionChanged( void ) {
	XrViewConfigurationView views[2];
	uint32_t count, i, width = 0, height = 0;
	if ( !active || !xr.session || failed || xr.lost || VK_XRVK_TargetsBusy( &xr ) )
		return qfalse;
	Com_Memset( views, 0, sizeof( views ) );
	views[0].type = views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	if ( xr.xr.EnumerateViewConfigurationViews( xr.instance, xr.system,
											   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count,
											   views ) != XR_SUCCESS ||
		count != 2 )
		return qfalse;
	for ( i = 0; i < 2; i++ ) {
		uint32_t w, h, mw = views[i].maxImageRectWidth, mh = views[i].maxImageRectHeight;
		if ( !mw )
			mw = views[i].recommendedImageRectWidth;
		if ( !mh )
			mh = views[i].recommendedImageRectHeight;
		if ( xr.maxEyeWidth && xr.maxEyeWidth < mw )
			mw = xr.maxEyeWidth;
		if ( xr.maxEyeHeight && xr.maxEyeHeight < mh )
			mh = xr.maxEyeHeight;
		if ( !VR_RenderExtent( views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight, mw, mh,
							  xr.renderScale, &w, &h ) )
			return qfalse;
		width = MAX( width, w );
		height = MAX( height, h );
	}
	return width != (uint32_t)glConfig.vidWidth || height != (uint32_t)glConfig.vidHeight;
}

qboolean VK_XR_SetActive( qboolean enabled ) {
	if ( enabled && (!xr.session || !xr.target.handle || failed || xr.lost) ) {
		return qfalse;
	}
	active = enabled;
	hudMatrices[0].valid = hudMatrices[1].valid = qfalse;
	screenMatrices[0].valid = screenMatrices[1].valid = qfalse;
	if ( enabled ) {
		hudScale = ri.Cvar_Get( "vr_hudScale", "1", 0 );
		hudDepth = ri.Cvar_Get( "vr_currentHudDepth", "3", 0 );
		hudOffset = ri.Cvar_Get( "vr_hudYOffset", "0", 0 );
		screenCurvature = ri.Cvar_Get( "vr_screenCurvature", "0.5", 0 );
		refreshRate = ri.Cvar_Get( "vr_refreshrate", "90", 0 );
		virtualScreenMode = ri.Cvar_Get( "vr_virtualScreenMode", "0", 0 );
		worldscale = ri.Cvar_Get( "vr_worldscale", "32.0", 0 );
		worldscaleScaler = ri.Cvar_Get( "vr_worldscaleScaler", "1.0", 0 );
		foveation = ri.Cvar_Get( "vr_foveation", "2", 0 );
		foveationStrength = ri.Cvar_Get( "vr_foveationStrength", "2", 0 );
	}
	if ( !enabled ) {
		VK_XRInput_Reset( &input );
		xr.scope = 0;
		zoomLevel = 1;
	}
	/* Multiview supplies both eyes in one centered client frame. */
	glConfig.stereoEnabled = qfalse;
	return qtrue;
}

static XrPosef VKXR_CenterPose( void ) {
	XrPosef center;
	const XrQuaternionf *a = &xr.views[0].pose.orientation, *b = &xr.views[1].pose.orientation;
	float sign = a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w < 0 ? -1.0f : 1.0f;
	float length;
	center.orientation.x = a->x + sign * b->x;
	center.orientation.y = a->y + sign * b->y;
	center.orientation.z = a->z + sign * b->z;
	center.orientation.w = a->w + sign * b->w;
	length =
		sqrtf( center.orientation.x * center.orientation.x + center.orientation.y * center.orientation.y +
			   center.orientation.z * center.orientation.z + center.orientation.w * center.orientation.w );
	if ( length > 0 ) {
		center.orientation.x /= length;
		center.orientation.y /= length;
		center.orientation.z /= length;
		center.orientation.w /= length;
	} else {
		center.orientation.w = 1;
	}
	center.position.x = (xr.views[0].pose.position.x + xr.views[1].pose.position.x) * 0.5f;
	center.position.y = (xr.views[0].pose.position.y + xr.views[1].pose.position.y) * 0.5f;
	center.position.z = (xr.views[0].pose.position.z + xr.views[1].pose.position.z) * 0.5f;
	return center;
}

static void VKXR_PlaneMatrix( int eye, float scale, float offset, float shift, float matrix[16] ) {
	XrPosef center;
	const XrQuaternionf *q = &center.orientation;
	float eyeMatrix[16], head[16] = {0}, clipToHead[16] = {0}, rotation[16], combined[16];
	float l, r, b, t, p0, p5, p9;
	int i, j, k;
	center = VKXR_CenterPose();
	l = tanf( xr.views[eye].fov.angleLeft );
	r = tanf( xr.views[eye].fov.angleRight );
	b = tanf( xr.views[eye].fov.angleDown );
	t = tanf( xr.views[eye].fov.angleUp );
	p0 = 2 / (r - l);
	p5 = -2 / (t - b);
	p9 = -(t + b) / (t - b);
	VK_XR_EyeMatrix( eye, eyeMatrix );
	head[0] = 1 - 2 * (q->y * q->y + q->z * q->z);
	head[1] = 2 * (q->x * q->y + q->w * q->z);
	head[2] = 2 * (q->x * q->z - q->w * q->y);
	head[4] = 2 * (q->x * q->y - q->w * q->z);
	head[5] = 1 - 2 * (q->x * q->x + q->z * q->z);
	head[6] = 2 * (q->y * q->z + q->w * q->x);
	head[8] = 2 * (q->x * q->z + q->w * q->y);
	head[9] = 2 * (q->y * q->z - q->w * q->x);
	head[10] = 1 - 2 * (q->x * q->x + q->y * q->y);
	/* Convergence is an explicit clip shift, so this rotation cancels the eye
	 * offset and the depth control applies after eye cant. */
	head[12] = xr.views[eye].pose.position.x;
	head[13] = xr.views[eye].pose.position.y;
	head[14] = xr.views[eye].pose.position.z;
	head[15] = 1;
	clipToHead[0] = scale / p0;
	clipToHead[5] = scale / p5;
	clipToHead[13] = (p9 * 0.5f - offset * scale / glConfig.vidHeight) / p5;
	clipToHead[14] = -1;
	clipToHead[15] = 1;
	for ( i = 0; i < 4; i++ ) {
		for ( j = 0; j < 4; j++ ) {
			rotation[i * 4 + j] = 0;
			for ( k = 0; k < 4; k++ ) {
				rotation[i * 4 + j] += eyeMatrix[k * 4 + j] * head[i * 4 + k];
			}
		}
	}
	for ( i = 0; i < 4; i++ ) {
		for ( j = 0; j < 4; j++ ) {
			combined[i * 4 + j] = 0;
			for ( k = 0; k < 4; k++ ) {
				combined[i * 4 + j] += rotation[k * 4 + j] * clipToHead[i * 4 + k];
			}
		}
	}
	if ( eye ) {
		shift = -shift;
	}
	for ( i = 0; i < 4; i++ ) {
		combined[i * 4] += shift * combined[i * 4 + 3];
#ifdef USE_REVERSED_DEPTH
		combined[i * 4 + 2] = combined[i * 4 + 3];
#else
		combined[i * 4 + 2] = 0;
#endif
	}
	Com_Memcpy( matrix, combined, sizeof( combined ) );
}

void VK_XR_HudMatrix( int eye, float matrix[16] ) {
	float scale = hudScale->value * (2.0f / 3.0f);
	float depth = hudDepth->value;
	float offset = hudOffset->value;
	float shift;
	if ( scale <= 0 || !VR_FloatFinite( scale ) ) {
		scale = 2.0f / 3.0f;
	}
	if ( depth < 0 || !VR_FloatFinite( depth ) ) {
		depth = 3;
	}
	if ( !VR_FloatFinite( offset ) ) {
		offset = 0;
	}
	if ( hudMatrices[eye].valid && hudMatrices[eye].time == xr.displayTime &&
		hudMatrices[eye].scale == scale && hudMatrices[eye].depth == depth &&
		hudMatrices[eye].offset == offset ) {
		Com_Memcpy( matrix, hudMatrices[eye].matrix, sizeof( hudMatrices[eye].matrix ) );
		return;
	}
	shift = 0.05f / (depth + 1) * glConfig.vidHeight * 4 * scale / glConfig.vidWidth;
	VKXR_PlaneMatrix( eye, scale, offset, shift, matrix );
	hudMatrices[eye].valid = qtrue;
	hudMatrices[eye].time = xr.displayTime;
	hudMatrices[eye].scale = scale;
	hudMatrices[eye].depth = depth;
	hudMatrices[eye].offset = offset;
	Com_Memcpy( hudMatrices[eye].matrix, matrix, sizeof( hudMatrices[eye].matrix ) );
}

/* Screen-space 2D covers the client frame at unit distance ahead of the head,
 * with no overlay scale, offset or convergence shift. */
void VK_XR_ScreenMatrix( int eye, float matrix[16] ) {
	if ( screenMatrices[eye].valid && screenMatrices[eye].time == xr.displayTime ) {
		Com_Memcpy( matrix, screenMatrices[eye].matrix, sizeof( screenMatrices[eye].matrix ) );
		return;
	}
	VKXR_PlaneMatrix( eye, 1, 0, 0, matrix );
	screenMatrices[eye].valid = qtrue;
	screenMatrices[eye].time = xr.displayTime;
	Com_Memcpy( screenMatrices[eye].matrix, matrix, sizeof( screenMatrices[eye].matrix ) );
}

int VK_XR_BeginFrame( refXRFrame_t *frame ) {
	int eye;
	XrResult result;
	Com_Memset( frame, 0, sizeof( *frame ) );
	copied = submitted = qfalse;
	if ( !xr.session ) {
		return 0;
	}
	// An unwound frame owes its xrEndFrame before anything else runs.
	if ( xr.frameBegun )
		VK_XRVK_End( &xr, 0 );
	if ( failed || xr.lost ) {
		return -1;
	}
	if ( !VKXR_Check( VK_XRVK_Poll( &xr ), "xrPollEvent" ) || xr.lost ) {
		return -1;
	}
	VKXR_UpdateRefresh();
	frame->running = xr.running;
	frame->focused = xr.state == XR_SESSION_STATE_FOCUSED;
	if ( !xr.running || !active || !xr.target.handle ) {
		return 0;
	}
	result = VK_XRVK_Begin( &xr );
	if ( !VKXR_Check( result, "OpenXR frame begin" ) ) {
		return -1;
	}
	frame->renderable = active && xr.renderable;
	if ( !VKXR_Check(
			VK_XRInput_Sample( &input, xr.space, xr.displayTime, active && frame->focused, &frame->input ),
			"OpenXR input sample" ) ) {
		return -1;
	}
	{
		XrPosef center = VKXR_CenterPose();
		Com_Memcpy( frame->head.position, &center.position, sizeof( frame->head.position ) );
		Com_Memcpy( frame->head.orientation, &center.orientation, sizeof( frame->head.orientation ) );
		if ( VK_XRVK_ConsumeSpaceChange( &xr ) ) {
			/* Rebase STAGE beneath the head and cancel its yaw for the floor.
			 * In raw-stage coordinates that is this translation. */
			VectorClear( floorOrigin );
			if ( xr.spaceType == XR_REFERENCE_SPACE_TYPE_STAGE ) {
				floorOrigin[0] = center.position.x;
				floorOrigin[2] = center.position.z;
			}
			Com_Memset( &screenAnchor, 0, sizeof( screenAnchor ) );
		}
	}
	gazeValid = VK_XRGaze_Sample( &gaze, xr.space, frame->head.orientation, xr.displayTime,
								  frame->renderable && frame->focused, gazeDirection );
	for ( eye = 0; eye < 2; eye++ ) {
		const XrView *view = &xr.views[eye];
		Com_Memcpy( frame->eyes[eye].position, &view->pose.position, sizeof( frame->eyes[eye].position ) );
		Com_Memcpy( frame->eyes[eye].orientation, &view->pose.orientation,
					sizeof( frame->eyes[eye].orientation ) );
		frame->eyes[eye].fov[0] = view->fov.angleLeft;
		frame->eyes[eye].fov[1] = view->fov.angleRight;
		frame->eyes[eye].fov[2] = view->fov.angleUp;
		frame->eyes[eye].fov[3] = view->fov.angleDown;
	}
	return frame->renderable ? 1 : 0;
}

void VK_XR_Submitted( void ) {
	submitted = copied;
}

int VK_XR_EndFrame( void ) {
	if ( !xr.frameBegun ) {
		return failed || xr.lost ? -1 : 0;
	}
	/* Vulkan2 accepts outstanding work on the bound graphics queue. CopyEyes
	 * restores COLOR_ATTACHMENT_OPTIMAL before submission; the runtime orders
	 * its reads on that same queue. Device idle is only needed for teardown.
	 * Recorded commands alone must not make a dropped frame presentable. */
	if ( !VKXR_Check( VK_XRVK_End( &xr, submitted && !failed ), "xrEndFrame" ) ) {
		return -1;
	}
	return failed || xr.lost ? -1 : 0;
}

void VK_XR_ShutdownSession( void ) {
	Com_Memset( &presentationFormats, 0, sizeof( presentationFormats ) );
	if ( xrDevice && waitIdle ) {
		waitIdle( xrDevice );
	}
	VK_XRGaze_Shutdown( &gaze );
	VK_XRInput_Shutdown( &input );
	if ( xr.frameBegun )
		VK_XRVK_End( &xr, 0 );
	VK_XRVK_Shutdown( &xr );
	Com_Memset( &refresh, 0, sizeof( refresh ) );
	refreshRequested = -1;
	xrDevice = VK_NULL_HANDLE;
	xrPhysical = VK_NULL_HANDLE;
	active = copied = submitted = qfalse;
	refreshRate = virtualScreenMode = worldscale = worldscaleScaler = foveation = foveationStrength = NULL;
	zoomLevel = 1;
	gazeValid = 0;
	Com_Memset( &foveationCenters, 0, sizeof( foveationCenters ) );
	ri.Cvar_Set( "vr_foveationCaps", "none" );
	Com_Memset( &screenAnchor, 0, sizeof( screenAnchor ) );
	Com_Memset( &screenGeometry, 0, sizeof( screenGeometry ) );
	VectorClear( floorOrigin );
	glConfig.stereoEnabled = qfalse;
}
void VK_XR_ShutdownInstance( void ) {
	VK_XRLive_Close( &live );
	Com_Memset( &xr, 0, sizeof( xr ) );
	requested = qfalse;
}
int VK_XR_Status( void ) {
	return failed || xr.lost ? -1 : active ? 2 : xr.session ? 1 : 0;
}
const char *VK_XR_LastError( void ) {
	return failure;
}
qboolean VK_XR_Haptic( int hand, float amplitude, int durationMs ) {
	if ( !active || failed || !input.focused ) {
		return qfalse;
	}
	return XR_SUCCEEDED( VK_XRInput_Haptic( &input, hand, amplitude, durationMs ) );
}

static void VKXR_Transition( VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
							 VkAccessFlags sourceAccess, VkAccessFlags destinationAccess ) {
	VkImageMemoryBarrier change;
	Com_Memset( &change, 0, sizeof( change ) );
	change.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	change.srcAccessMask = sourceAccess;
	change.dstAccessMask = destinationAccess;
	change.oldLayout = from;
	change.newLayout = to;
	change.srcQueueFamilyIndex = change.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	change.image = image;
	change.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	change.subresourceRange.levelCount = 1;
	change.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	barrier( command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			 NULL, 1, &change );
}

void VK_XR_CopyEyes( VkCommandBuffer command, VkImage source, VkFormat format, int width, int height,
					 VkImageLayout layout ) {
	VkFormatProperties srcProps, dstProps;
	VkImage image;
	VkFilter filter;
	int eye;
	if ( !VK_XR_Drawing() ) {
		return;
	}
	if ( presentationFormats.physical != xrPhysical || presentationFormats.source != format ||
		presentationFormats.destination != xr.format ) {
		formatProperties( xrPhysical, format, &presentationFormats.sourceProperties );
		formatProperties( xrPhysical, xr.format, &presentationFormats.destinationProperties );
		presentationFormats.physical = xrPhysical;
		presentationFormats.source = format;
		presentationFormats.destination = xr.format;
	}
	srcProps = presentationFormats.sourceProperties;
	dstProps = presentationFormats.destinationProperties;
	if ( !(srcProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
		!(dstProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ) {
		Com_sprintf( failure, sizeof( failure ), "XR presentation formats do not support blit" );
		failed = qtrue;
		return;
	}
	VKXR_Transition( command, source, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
					 VK_ACCESS_TRANSFER_READ_BIT );
	image = xr.target.images[xr.target.index].image;
	filter = srcProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? VK_FILTER_LINEAR
																								: VK_FILTER_NEAREST;
	// Runtime images are discard-written; their old contents are never read.
	VKXR_Transition( command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
					 VK_ACCESS_TRANSFER_WRITE_BIT );
	for ( eye = 0; eye < 2; eye++ ) {
		VkImageBlit region;
		Com_Memset( &region, 0, sizeof( region ) );
		region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
		region.srcSubresource.baseArrayLayer = eye;
		region.dstSubresource.baseArrayLayer = eye;
		region.srcOffsets[1].x = width;
		region.srcOffsets[1].y = height;
		region.srcOffsets[1].z = 1;
		region.dstOffsets[1].x = xr.target.width;
		region.dstOffsets[1].y = xr.target.height;
		region.dstOffsets[1].z = 1;
		blit( command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
			  &region, filter );
	}
	VKXR_Transition( command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					 VK_ACCESS_TRANSFER_WRITE_BIT,
					 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
	VKXR_Transition( command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT,
					 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT );
	copied = qtrue;
}

qboolean VK_XR_SwapchainImages( VkFormat *format, uint32_t *count, VkImage images[VK_XRVK_MAX_IMAGES] ) {
	uint32_t i;
	if ( !xr.session || !xr.target.handle )
		return qfalse;
	*format = xr.format;
	*count = xr.target.count;
	for ( i = 0; i < xr.target.count; i++ )
		images[i] = xr.target.images[i].image;
	return qtrue;
}

uint32_t VK_XR_AcquiredIndex( void ) {
	return xr.target.index;
}

void VK_XR_Rendered( void ) {
	copied = qtrue;
}

/* eye: 0 left, 1 right. */
qboolean VK_XR_EyeView( int eye, refdef_t *view, float fov[4] ) {
	int axis;
	float offset[3], scale;
	const XrPosef *pose;
	XrPosef center;
	if ( !VK_XR_Drawing() || tr_hudDrawing ) {
		return qfalse;
	}
	if ( screenGeometry.visible ) {
		float halfX, halfY;
		if ( tr_hudScreenDrawing ) {
			/* The HUD uses a symmetric 30-degree FOV. */
			halfX = halfY = tanf( 15 * (float)M_PI / 180 );
		} else if ( view->rdflags & RDF_NOWORLDMODEL ) {
			float cropFactor = (float)glConfig.vidHeight / (glConfig.vidWidth * .75f);
			halfX = tanf( view->fov_x * (float)M_PI / 360 ) * cropFactor;
			halfY = tanf( view->fov_y * (float)M_PI / 360 ) * cropFactor;
		} else {
			/* Centered mono runtime projection, not angle-span FOV. */
			float angleScale = .5f / zoomLevel;
			halfX = (tanf( (xr.views[0].fov.angleRight + xr.views[1].fov.angleRight) * angleScale ) -
					 tanf( (xr.views[0].fov.angleLeft + xr.views[1].fov.angleLeft) * angleScale )) *
					.5f;
			halfY = (tanf( (xr.views[0].fov.angleUp + xr.views[1].fov.angleUp) * angleScale ) -
					 tanf( (xr.views[0].fov.angleDown + xr.views[1].fov.angleDown) * angleScale )) *
					.5f * glConfig.vidWidth / glConfig.vidHeight;
		}
		fov[0] = -atanf( halfX );
		fov[1] = -fov[0];
		fov[2] = atanf( halfY );
		fov[3] = -fov[2];
		return qtrue;
	}
	if ( view->rdflags & RDF_NOWORLDMODEL ) {
		return qfalse;
	}
	if ( xr.scope ) {
		float horizontal = atanf( .935f ) / zoomLevel;
		float aspect = xr.target.width ? (float)xr.target.height / xr.target.width : 1;
		fov[0] = -horizontal;
		fov[1] = horizontal;
		fov[2] = atanf( tanf( horizontal ) * aspect );
		fov[3] = -fov[2];
		return qtrue;
	}
	pose = &xr.views[eye].pose;
	center = VKXR_CenterPose();
	// The module supplies center-head view origin/orientation. Convert only IPD
	// displacement here: OpenXR +X right/+Y up/-Z forward to Q3 forward/left/up.
	offset[0] = -(pose->position.z - (xr.views[0].pose.position.z + xr.views[1].pose.position.z) * 0.5f);
	offset[1] = -(pose->position.x - (xr.views[0].pose.position.x + xr.views[1].pose.position.x) * 0.5f);
	offset[2] = pose->position.y - (xr.views[0].pose.position.y + xr.views[1].pose.position.y) * 0.5f;
	// Positions above are in tracking space; convert using the center head's
	// inverse orientation before the game-view axes to avoid double yaw.
	{
		const XrQuaternionf *q = &center.orientation;
		float v[3] = {-offset[1], offset[2], -offset[0]}, cross[3], cross2[3];
		float inverse[3] = {-q->x, -q->y, -q->z};
		CrossProduct( inverse, v, cross );
		CrossProduct( inverse, cross, cross2 );
		for ( axis = 0; axis < 3; axis++ ) {
			v[axis] += 2 * (q->w * cross[axis] + cross2[axis]);
		}
		offset[0] = -v[2];
		offset[1] = -v[0];
		offset[2] = v[1];
	}
	// Cgame publishes its current camera scale before submitting this view.
	// Keep spectator/zoom policy there; eye separation is measured in meters.
	scale = worldscale ? worldscale->value : 32.0f;
	scale *= worldscaleScaler ? worldscaleScaler->value : 1.0f;
	for ( axis = 0; axis < 3; axis++ ) {
		VectorMA( view->vieworg, offset[axis] * scale, view->viewaxis[axis], view->vieworg );
	}
	// Account for canted eye rotations relative to the midpoint HMD orientation.
	{
		const XrQuaternionf *h = &center.orientation, *e = &pose->orientation;
		float relative[3] = {h->w * e->x - h->x * e->w - h->y * e->z + h->z * e->y,
							 h->w * e->y + h->x * e->z - h->y * e->w - h->z * e->x,
							 h->w * e->z - h->x * e->y + h->y * e->x - h->z * e->w};
		float scalar = h->w * e->w + h->x * e->x + h->y * e->y + h->z * e->z;
		vec3_t original[3];
		int component;
		Com_Memcpy( original, view->viewaxis, sizeof( original ) );
		for ( axis = 0; axis < 3; axis++ ) {
			float v[3] = {0, 0, 0}, cross[3], cross2[3], game[3];
			v[axis == 0 ? 2 : axis == 1 ? 0 : 1] = axis == 2 ? 1 : -1;
			CrossProduct( relative, v, cross );
			CrossProduct( relative, cross, cross2 );
			for ( component = 0; component < 3; component++ ) {
				v[component] += 2 * (scalar * cross[component] + cross2[component]);
			}
			game[0] = -v[2];
			game[1] = -v[0];
			game[2] = v[1];
			VectorClear( view->viewaxis[axis] );
			for ( component = 0; component < 3; component++ ) {
				VectorMA( view->viewaxis[axis], game[component], original[component], view->viewaxis[axis] );
			}
		}
	}
	fov[0] = xr.views[eye].fov.angleLeft;
	fov[1] = xr.views[eye].fov.angleRight;
	fov[2] = xr.views[eye].fov.angleUp;
	fov[3] = xr.views[eye].fov.angleDown;
	for ( axis = 0; axis < 4; axis++ ) {
		fov[axis] /= zoomLevel;
	}
	return qtrue;
}

/* Capture immutable eye poses on the frontend; deferred commands own a copy. */
void VK_XR_SetupView( refdef_t *view, viewParms_t *parms ) {
	int e;
	if ( !VK_XR_Drawing() || tr_hudDrawing )
		return;
	if ( screenGeometry.visible ) {
		parms->xrProjection = VK_XR_EyeView( 0, view, parms->xrFov );
		return;
	}
	if ( view->rdflags & RDF_NOWORLDMODEL )
		return;
	parms->xrMultiview = qtrue;
	for ( e = 0; e < 2; e++ ) {
		refdef_t eye = *view;
		VK_XR_EyeView( e, &eye, parms->eyeFov[e] );
		VectorCopy( eye.vieworg, parms->eyeOrigin[e] );
		VectorCopy( eye.vieworg, parms->eyePvsOrigin[e] );
		Com_Memcpy( parms->eyeAxis[e], eye.viewaxis, sizeof( eye.viewaxis ) );
	}
}

void VK_XR_FoveationCaps( qboolean supported ) {
	ri.Cvar_Set( "vr_foveationCaps", supported ? (gaze.supported ? "eyetracked" : "fixed") : "none" );
}

void VK_XR_FoveationMap( vkFovMap_t *map ) {
	float eyes[2][4], fov[2][4];
	XrPosef center = VKXR_CenterPose();
	int e, j;
	int mode = VK_FovMode( foveation ? foveation->integer : 0, 1, gaze.supported );
	for ( e = 0; e < 2; e++ ) {
		map->eye[e].x = 0;
		map->eye[e].width = map->width;
		map->eye[e].height = map->height;
		Com_Memcpy( eyes[e], &xr.views[e].pose.orientation, sizeof( eyes[e] ) );
		fov[e][0] = xr.views[e].fov.angleLeft;
		fov[e][1] = xr.views[e].fov.angleRight;
		fov[e][2] = xr.views[e].fov.angleUp;
		fov[e][3] = xr.views[e].fov.angleDown;
		if ( xr.scope ) {
			fov[e][1] = atanf( .935f ) / zoomLevel;
			fov[e][0] = -fov[e][1];
			fov[e][2] = atanf( tanf( fov[e][1] ) * (float)xr.target.height / xr.target.width );
			fov[e][3] = -fov[e][2];
		} else {
			for ( j = 0; j < 4; j++ ) {
				fov[e][j] /= zoomLevel;
			}
		}
	}
	VK_FovCenters( &foveationCenters, (float *)&center.orientation, eyes, fov, mode, screenGeometry.visible,
				   xr.scope, gazeDirection, gazeValid, xr.displayTime );
	Com_Memcpy( map->center, foveationCenters.center, sizeof( map->center ) );
	Com_Memcpy( map->tangent, foveationCenters.tangent, sizeof( map->tangent ) );
	map->eyeTracked = foveationCenters.eyeTracked;
	map->strength = mode && VK_XR_Drawing() && !screenGeometry.visible && foveationCenters.valid
						? MIN( 3, MAX( 1, foveationStrength ? foveationStrength->integer : 1 ) )
						: 0;

}
