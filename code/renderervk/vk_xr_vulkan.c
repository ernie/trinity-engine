#include "../vrcommon/vr_float.h"
#include "vk_xr_vulkan.h"
#include "../vrcommon/vr_render_extent.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

void VK_XRVK_Sleep( unsigned msec ) {
#ifdef _WIN32
	Sleep( msec );
#else
	struct timespec delay;
	delay.tv_sec = msec / 1000;
	delay.tv_nsec = ( msec % 1000 ) * 1000000L;
	nanosleep( &delay, NULL );
#endif
}

static XrResult VKXR_Result( vkXRVk_t *ctx, XrResult result ) {
	if ( result == XR_ERROR_SESSION_LOST || result == XR_SESSION_LOSS_PENDING ||
		result == XR_ERROR_INSTANCE_LOST )
		ctx->lost = 1;
	return result;
}

XrResult VK_XRVK_Init( vkXRVk_t *ctx, XrInstance instance, XrSystemId system,
					   PFN_xrGetInstanceProcAddr getproc, int formatList ) {
	XrResult result;
	if ( !ctx || !instance || !system || !getproc )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( ctx, 0, sizeof( *ctx ) );
	ctx->instance = instance;
	ctx->system = system;
	ctx->formatList = formatList;
#define LOAD(name) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = getproc( instance, "xr" #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) \
			return XR_FAILED( result ) ? result : XR_ERROR_FUNCTION_UNSUPPORTED; \
		ctx->xr.name = (PFN_xr##name)fn; \
	} while ( 0 );
	VK_XRVK_FUNCTIONS( LOAD )
#undef LOAD
	ctx->requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
	return VKXR_Result( ctx,
						ctx->xr.GetVulkanGraphicsRequirements2KHR( instance, system, &ctx->requirements ) );
}

XrResult VK_XRVK_CreateInstance( vkXRVk_t *ctx, PFN_vkGetInstanceProcAddr proc,
								 const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *allocator,
								 VkInstance *instance, VkResult *result ) {
	XrVulkanInstanceCreateInfoKHR info;
	if ( !ctx || !ctx->xr.CreateVulkanInstanceKHR || !proc || !ci || !instance || !result )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( &info, 0, sizeof( info ) );
	info.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	info.systemId = ctx->system;
	info.pfnGetInstanceProcAddr = proc;
	info.vulkanCreateInfo = ci;
	info.vulkanAllocator = allocator;
	return VKXR_Result( ctx, ctx->xr.CreateVulkanInstanceKHR( ctx->instance, &info, instance, result ) );
}

XrResult VK_XRVK_GetPhysicalDevice( vkXRVk_t *ctx, VkInstance instance, VkPhysicalDevice *device ) {
	XrVulkanGraphicsDeviceGetInfoKHR info;
	if ( !ctx || !ctx->xr.GetVulkanGraphicsDevice2KHR || !instance || !device )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( &info, 0, sizeof( info ) );
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = ctx->system;
	info.vulkanInstance = instance;
	return VKXR_Result( ctx, ctx->xr.GetVulkanGraphicsDevice2KHR( ctx->instance, &info, device ) );
}

XrResult VK_XRVK_CreateDevice( vkXRVk_t *ctx, PFN_vkGetInstanceProcAddr proc, VkPhysicalDevice physical,
							   const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *allocator,
							   VkDevice *device, VkResult *result ) {
	XrVulkanDeviceCreateInfoKHR info;
	if ( !ctx || !ctx->xr.CreateVulkanDeviceKHR || !proc || !physical || !ci || !device || !result )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( &info, 0, sizeof( info ) );
	info.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	info.systemId = ctx->system;
	info.pfnGetInstanceProcAddr = proc;
	info.vulkanPhysicalDevice = physical;
	info.vulkanCreateInfo = ci;
	info.vulkanAllocator = allocator;
	return VKXR_Result( ctx, ctx->xr.CreateVulkanDeviceKHR( ctx->instance, &info, device, result ) );
}

int VK_XRVK_TargetsBusy( const vkXRVk_t *ctx ) {
	return !ctx || ctx->frameBegun || ctx->target.acquired || ctx->target.waited;
}

