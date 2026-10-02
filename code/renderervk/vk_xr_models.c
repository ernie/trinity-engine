#include "vk_xr_models.h"
#include "../vrcommon/vr_float.h"
#include <stdio.h>
#include <string.h>

#define XRM_MAX_IDS 32
#define XRM_MAX_ANIMATABLE 256
#define XRM_RETRY_UPDATES 120
#define XRM_RETRIES 5

/* node names pass to the model as plain fixed-size strings */
typedef char xrmNameCheck[sizeof( XrRenderModelAssetNodePropertiesEXT ) == VR_MODEL_NAME_SIZE ? 1 : -1];
typedef char xrmCacheCheck[sizeof( XrUuidEXT ) == sizeof( ( (vkXRModel_t *)0 )->cacheId ) ? 1 : -1];

XrResult VK_XRModels_Init( vkXRModels_t *ctx, XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr proc,
						   int enabled, vrModelAlloc_t alloc, vrModelFree_t release, void ( *log )( const char *line ) ) {
	XrResult result;
	if ( !ctx )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( ctx, 0, sizeof( *ctx ) );
	if ( !enabled )
		return XR_SUCCESS;
	if ( !instance || !session || !proc || !alloc || !release )
		return XR_ERROR_VALIDATION_FAILURE;
#define LOAD(out, name) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = proc( instance, #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) { \
			memset( ctx, 0, sizeof( *ctx ) ); \
			return XR_FAILED( result ) ? result : XR_ERROR_FUNCTION_UNSUPPORTED; \
		} \
		ctx->out = (PFN_##name)fn; \
	} while ( 0 )
	LOAD( enumerate, xrEnumerateInteractionRenderModelIdsEXT );
	LOAD( create, xrCreateRenderModelEXT );
	LOAD( destroy, xrDestroyRenderModelEXT );
	LOAD( properties, xrGetRenderModelPropertiesEXT );
	LOAD( createSpace, xrCreateRenderModelSpaceEXT );
	LOAD( createAsset, xrCreateRenderModelAssetEXT );
	LOAD( destroyAsset, xrDestroyRenderModelAssetEXT );
	LOAD( assetData, xrGetRenderModelAssetDataEXT );
	LOAD( assetProperties, xrGetRenderModelAssetPropertiesEXT );
	LOAD( state, xrGetRenderModelStateEXT );
	LOAD( locate, xrLocateSpace );
	LOAD( destroySpace, xrDestroySpace );
#undef LOAD
	ctx->session = session;
	ctx->alloc = alloc;
	ctx->release = release;
	ctx->log = log;
	/* the changed event is the cue to enumerate; this covers a runtime that never sends it */
	ctx->retry = XRM_RETRY_UPDATES;
	ctx->retries = XRM_RETRIES;
	return XR_SUCCESS;
}

static void XRM_Drop( vkXRModels_t *ctx, vkXRModel_t *slot ) {
	if ( slot->space )
		ctx->destroySpace( slot->space );
	if ( slot->handle )
		ctx->destroy( slot->handle );
	VR_ModelFree( &slot->model, ctx->release );
	if ( slot->map )
		ctx->release( slot->map );
	if ( slot->raw )
		ctx->release( slot->raw );
	if ( slot->states )
		ctx->release( slot->states );
	memset( slot, 0, sizeof( *slot ) );
}

