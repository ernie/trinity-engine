#include "vk_xr_meta.h"
#if VR_MODEL_KTX2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../vrcommon/vr_float.h"
#include "../vrcommon/vr_meta_pose.h"

/* XR_FB_render_model doesn't say which nodes move, so input nodes are matched by name, as Meta's loader does. */
typedef struct {
	vkXRModel_t slot;
	int map[VR_META_INPUT_COUNT];
	vrModelNodeState_t states[VR_META_INPUT_COUNT];
} vkXRMetaModel_t;

static struct {
	XrInstance instance;
	XrSession session;
	vrModelAlloc_t alloc;
	vrModelFree_t release;
	void ( *log )( const char *line );
	PFN_xrEnumerateRenderModelPathsFB paths;
	PFN_xrGetRenderModelPropertiesFB properties;
	PFN_xrLoadRenderModelFB load;
	PFN_xrPathToString pathToString;
	PFN_xrLocateSpace locate;
	int loaded; /* the one fetch, on the first shown frame: earlier calls get XR_ERROR_CALL_ORDER_INVALID */
	vkXRMetaModel_t hands[2];
} meta;
/* Survives sessions so the renderer can tell a reload from what it cached */
static unsigned metaSerial;
static const char *const metaPaths[2] = {"/model_fb/controller/left", "/model_fb/controller/right"};

static void VKXRMeta_Log( const char *format, const char *path, int value, unsigned count ) {
	char line[256];
	if ( !meta.log )
		return;
	snprintf( line, sizeof( line ), format, path, value, count );
	meta.log( line );
}

/* a NULL hand leaves the nodes at rest */
static void VKXRMeta_Pose( vkXRMetaModel_t *m, const clXRHandInput_t *hand ) {
	vrMetaInput_t input;
	memset( &input, 0, sizeof( input ) );
	if ( hand ) {
		input.buttonAX = ( hand->buttons & CL_XRI_PRIMARY_BUTTON ) != 0;
		input.buttonBY = ( hand->buttons & CL_XRI_SECONDARY_BUTTON ) != 0;
		input.menu = ( hand->buttons & CL_XRI_MENU_BUTTON ) != 0;
		input.trigger = hand->trigger;
		input.grip = hand->squeeze;
		input.stick[0] = hand->stick[0];
		input.stick[1] = hand->stick[1];
	}
	VR_MetaPoseInputs( &m->slot.model, m->map, &input, m->states );
}

static void VKXRMeta_Load( int hand, XrPath path ) {
	vkXRMetaModel_t *m = &meta.hands[hand];
	XrRenderModelCapabilitiesRequestFB capabilities;
	XrRenderModelPropertiesFB properties;
	XrRenderModelLoadInfoFB loadInfo;
	XrRenderModelBufferFB buffer;
	XrResult result;
	unsigned char *glb = NULL;
	uint32_t size = 0;
	int i;

	/* which glTF subsets we take; the parser reads the core format either way */
	memset( &capabilities, 0, sizeof( capabilities ) );
	capabilities.type = XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB;
	capabilities.flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB | XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB;
	memset( &properties, 0, sizeof( properties ) );
	properties.type = XR_TYPE_RENDER_MODEL_PROPERTIES_FB;
	properties.next = &capabilities;
	result = meta.properties( meta.session, path, &properties );
	if ( result != XR_SUCCESS || properties.modelKey == XR_NULL_RENDER_MODEL_KEY_FB ) {
		VKXRMeta_Log( "Meta controller model %s: no properties (%d)", metaPaths[hand], (int)result, 0 );
		return;
	}
	memset( &loadInfo, 0, sizeof( loadInfo ) );
	loadInfo.type = XR_TYPE_RENDER_MODEL_LOAD_INFO_FB;
	loadInfo.modelKey = properties.modelKey;
	memset( &buffer, 0, sizeof( buffer ) );
	buffer.type = XR_TYPE_RENDER_MODEL_BUFFER_FB;
	result = meta.load( meta.session, &loadInfo, &buffer );
	if ( result == XR_SUCCESS && ( !buffer.bufferCountOutput || buffer.bufferCountOutput > VK_XR_MODEL_MAX_BYTES ) )
		result = XR_ERROR_SIZE_INSUFFICIENT;
	if ( result == XR_SUCCESS ) {
		size = buffer.bufferCountOutput;
		glb = malloc( size );
		buffer.bufferCapacityInput = size;
		buffer.buffer = glb;
		result = glb ? meta.load( meta.session, &loadInfo, &buffer ) : XR_ERROR_OUT_OF_MEMORY;
	}
	if ( result != XR_SUCCESS ) {
		VKXRMeta_Log( "Meta controller model %s: the file could not be read (%d)", metaPaths[hand], (int)result, 0 );
		free( glb );
		return;
	}
	if ( !VR_ModelParse( glb, size, meta.alloc, meta.release, &m->slot.model ) ) {
		VKXRMeta_Log( "Meta controller model %s: its file is malformed or too large (%d, %u bytes)", metaPaths[hand], 0, size );
		free( glb );
		return;
	}
	free( glb );
	/* hide the battery gauge quad, which only the runtime's own battery display fills in */
	for ( i = 0; i < m->slot.model.nodeCount; i++ )
		if ( strstr( m->slot.model.nodes[i].name, "batteryIndicatorQuad" ) )
			m->slot.model.nodes[i].mesh = -1;
	VR_MetaBindInputs( &m->slot.model, m->map );
	m->slot.map = m->map;
	m->slot.states = m->states;
	m->slot.nodeCount = VR_META_INPUT_COUNT;
	VKXRMeta_Pose( m, NULL );
	/* the renderer caches by id and size, and each hand's model path is fixed, so the id names the hand */
	strncpy( (char *)m->slot.cacheId, hand ? "meta-right" : "meta-left", sizeof( m->slot.cacheId ) - 1 );
	m->slot.serial = ++metaSerial;
	VKXRMeta_Log( "Meta controller model %s: loaded (%d triangles)", metaPaths[hand], m->slot.model.triangleCount, 0 );
}