XrResult VK_XRVK_DestroyTarget( vkXRVk_t *ctx, vkXRVkTarget_t *target ) {
	XrResult result = XR_SUCCESS;
	if ( !ctx || !target )
		return XR_ERROR_VALIDATION_FAILURE;
	if ( VK_XRVK_TargetsBusy( ctx ) || target->acquired || target->waited )
		return XR_ERROR_CALL_ORDER_INVALID;
	if ( target->handle ) {
		result = ctx->xr.DestroySwapchain ? ctx->xr.DestroySwapchain( target->handle )
										  : XR_ERROR_FUNCTION_UNSUPPORTED;
		if ( result != XR_SUCCESS )
			return result;
	}
	memset( target, 0, sizeof( *target ) );
	if ( target == &ctx->target ) {
		ctx->renderable = 0;
		ctx->scope = 0;
	}
	return result;
}

static VkFormat VKXR_UnormTwin( VkFormat format ) {
	switch ( format ) {
		case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
		default: return format;
	}
}

XrResult VK_XRVK_CreateTarget( vkXRVk_t *ctx, vkXRVkTarget_t *target ) {
	XrViewConfigurationView views[2];
	XrSwapchainCreateInfo ci;
	XrVulkanSwapchainFormatListCreateInfoKHR formats;
	XrVulkanSwapchainCreateInfoMETA meta;
	VkFormat viewFormats[2];
	uint32_t i, count, width[2], height[2];
	XrResult result;
	if ( !ctx || !ctx->session || !target || ctx->format == VK_FORMAT_UNDEFINED )
		return XR_ERROR_VALIDATION_FAILURE;
	if ( VK_XRVK_TargetsBusy( ctx ) || target->handle )
		return XR_ERROR_CALL_ORDER_INVALID;
	if ( ctx->lost )
		return XR_ERROR_SESSION_LOST;
#define CHECK(call) \
	do { \
		result = (call); \
		if ( result != XR_SUCCESS ) \
			goto fail; \
	} while ( 0 )
	memset( target, 0, sizeof( *target ) );
	memset( views, 0, sizeof( views ) );
	views[0].type = views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	CHECK( ctx->xr.EnumerateViewConfigurationViews(
		ctx->instance, ctx->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views ) );
	if ( count != 2 ) {
		result = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		goto fail;
	}
	for ( i = 0; i < 2; i++ ) {
		uint32_t maxWidth = views[i].maxImageRectWidth, maxHeight = views[i].maxImageRectHeight;
		if ( !maxWidth ) {
			maxWidth = views[i].recommendedImageRectWidth;
		}
		if ( !maxHeight ) {
			maxHeight = views[i].recommendedImageRectHeight;
		}
		if ( ctx->maxEyeWidth && ctx->maxEyeWidth < maxWidth ) {
			maxWidth = ctx->maxEyeWidth;
		}
		if ( ctx->maxEyeHeight && ctx->maxEyeHeight < maxHeight ) {
			maxHeight = ctx->maxEyeHeight;
		}
		if ( !VR_RenderExtent( views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight,
							  maxWidth, maxHeight, ctx->renderScale, &width[i], &height[i] ) ) {
			result = XR_ERROR_VALIDATION_FAILURE;
			goto fail;
		}
	}
	target->width = width[0] > width[1] ? width[0] : width[1];
	target->height = height[0] > height[1] ? height[0] : height[1];
	if ( !target->width || !target->height || target->width > 0x7fffffff || target->height > 0x7fffffff ) {
		result = XR_ERROR_VALIDATION_FAILURE;
		goto fail;
	}
	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = VK_XRVK_TARGET_USAGE;
	ci.format = ctx->format;
	ci.sampleCount = 1;
	ci.width = target->width;
	ci.height = target->height;
	ci.faceCount = 1;
	ci.arraySize = 2;
	ci.mipCount = 1;
	if ( ctx->formatList ) {
		viewFormats[0] = ctx->format;
		viewFormats[1] = VKXR_UnormTwin( ctx->format );
		memset( &formats, 0, sizeof( formats ) );
		formats.type = XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR;
		formats.viewFormatCount = 2;
		formats.viewFormats = viewFormats;
		ci.next = &formats;
	}
	if ( ctx->createInfoMeta && ctx->targetCreateFlags ) {
		memset( &meta, 0, sizeof( meta ) );
		meta.type = XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META;
		meta.next = ci.next;
		meta.additionalCreateFlags = ctx->targetCreateFlags;
		ci.next = &meta;
		if ( ctx->xr.CreateSwapchain( ctx->session, &ci, &target->handle ) == XR_SUCCESS )
			target->createFlags = ctx->targetCreateFlags;
		else {
			target->handle = XR_NULL_HANDLE;
			ci.next = meta.next; // the flags are optional; retry without them
		}
	}
	if ( !target->handle )
		CHECK( ctx->xr.CreateSwapchain( ctx->session, &ci, &target->handle ) );
	CHECK( ctx->xr.EnumerateSwapchainImages( target->handle, 0, &target->count, NULL ) );
	if ( !target->count || target->count > VK_XRVK_MAX_IMAGES ) {
		result = XR_ERROR_LIMIT_REACHED;
		goto fail;
	}
	for ( i = 0; i < target->count; i++ )
		target->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
	CHECK( ctx->xr.EnumerateSwapchainImages( target->handle, target->count, &target->count,
											 (XrSwapchainImageBaseHeader *)target->images ) );
	return XR_SUCCESS;
fail:
	VK_XRVK_DestroyTarget( ctx, target );
	return VKXR_Result( ctx, result );
#undef CHECK
}
XrResult VK_XRVK_Bind( vkXRVk_t *ctx, const XrGraphicsBindingVulkan2KHR *binding, const VkFormat *formats,
					   uint32_t formatCount ) {
	XrSessionCreateInfo sessionInfo;
	XrReferenceSpaceCreateInfo spaceInfo;
	XrViewConfigurationView views[2];
	XrEnvironmentBlendMode blends[16];
	int64_t supported[128];
	uint32_t count, i, j;
	XrResult result;
	VkPhysicalDevice required;
	if ( !ctx || !ctx->xr.CreateSession || !binding || !formats || !formatCount || ctx->session )
		return XR_ERROR_VALIDATION_FAILURE;
	result = VK_XRVK_GetPhysicalDevice( ctx, binding->instance, &required );
	if ( XR_FAILED( result ) )
		return result;
	if ( required != binding->physicalDevice )
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
#define CHECK(call) \
	do { \
		result = (call); \
		if ( result != XR_SUCCESS ) \
			goto fail; \
	} while ( 0 )
	memset( views, 0, sizeof( views ) );
	views[0].type = views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	CHECK( ctx->xr.EnumerateViewConfigurationViews(
		ctx->instance, ctx->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views ) );
	if ( count != 2 ) {
		result = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		goto fail;
	}
	CHECK( ctx->xr.EnumerateEnvironmentBlendModes(
		ctx->instance, ctx->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 16, &count, blends ) );
	if ( count > 16 ) {
		result = XR_ERROR_LIMIT_REACHED;
		goto fail;
	}
	ctx->blend = XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM;
	for ( i = 0; i < count; i++ )
		if ( blends[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE )
			ctx->blend = blends[i];
	if ( ctx->blend != XR_ENVIRONMENT_BLEND_MODE_OPAQUE ) {
		result = XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
		goto fail;
	}
	memset( &sessionInfo, 0, sizeof( sessionInfo ) );
	sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	sessionInfo.next = binding;
	sessionInfo.systemId = ctx->system;
	CHECK( ctx->xr.CreateSession( ctx->instance, &sessionInfo, &ctx->session ) );
	memset( &spaceInfo, 0, sizeof( spaceInfo ) );
	spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	/* STAGE gives floor-relative meters; LOCAL falls back with the 1.675 m default eye height. */
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	spaceInfo.poseInReferenceSpace.orientation.w = 1;
	result = ctx->xr.CreateReferenceSpace( ctx->session, &spaceInfo, &ctx->space );
	if ( result == XR_ERROR_REFERENCE_SPACE_UNSUPPORTED ) {
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaceInfo.poseInReferenceSpace.position.y = -1.675f;
		CHECK( ctx->xr.CreateReferenceSpace( ctx->session, &spaceInfo, &ctx->space ) );
	} else if ( result != XR_SUCCESS )
		goto fail;
	ctx->spaceType = spaceInfo.referenceSpaceType;
	ctx->referenceSpaceChanged = 1;
	ctx->referenceSpaceChangeTime = 0;
	memset( &spaceInfo, 0, sizeof( spaceInfo ) );
	spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	spaceInfo.poseInReferenceSpace.orientation.w = 1;
	CHECK( ctx->xr.CreateReferenceSpace( ctx->session, &spaceInfo, &ctx->viewSpace ) );
	CHECK( ctx->xr.EnumerateSwapchainFormats( ctx->session, 128, &count, supported ) );
	if ( count > 128 ) {
		result = XR_ERROR_LIMIT_REACHED;
		goto fail;
	}
	ctx->format = VK_FORMAT_UNDEFINED;
	for ( i = 0; i < formatCount && ctx->format == VK_FORMAT_UNDEFINED; i++ )
		for ( j = 0; j < count; j++ )
			if ( (int64_t)formats[i] == supported[j] ) {
				ctx->format = formats[i];
				break;
			}
	if ( ctx->format == VK_FORMAT_UNDEFINED ) {
		result = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
		goto fail;
	}
	CHECK( VK_XRVK_CreateTarget( ctx, &ctx->target ) );
	return XR_SUCCESS;
fail:
	VKXR_Result( ctx, result );
	VK_XRVK_Shutdown( ctx );
	return result;
#undef CHECK
}

int VK_XRVK_ConsumeSpaceChange( vkXRVk_t *ctx ) {
	if ( !ctx->renderable || !ctx->referenceSpaceChanged || ctx->displayTime < ctx->referenceSpaceChangeTime )
		return 0;
	ctx->referenceSpaceChanged = 0;
	return 1;
}

XrResult VK_XRVK_Poll( vkXRVk_t *ctx ) {
	unsigned i;
	XrResult result;
	if ( !ctx || !ctx->xr.PollEvent || !ctx->session )
		return XR_ERROR_HANDLE_INVALID;
	if ( ctx->frameBegun )
		return XR_ERROR_CALL_ORDER_INVALID;
	for ( i = 0; i < 64; i++ ) {
		XrEventDataBuffer event;
		memset( &event, 0, sizeof( event ) );
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		result = ctx->xr.PollEvent( ctx->instance, &event );
		if ( result == XR_EVENT_UNAVAILABLE )
			return XR_SUCCESS;
		if ( XR_FAILED( result ) )
			return VKXR_Result( ctx, result );
		if ( event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING ) {
			ctx->lost = 1;
			return XR_SESSION_LOSS_PENDING;
		}
		if ( event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING ) {
			const XrEventDataReferenceSpaceChangePending *change = (const void *)&event;
			if ( change->session == ctx->session && change->referenceSpaceType == ctx->spaceType ) {
				ctx->referenceSpaceChanged = 1;
				ctx->referenceSpaceChangeTime = change->changeTime;
			}
		}
		if ( event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED )
			ctx->profileChanged = 1;
		if ( event.type == XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT )
			ctx->modelsChanged = 1;
		if ( event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED ) {
			XrEventDataSessionStateChanged *state = (XrEventDataSessionStateChanged *)&event;
			if ( state->session != ctx->session )
				continue;
			ctx->state = state->state;
			if ( state->state == XR_SESSION_STATE_READY && !ctx->running ) {
				XrSessionBeginInfo begin;
				memset( &begin, 0, sizeof( begin ) );
				begin.type = XR_TYPE_SESSION_BEGIN_INFO;
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				result = ctx->xr.BeginSession( ctx->session, &begin );
				VKXR_Result( ctx, result );
				if ( XR_FAILED( result ) )
					return VKXR_Result( ctx, result );
				ctx->running = 1;
			} else if ( state->state == XR_SESSION_STATE_STOPPING && ctx->running ) {
				result = ctx->xr.EndSession( ctx->session );
				VKXR_Result( ctx, result );
				ctx->running = 0;
				if ( XR_FAILED( result ) )
					return VKXR_Result( ctx, result );
			} else if ( state->state == XR_SESSION_STATE_EXITING ||
					   state->state == XR_SESSION_STATE_LOSS_PENDING ) {
				ctx->lost = 1;
				return XR_SESSION_LOSS_PENDING;
			}
		}
	}
	return XR_SUCCESS;
}

XrResult VK_XRVK_Begin( vkXRVk_t *ctx ) {
	XrFrameWaitInfo wait;
	XrFrameBeginInfo begin;
	XrFrameState frame;
	XrViewLocateInfo locate;
	XrViewState state;
	XrSwapchainImageAcquireInfo acquire;
	XrSwapchainImageWaitInfo imageWait;
	XrResult result;
	uint32_t count, i;
	if ( !ctx || !ctx->session )
		return XR_ERROR_HANDLE_INVALID;
	if ( ctx->frameBegun )
		return XR_ERROR_CALL_ORDER_INVALID;
	if ( ctx->lost )
		return XR_ERROR_SESSION_LOST;
	if ( !ctx->running || !ctx->target.handle )
		return XR_SESSION_NOT_FOCUSED;
	ctx->renderable = 0;
	memset( &wait, 0, sizeof( wait ) );
	wait.type = XR_TYPE_FRAME_WAIT_INFO;
	memset( &frame, 0, sizeof( frame ) );
	frame.type = XR_TYPE_FRAME_STATE;
	result = ctx->xr.WaitFrame( ctx->session, &wait, &frame );
	if ( result == XR_SESSION_LOSS_PENDING )
		return VKXR_Result( ctx, result );
	if ( XR_FAILED( result ) )
		return VKXR_Result( ctx, result );
	memset( &begin, 0, sizeof( begin ) );
	begin.type = XR_TYPE_FRAME_BEGIN_INFO;
	result = ctx->xr.BeginFrame( ctx->session, &begin );
	if ( XR_FAILED( result ) )
		return VKXR_Result( ctx, result );
	ctx->frameBegun = 1;
	ctx->displayTime = frame.predictedDisplayTime;
	ctx->displayPeriod = frame.predictedDisplayPeriod;
	if ( result == XR_SESSION_LOSS_PENDING )
		return VKXR_Result( ctx, result );
	if ( !frame.shouldRender || !ctx->target.handle )
		return XR_SUCCESS;
	memset( &locate, 0, sizeof( locate ) );
	locate.type = XR_TYPE_VIEW_LOCATE_INFO;
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = ctx->displayTime;
	locate.space = ctx->space;
	memset( &state, 0, sizeof( state ) );
	state.type = XR_TYPE_VIEW_STATE;
	memset( ctx->views, 0, sizeof( ctx->views ) );
	ctx->views[0].type = ctx->views[1].type = XR_TYPE_VIEW;
	result = ctx->xr.LocateViews( ctx->session, &locate, &state, 2, &count, ctx->views );
	if ( result == XR_SESSION_LOSS_PENDING )
		return VKXR_Result( ctx, result );
	if ( XR_FAILED( result ) )
		return VKXR_Result( ctx, result );
	if ( count != 2 ||
		(state.viewStateFlags & (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT)) !=
			(XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT) )
		return XR_SUCCESS;
	for ( i = 0; i < 2; i++ ) {
		float pose[7], fov[4], norm;
		memcpy( pose, &ctx->views[i].pose, sizeof( pose ) );
		memcpy( fov, &ctx->views[i].fov, sizeof( fov ) );
		if ( !VR_FloatsFinite( pose, 7 ) || !VR_FloatsFinite( fov, 4 ) )
			return XR_SUCCESS;
		norm = pose[0] * pose[0] + pose[1] * pose[1] + pose[2] * pose[2] + pose[3] * pose[3];
		if ( !VR_FloatFinite( norm ) || norm < .000001f || fov[0] <= -1.5707963f || fov[1] >= 1.5707963f ||
			fov[0] >= fov[1] || fov[3] <= -1.5707963f || fov[2] >= 1.5707963f || fov[3] >= fov[2] )
			return XR_SUCCESS;
	}
	memset( &acquire, 0, sizeof( acquire ) );
	acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
	result = ctx->xr.AcquireSwapchainImage( ctx->target.handle, &acquire, &ctx->target.index );
	if ( XR_FAILED( result ) )
		return VKXR_Result( ctx, result );
	ctx->target.acquired = 1;
	if ( result == XR_SESSION_LOSS_PENDING )
		return VKXR_Result( ctx, result );
	memset( &imageWait, 0, sizeof( imageWait ) );
	imageWait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	imageWait.timeout = 100000000; /* 100 ms; no infinite swapchain wait. */
	result = ctx->xr.WaitSwapchainImage( ctx->target.handle, &imageWait );
	if ( result != XR_SUCCESS ) {
		ctx->lost = 1;
		return result;
	}
	ctx->target.waited = 1;
	if ( ctx->target.index >= ctx->target.count ) {
		ctx->lost = 1;
		return XR_ERROR_RUNTIME_FAILURE;
	}
	ctx->renderable = 1;
	return XR_SUCCESS;
}

XrResult VK_XRVK_End( vkXRVk_t *ctx, int rendered ) {
	XrCompositionLayerProjectionView views[2];
	XrCompositionLayerProjection projection;
	XrCompositionLayerQuad scope;
	const XrCompositionLayerBaseHeader *layer;
	XrFrameEndInfo end;
	XrResult result, first = XR_SUCCESS;
	unsigned i;
	if ( !ctx || !ctx->frameBegun )
		return XR_ERROR_CALL_ORDER_INVALID;
	if ( ctx->target.acquired && ctx->target.waited ) {
		XrSwapchainImageReleaseInfo release;
		memset( &release, 0, sizeof( release ) );
		release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
		result = ctx->xr.ReleaseSwapchainImage( ctx->target.handle, &release );
		VKXR_Result( ctx, result );
		if ( XR_FAILED( result ) ) {
			first = result;
			ctx->lost = 1;
		} else
			ctx->target.acquired = ctx->target.waited = 0;
	}
	memset( views, 0, sizeof( views ) );
	for ( i = 0; i < 2; i++ ) {
		views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		views[i].pose = ctx->views[i].pose;
		views[i].fov = ctx->views[i].fov;
		views[i].subImage.swapchain = ctx->target.handle;
		views[i].subImage.imageArrayIndex = i;
		views[i].subImage.imageRect.offset.x = views[i].subImage.imageRect.offset.y = 0;
		views[i].subImage.imageRect.extent.width = ctx->target.width;
		views[i].subImage.imageRect.extent.height = ctx->target.height;
	}
	memset( &projection, 0, sizeof( projection ) );
	projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	projection.space = ctx->space;
	projection.viewCount = 2;
	projection.views = views;
	layer = (const XrCompositionLayerBaseHeader *)&projection;
	if ( ctx->scope && ctx->viewSpace && ctx->target.width ) {
		memset( &scope, 0, sizeof( scope ) );
		scope.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		scope.space = ctx->viewSpace;
		scope.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		scope.subImage = views[0].subImage;
		scope.pose.orientation.w = 1;
		scope.pose.position.z = -1;
		scope.size.width = 1.87f;
		scope.size.height = 1.87f * ctx->target.height / ctx->target.width;
		layer = (const XrCompositionLayerBaseHeader *)&scope;
	}
	memset( &end, 0, sizeof( end ) );
	end.type = XR_TYPE_FRAME_END_INFO;
	end.displayTime = ctx->displayTime;
	end.environmentBlendMode = ctx->blend;
	if ( rendered && ctx->renderable && !ctx->lost && XR_SUCCEEDED( first ) ) {
		end.layerCount = 1;
		end.layers = &layer;
	}
	result = ctx->xr.EndFrame( ctx->session, &end );
	ctx->frameBegun = ctx->renderable = 0;
	return VKXR_Result( ctx, XR_FAILED( first ) ? first : result );
}

void VK_XRVK_Shutdown( vkXRVk_t *ctx ) {
	if ( !ctx )
		return;
	if ( ctx->frameBegun )
		VK_XRVK_End( ctx, 0 );
	if ( ctx->running && !ctx->lost && ctx->xr.RequestExitSession ) {
		int spins;
		ctx->xr.RequestExitSession( ctx->session );
		/* The poll ends the session and clears running at the STOPPING state. */
		for ( spins = 0; spins < 100 && ctx->running && !ctx->lost; spins++ ) {
			VK_XRVK_Sleep( 1 ); /* leaves the runtime a slice to queue the events the next poll consumes */
			VK_XRVK_Poll( ctx );
		}
	}
	if ( ctx->target.handle && ctx->xr.DestroySwapchain )
		ctx->xr.DestroySwapchain( ctx->target.handle );
	memset( &ctx->target, 0, sizeof( ctx->target ) );
	if ( ctx->space && ctx->xr.DestroySpace )
		ctx->xr.DestroySpace( ctx->space );
	if ( ctx->viewSpace && ctx->xr.DestroySpace )
		ctx->xr.DestroySpace( ctx->viewSpace );
	if ( ctx->session && ctx->xr.DestroySession )
		ctx->xr.DestroySession( ctx->session );
	ctx->session = XR_NULL_HANDLE;
	ctx->space = XR_NULL_HANDLE;
	ctx->viewSpace = XR_NULL_HANDLE;
	ctx->scope = 0;
	ctx->running = ctx->frameBegun = ctx->renderable = 0;
	ctx->state = XR_SESSION_STATE_UNKNOWN;
}