/* The asset's bytes and the names of its animatable nodes, through a short-lived asset handle. */
static XrResult XRM_Fetch( vkXRModels_t *ctx, const XrUuidEXT *cacheId, int nodeCount, unsigned char **bytes,
						   uint32_t *size, XrRenderModelAssetNodePropertiesEXT *names ) {
	XrRenderModelAssetCreateInfoEXT assetInfo;
	XrRenderModelAssetDataGetInfoEXT dataInfo;
	XrRenderModelAssetDataEXT data;
	XrRenderModelAssetEXT asset = XR_NULL_HANDLE;
	XrResult result;
	*bytes = NULL;
	memset( &assetInfo, 0, sizeof( assetInfo ) );
	assetInfo.type = XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT;
	assetInfo.cacheId = *cacheId;
	result = ctx->createAsset( ctx->session, &assetInfo, &asset );
	if ( result != XR_SUCCESS )
		return XR_FAILED( result ) ? result : XR_ERROR_RUNTIME_FAILURE;
	memset( &dataInfo, 0, sizeof( dataInfo ) );
	dataInfo.type = XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT;
	memset( &data, 0, sizeof( data ) );
	data.type = XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT;
	result = ctx->assetData( asset, &dataInfo, &data );
	if ( result == XR_SUCCESS && ( !data.bufferCountOutput || data.bufferCountOutput > VK_XR_MODEL_MAX_BYTES ) )
		result = XR_ERROR_SIZE_INSUFFICIENT;
	if ( result == XR_SUCCESS ) {
		*bytes = ctx->alloc( data.bufferCountOutput );
		if ( !*bytes )
			result = XR_ERROR_OUT_OF_MEMORY;
	}
	if ( result == XR_SUCCESS ) {
		data.bufferCapacityInput = data.bufferCountOutput;
		data.buffer = *bytes;
		result = ctx->assetData( asset, &dataInfo, &data );
		*size = data.bufferCountOutput;
	}
	if ( result == XR_SUCCESS && nodeCount ) {
		XrRenderModelAssetPropertiesGetInfoEXT info;
		XrRenderModelAssetPropertiesEXT properties;
		memset( &info, 0, sizeof( info ) );
		info.type = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT;
		memset( &properties, 0, sizeof( properties ) );
		properties.type = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT;
		properties.nodePropertyCount = (uint32_t)nodeCount;
		properties.nodeProperties = names;
		result = ctx->assetProperties( asset, &info, &properties );
	}
	ctx->destroyAsset( asset );
	if ( result != XR_SUCCESS && *bytes ) {
		ctx->release( *bytes );
		*bytes = NULL;
	}
	return XR_FAILED( result ) ? result : result == XR_SUCCESS ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

static void XRM_Log( vkXRModels_t *ctx, const char *format, int a, int b ) {
	char line[128];
	if ( !ctx->log )
		return;
	snprintf( line, sizeof( line ), format, a, b );
	ctx->log( line );
}

/* 1 when the slot now holds the model, 0 when the asset can't be used, -1 when the runtime doesn't have it yet. */
static int XRM_Load( vkXRModels_t *ctx, vkXRModel_t *slot, XrRenderModelIdEXT id ) {
	XrRenderModelCreateInfoEXT createInfo;
	XrRenderModelPropertiesGetInfoEXT propertiesInfo;
	XrRenderModelPropertiesEXT properties;
	XrRenderModelSpaceCreateInfoEXT spaceInfo;
	XrRenderModelAssetNodePropertiesEXT *names = NULL;
	unsigned char *bytes = NULL;
	uint32_t size = 0;
	XrResult result;
	int i, outcome = 0;
	memset( slot, 0, sizeof( *slot ) );
	/* No glTF extensions are offered, so the runtime has to hand over a core glTF asset. */
	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT;
	createInfo.renderModelId = id;
	result = ctx->create( ctx->session, &createInfo, &slot->handle );
	if ( result != XR_SUCCESS || !slot->handle ) {
		slot->handle = XR_NULL_HANDLE;
		XRM_Log( ctx, "OpenXR controller model refused: the runtime would not create it (%d)", (int)result, 0 );
		return 0;
	}
	slot->id = id;
	memset( &propertiesInfo, 0, sizeof( propertiesInfo ) );
	propertiesInfo.type = XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT;
	memset( &properties, 0, sizeof( properties ) );
	properties.type = XR_TYPE_RENDER_MODEL_PROPERTIES_EXT;
	result = ctx->properties( slot->handle, &propertiesInfo, &properties );
	if ( result != XR_SUCCESS || properties.animatableNodeCount > XRM_MAX_ANIMATABLE ) {
		XRM_Log( ctx, "OpenXR controller model refused: properties unreadable (%d) or %d moving parts",
				 (int)result, (int)properties.animatableNodeCount );
		goto fail;
	}
	memcpy( slot->cacheId, &properties.cacheId, sizeof( slot->cacheId ) );
	slot->nodeCount = (int)properties.animatableNodeCount;
	if ( slot->nodeCount ) {
		names = ctx->alloc( (size_t)slot->nodeCount * sizeof( *names ) );
		slot->map = ctx->alloc( (size_t)slot->nodeCount * sizeof( int ) );
		slot->raw = ctx->alloc( (size_t)slot->nodeCount * sizeof( *slot->raw ) );
		slot->states = ctx->alloc( (size_t)slot->nodeCount * sizeof( *slot->states ) );
		if ( !names || !slot->map || !slot->raw || !slot->states )
			goto fail;
		memset( names, 0, (size_t)slot->nodeCount * sizeof( *names ) );
	}
	result = XRM_Fetch( ctx, &properties.cacheId, slot->nodeCount, &bytes, &size, names );
	if ( result != XR_SUCCESS ) {
		/* the handle can exist before the runtime has the file */
		if ( result == XR_ERROR_RENDER_MODEL_ASSET_UNAVAILABLE_EXT )
			outcome = -1;
		else
			XRM_Log( ctx, "OpenXR controller model refused: the asset could not be read (%d)", (int)result, 0 );
		goto fail;
	}
	if ( !VR_ModelParse( bytes, size, ctx->alloc, ctx->release, &slot->model ) ) {
		XRM_Log( ctx, "OpenXR controller model refused: its %d-byte file is malformed or over %d triangles", (int)size,
				 VR_MODEL_MAX_TRIANGLES );
		goto fail;
	}
	for ( i = 0; i < slot->nodeCount; i++ )
		names[i].uniqueName[XR_MAX_RENDER_MODEL_ASSET_NODE_NAME_SIZE_EXT - 1] = 0;
	VR_ModelBindNodes( &slot->model, (const char ( * )[VR_MODEL_NAME_SIZE])names, slot->nodeCount, slot->map );
	memset( &spaceInfo, 0, sizeof( spaceInfo ) );
	spaceInfo.type = XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT;
	spaceInfo.renderModel = slot->handle;
	result = ctx->createSpace( ctx->session, &spaceInfo, &slot->space );
	if ( result != XR_SUCCESS || !slot->space ) {
		slot->space = XR_NULL_HANDLE;
		XRM_Log( ctx, "OpenXR controller model refused: no space for it (%d)", (int)result, 0 );
		goto fail;
	}
	slot->serial = ++ctx->serial;
	ctx->release( bytes );
	if ( names )
		ctx->release( names );
	XRM_Log( ctx, "OpenXR controller model loaded: %d triangles, %d moving parts", slot->model.triangleCount,
			 slot->nodeCount );
	return 1;
fail:
	if ( bytes )
		ctx->release( bytes );
	if ( names )
		ctx->release( names );
	XRM_Drop( ctx, slot );
	return outcome;
}

static int XRM_Refused( const vkXRModels_t *ctx, XrRenderModelIdEXT id ) {
	int i;
	for ( i = 0; i < ctx->refusedCount; i++ )
		if ( ctx->refused[i] == id )
			return 1;
	return 0;
}

void VK_XRModels_Refresh( vkXRModels_t *ctx ) {
	XrRenderModelIdEXT ids[XRM_MAX_IDS];
	uint32_t count = 0, i;
	int s, kept = 0, waiting = 0;
	if ( !ctx || !ctx->session )
		return;
	ctx->retry = 0;
	if ( ctx->enumerate( ctx->session, NULL, 0, &count, NULL ) != XR_SUCCESS )
		return;
	if ( count > XRM_MAX_IDS )
		count = XRM_MAX_IDS;
	if ( count && ctx->enumerate( ctx->session, NULL, count, &count, ids ) != XR_SUCCESS )
		return;
	if ( count > XRM_MAX_IDS )
		count = XRM_MAX_IDS;
	/* An ID that stopped being enumerated never returns; a returning device comes back under a new one. */
	for ( s = 0; s < VK_XR_MODELS_MAX; s++ ) {
		vkXRModel_t *slot = &ctx->models[s];
		if ( !slot->handle )
			continue;
		for ( i = 0; i < count && ids[i] != slot->id; i++ )
			;
		if ( i == count )
			XRM_Drop( ctx, slot );
	}
	for ( s = 0; s < ctx->refusedCount; s++ ) {
		for ( i = 0; i < count && ids[i] != ctx->refused[s]; i++ )
			;
		if ( i < count )
			ctx->refused[kept++] = ctx->refused[s];
	}
	ctx->refusedCount = kept;
	for ( i = 0; i < count; i++ ) {
		int vacant = -1, outcome;
		if ( ids[i] == XR_NULL_RENDER_MODEL_ID_EXT || XRM_Refused( ctx, ids[i] ) )
			continue;
		for ( s = 0; s < VK_XR_MODELS_MAX; s++ ) {
			if ( ctx->models[s].handle && ctx->models[s].id == ids[i] )
				break;
			if ( !ctx->models[s].handle && vacant < 0 )
				vacant = s;
		}
		if ( s < VK_XR_MODELS_MAX || vacant < 0 )
			continue;
		outcome = XRM_Load( ctx, &ctx->models[vacant], ids[i] );
		if ( outcome < 0 && ctx->retries > 0 ) {
			waiting = 1;
			continue;
		}
		if ( outcome < 0 )
			XRM_Log( ctx, "OpenXR controller model refused: the runtime never had its asset ready", 0, 0 );
		if ( outcome <= 0 && ctx->refusedCount < VK_XR_MODELS_REFUSED )
			ctx->refused[ctx->refusedCount++] = ids[i];
	}
	if ( waiting ) {
		ctx->retries--;
		ctx->retry = XRM_RETRY_UPDATES;
	}
}

int VK_XRModels_Update( vkXRModels_t *ctx, XrSpace base, XrTime time, int focused ) {
	const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
										 XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	int s, i, refreshed = 0;
	if ( !ctx || !ctx->session )
		return 0;
	/* nothing enumerates before input has synced, which needs focus */
	if ( focused && ctx->retry && !--ctx->retry ) {
		VK_XRModels_Refresh( ctx );
		refreshed = 1;
	}
	for ( s = 0; s < VK_XR_MODELS_MAX; s++ ) {
		vkXRModel_t *slot = &ctx->models[s];
		XrSpaceLocation location;
		slot->drawable = 0;
		/* The runtime only shows these while the session has focus, so each is drawn once. */
		if ( !slot->handle || !focused || !base || time <= 0 )
			continue;
		memset( &location, 0, sizeof( location ) );
		location.type = XR_TYPE_SPACE_LOCATION;
		if ( ctx->locate( slot->space, base, time, &location ) != XR_SUCCESS ||
			 ( location.locationFlags & tracked ) != tracked ||
			 !VR_FloatsFinite( (const float *)&location.pose.position, 3 ) ||
			 !VR_FloatsFinite( (const float *)&location.pose.orientation, 4 ) )
			continue;
		if ( slot->nodeCount ) {
			XrRenderModelStateGetInfoEXT info;
			XrRenderModelStateEXT state;
			memset( &info, 0, sizeof( info ) );
			info.type = XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT;
			info.displayTime = time;
			memset( &state, 0, sizeof( state ) );
			state.type = XR_TYPE_RENDER_MODEL_STATE_EXT;
			state.nodeStateCount = (uint32_t)slot->nodeCount;
			state.nodeStates = slot->raw;
			if ( ctx->state( slot->handle, &info, &state ) != XR_SUCCESS )
				continue;
			for ( i = 0; i < slot->nodeCount; i++ ) {
				const XrPosef *pose = &slot->raw[i].nodePose;
				if ( !VR_FloatsFinite( (const float *)pose, 7 ) )
					break;
				memcpy( slot->states[i].pose.position, &pose->position, sizeof( slot->states[i].pose.position ) );
				memcpy( slot->states[i].pose.orientation, &pose->orientation, sizeof( slot->states[i].pose.orientation ) );
				slot->states[i].visible = slot->raw[i].isVisible != XR_FALSE;
			}
			if ( i < slot->nodeCount )
				continue;
		}
		memcpy( slot->root.position, &location.pose.position, sizeof( slot->root.position ) );
		memcpy( slot->root.orientation, &location.pose.orientation, sizeof( slot->root.orientation ) );
		slot->drawable = 1;
	}
	return refreshed;
}

void VK_XRModels_Shutdown( vkXRModels_t *ctx ) {
	int s;
	if ( !ctx )
		return;
	if ( ctx->session )
		for ( s = 0; s < VK_XR_MODELS_MAX; s++ )
			if ( ctx->models[s].handle )
				XRM_Drop( ctx, &ctx->models[s] );
	memset( ctx, 0, sizeof( *ctx ) );
}
