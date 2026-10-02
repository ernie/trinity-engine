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
/* xrCreateInstance failures that no other request would get past */
static int VKXRLive_Unavailable( XrResult result ) {
	return result == XR_ERROR_RUNTIME_UNAVAILABLE || result == XR_ERROR_LIMIT_REACHED ||
		   result == XR_ERROR_OUT_OF_MEMORY || result == XR_ERROR_INSTANCE_LOST;
}
XrResult VK_XRLive_Open( vkXRLive_t *ctx ) {
	PFN_xrCreateInstance create;
	PFN_xrGetSystem getSystem;
	PFN_xrGetInstanceProperties properties;
	XrInstanceProperties ip;
	PFN_xrEnumerateInstanceExtensionProperties enumerate;
	XrExtensionProperties extensions[256];
	const char *enabled[10] = {"XR_KHR_vulkan_enable2"};
	XrInstanceCreateInfo ci;
	XrSystemGetInfo si;
	XrResult result;
	uint32_t count, i, without;
	int vulkan2 = 0, uuid = 0, renderModel = 0, interactionModel = 0, wantModels;
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
		if ( !strcmp( extensions[i].extensionName, XR_EXT_UUID_EXTENSION_NAME ) )
			uuid = 1;
		if ( !strcmp( extensions[i].extensionName, XR_EXT_RENDER_MODEL_EXTENSION_NAME ) )
			renderModel = 1;
		if ( !strcmp( extensions[i].extensionName, XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME ) )
			interactionModel = 1;
	}
	wantModels = renderModel && interactionModel;
	if ( !vulkan2 ) {
		result = XR_ERROR_EXTENSION_NOT_PRESENT;
		goto fail;
	}
	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
	strcpy( ci.applicationInfo.applicationName, "Trinity Engine" );
	strcpy( ci.applicationInfo.engineName, "Trinity Engine" );
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
	without = ci.enabledExtensionCount;
	/* OpenXR 1.1, or 1.0 from a runtime that turns 1.1 down, whatever code it uses to say so */
	for ( i = 0; i < 2; i++ ) {
		XrResult refused = XR_SUCCESS;
		ci.applicationInfo.apiVersion = i ? XR_MAKE_VERSION( 1, 0, 0 ) : XR_MAKE_VERSION( 1, 1, 0 );
		ci.enabledExtensionCount = without;
		if ( wantModels ) {
			/* render models need OpenXR 1.1 or this extension */
			if ( i && uuid )
				enabled[ci.enabledExtensionCount++] = XR_EXT_UUID_EXTENSION_NAME;
			enabled[ci.enabledExtensionCount++] = XR_EXT_RENDER_MODEL_EXTENSION_NAME;
			enabled[ci.enabledExtensionCount++] = XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME;
		}
		result = create( &ci, &ctx->instance );
		/* a runtime may refuse the extensions with any code */
		if ( XR_FAILED( result ) && wantModels && result != XR_ERROR_API_VERSION_UNSUPPORTED &&
			 !VKXRLive_Unavailable( result ) ) {
			refused = result;
			ci.enabledExtensionCount = without;
			result = create( &ci, &ctx->instance );
		}
		if ( XR_SUCCEEDED( result ) ) {
			ctx->models = wantModels && !refused;
			ctx->modelsRefused = refused;
			break;
		}
		if ( VKXRLive_Unavailable( result ) )
			break;
	}
	if ( XR_FAILED( result ) ) {
		ctx->instance = XR_NULL_HANDLE;
		goto fail;
	}
	ctx->apiVersion = ci.applicationInfo.apiVersion;
	memcpy( (void *)ctx->enabled, enabled, sizeof( ctx->enabled ) );
	ctx->enabledCount = ci.enabledExtensionCount;
	LIVE_PROC( ctx->instance, xrGetInstanceProperties, properties );
	memset( &ip, 0, sizeof( ip ) );
	ip.type = XR_TYPE_INSTANCE_PROPERTIES;
	result = properties( ctx->instance, &ip );
	if ( XR_FAILED( result ) )
		goto fail;
	memcpy( ctx->runtimeName, ip.runtimeName, sizeof( ctx->runtimeName ) );
	ctx->runtimeName[sizeof( ctx->runtimeName ) - 1] = 0;
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
