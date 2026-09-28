#include "vk_xr_live.h"
#include <string.h>
void VK_XRLive_Close( vkXRLive_t *ctx ) {
	if ( !ctx )
		return;
	if ( ctx->instance && ctx->destroy )
		ctx->destroy( ctx->instance );
	if ( ctx->library )
		XRLoader_UnloadLibrary( ctx->library );
	memset( ctx, 0, sizeof( *ctx ) );
}
XrResult VK_XRLive_Open( vkXRLive_t *ctx ) {
	PFN_xrCreateInstance create;
	PFN_xrGetSystem getSystem;
	PFN_xrEnumerateInstanceExtensionProperties enumerate;
	XrExtensionProperties extensions[256];
	const char *enabled[7] = {"XR_KHR_vulkan_enable2", NULL, NULL, NULL, NULL, NULL, NULL};
	XrInstanceCreateInfo ci;
	XrSystemGetInfo si;
	XrResult result;
	uint32_t count, i;
	int vulkan2 = 0;
	if ( !ctx )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( ctx, 0, sizeof( *ctx ) );
	ctx->library = XRLoader_LoadLibrary();
	if ( !ctx->library )
		return XR_ERROR_RUNTIME_UNAVAILABLE;
	ctx->getproc = (PFN_xrGetInstanceProcAddr)XRLoader_LibrarySymbol( ctx->library, "xrGetInstanceProcAddr" );
	/* Resolve exported destroy before creating anything so failed dispatch
	 * resolution can never strand an instance in a partially loaded backend. */
	ctx->destroy = (PFN_xrDestroyInstance)XRLoader_LibrarySymbol( ctx->library, "xrDestroyInstance" );
	if ( !ctx->getproc || !ctx->destroy ) {
		result = XR_ERROR_FUNCTION_UNSUPPORTED;
		goto fail;
	}
#define LIVE_PROC(handle, name, out) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = ctx->getproc( handle, #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) { \
			if ( !fn && XR_SUCCEEDED( result ) ) \
				result = XR_ERROR_FUNCTION_UNSUPPORTED; \
			goto fail; \
		} \
		out = (PFN_##name)fn; \
	} while ( 0 )
	LIVE_PROC( XR_NULL_HANDLE, xrCreateInstance, create );
	LIVE_PROC( XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties, enumerate );
	memset( extensions, 0, sizeof( extensions ) );
	for ( i = 0; i < 256; i++ )
		extensions[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	result = enumerate( NULL, 256, &count, extensions );
	if ( XR_FAILED( result ) )
		goto fail;
	if ( count > 256 ) {
		result = XR_ERROR_LIMIT_REACHED;
		goto fail;
	}
	for ( i = 0; i < count; i++ ) {
		extensions[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = 0;
		if ( !strcmp( extensions[i].extensionName, enabled[0] ) )
			vulkan2 = 1;
		if ( !strcmp( extensions[i].extensionName, "XR_BD_controller_interaction" ) )
			ctx->picoInteraction = 1;
		if ( !strcmp( extensions[i].extensionName, "XR_FB_display_refresh_rate" ) )
			ctx->displayRefresh = 1;
		if ( !strcmp( extensions[i].extensionName, "XR_EXT_eye_gaze_interaction" ) )
			ctx->eyeGaze = 1;
		if ( !strcmp( extensions[i].extensionName, "XR_KHR_vulkan_swapchain_format_list" ) )
			ctx->formatList = 1;
		if ( !strcmp( extensions[i].extensionName, "XR_VALVE_frame_controller_interaction" ) )
			ctx->frameInteraction = 1;
		if ( !strcmp( extensions[i].extensionName, "XR_META_vulkan_swapchain_create_info" ) )
			ctx->createInfoMeta = 1;
	}
	if ( !vulkan2 ) {
		result = XR_ERROR_EXTENSION_NOT_PRESENT;
		goto fail;
	}
	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
	strcpy( ci.applicationInfo.applicationName, "Trinity Engine" );
	strcpy( ci.applicationInfo.engineName, "Trinity Engine" );
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION( 1, 0, 0 );
	ci.enabledExtensionCount = 1;
	if ( ctx->picoInteraction )
		enabled[ci.enabledExtensionCount++] = "XR_BD_controller_interaction";
	if ( ctx->displayRefresh )
		enabled[ci.enabledExtensionCount++] = "XR_FB_display_refresh_rate";
	if ( ctx->eyeGaze )
		enabled[ci.enabledExtensionCount++] = "XR_EXT_eye_gaze_interaction";
	if ( ctx->formatList )
		enabled[ci.enabledExtensionCount++] = "XR_KHR_vulkan_swapchain_format_list";
	if ( ctx->frameInteraction )
		enabled[ci.enabledExtensionCount++] = "XR_VALVE_frame_controller_interaction";
	if ( ctx->createInfoMeta )
		enabled[ci.enabledExtensionCount++] = "XR_META_vulkan_swapchain_create_info";
	ci.enabledExtensionNames = enabled;
	memcpy( (void *)ctx->enabled, enabled, sizeof( ctx->enabled ) );
	ctx->enabledCount = ci.enabledExtensionCount;
	result = create( &ci, &ctx->instance );
	if ( XR_FAILED( result ) )
		goto fail;
	LIVE_PROC( ctx->instance, xrGetSystem, getSystem );
	memset( &si, 0, sizeof( si ) );
	si.type = XR_TYPE_SYSTEM_GET_INFO;
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	result = getSystem( ctx->instance, &si, &ctx->system );
	if ( XR_FAILED( result ) )
		goto fail;
	return XR_SUCCESS;
fail:
	VK_XRLive_Close( ctx );
	return result;
#undef LIVE_PROC
}