static void VKXRMeta_LoadAll( void ) {
	XrRenderModelPathInfoFB *paths;
	uint32_t count = 0, i;
	int hand;

	meta.loaded = 1;
	if ( meta.paths( meta.session, 0, &count, NULL ) != XR_SUCCESS || !count ) {
		VKXRMeta_Log( "Meta controller models: none offered%s", "", 0, 0 );
		return;
	}
	paths = calloc( count, sizeof( *paths ) );
	if ( !paths )
		return;
	for ( i = 0; i < count; i++ )
		paths[i].type = XR_TYPE_RENDER_MODEL_PATH_INFO_FB;
	if ( meta.paths( meta.session, count, &count, paths ) == XR_SUCCESS ) {
		for ( i = 0; i < count; i++ ) {
			char name[XR_MAX_PATH_LENGTH];
			uint32_t length = 0;
			if ( meta.pathToString( meta.instance, paths[i].path, sizeof( name ), &length, name ) != XR_SUCCESS )
				continue;
			for ( hand = 0; hand < 2; hand++ )
				if ( !strcmp( name, metaPaths[hand] ) && !meta.hands[hand].slot.model.block )
					VKXRMeta_Load( hand, paths[i].path );
		}
	}
	free( paths );
}

void VK_XRMeta_Init( XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr getproc, vrModelAlloc_t alloc,
					 vrModelFree_t release, void ( *log )( const char *line ) ) {
	VK_XRMeta_Shutdown();
	meta.instance = instance;
	meta.session = session;
	meta.alloc = alloc;
	meta.release = release;
	meta.log = log;
	if ( getproc( instance, "xrEnumerateRenderModelPathsFB", (PFN_xrVoidFunction *)&meta.paths ) != XR_SUCCESS ||
		 getproc( instance, "xrGetRenderModelPropertiesFB", (PFN_xrVoidFunction *)&meta.properties ) != XR_SUCCESS ||
		 getproc( instance, "xrLoadRenderModelFB", (PFN_xrVoidFunction *)&meta.load ) != XR_SUCCESS ||
		 getproc( instance, "xrPathToString", (PFN_xrVoidFunction *)&meta.pathToString ) != XR_SUCCESS ||
		 getproc( instance, "xrLocateSpace", (PFN_xrVoidFunction *)&meta.locate ) != XR_SUCCESS ) {
		VKXRMeta_Log( "Meta controller models: the runtime lists the extension without its functions%s", "", 0, 0 );
		meta.loaded = 1;
		meta.session = XR_NULL_HANDLE;
	}
}

int VK_XRMeta_Update( XrSpace base, XrTime time, int shown, const XrSpace grips[2], const clXRHandInput_t *hands ) {
	const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
										 XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	int hand, fetched = 0;

	if ( !meta.session )
		return 0;
	if ( shown && !meta.loaded ) {
		VKXRMeta_LoadAll();
		fetched = 1;
	}
	/* the models follow the grip pose, as the runtime's own hands do */
	for ( hand = 0; hand < 2; hand++ ) {
		vkXRModel_t *slot = &meta.hands[hand].slot;
		XrSpaceLocation location;
		slot->drawable = 0;
		if ( !slot->model.block || !shown || grips[hand] == XR_NULL_HANDLE || base == XR_NULL_HANDLE || time <= 0 )
			continue;
		memset( &location, 0, sizeof( location ) );
		location.type = XR_TYPE_SPACE_LOCATION;
		if ( meta.locate( grips[hand], base, time, &location ) != XR_SUCCESS ||
			 ( location.locationFlags & tracked ) != tracked ||
			 !VR_FloatsFinite( (const float *)&location.pose.position, 3 ) ||
			 !VR_FloatsFinite( (const float *)&location.pose.orientation, 4 ) )
			continue;
		memcpy( slot->root.position, &location.pose.position, sizeof( slot->root.position ) );
		memcpy( slot->root.orientation, &location.pose.orientation, sizeof( slot->root.orientation ) );
		slot->drawable = 1;
		VKXRMeta_Pose( &meta.hands[hand], hands ? &hands[hand] : NULL );
	}
	return fetched;
}

const vkXRModel_t *VK_XRMeta_Model( int hand ) {
	return hand >= 0 && hand < 2 && meta.hands[hand].slot.model.block ? &meta.hands[hand].slot : NULL;
}

void VK_XRMeta_Shutdown( void ) {
	int hand;
	for ( hand = 0; hand < 2; hand++ )
		if ( meta.hands[hand].slot.model.block )
			VR_ModelFree( &meta.hands[hand].slot.model, meta.release );
	memset( &meta, 0, sizeof( meta ) );
}
#endif
