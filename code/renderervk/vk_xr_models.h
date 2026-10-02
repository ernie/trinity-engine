/* The runtime's models of the devices in the player's hands (XR_EXT_interaction_render_model):
 * which exist, their parsed assets, and where each one and its moving parts are this frame.
 * No engine or Vulkan dependencies. */
#ifndef VK_XR_MODELS_H
#define VK_XR_MODELS_H
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include "../thirdparty/openxr/openxr.h"
#include "../vrcommon/vr_model.h"

#define VK_XR_MODELS_MAX 8
#define VK_XR_MODELS_REFUSED 16
#define VK_XR_MODEL_MAX_BYTES ( 64u << 20 )

typedef struct {
	XrRenderModelIdEXT id;
	XrRenderModelEXT handle; /* XR_NULL_HANDLE marks a free slot */
	XrSpace space;
	unsigned char cacheId[16]; /* names the asset; the same device returning brings the same one */
	vrModel_t model;
	int nodeCount; /* animatable nodes, in the runtime's order */
	int *map;	   /* each one's node in the model, or -1 */
	XrRenderModelNodeStateEXT *raw;
	vrModelNodeState_t *states;
	vrModelPose_t root;
	int drawable;	 /* located and fully tracked this frame */
	unsigned serial; /* a different value means the slot holds a different load */
} vkXRModel_t;

typedef struct {
	XrSession session;
	vrModelAlloc_t alloc;
	vrModelFree_t release;
	void ( *log )( const char *line );
	PFN_xrEnumerateInteractionRenderModelIdsEXT enumerate;
	PFN_xrCreateRenderModelEXT create;
	PFN_xrDestroyRenderModelEXT destroy;
	PFN_xrGetRenderModelPropertiesEXT properties;
	PFN_xrCreateRenderModelSpaceEXT createSpace;
	PFN_xrCreateRenderModelAssetEXT createAsset;
	PFN_xrDestroyRenderModelAssetEXT destroyAsset;
	PFN_xrGetRenderModelAssetDataEXT assetData;
	PFN_xrGetRenderModelAssetPropertiesEXT assetProperties;
	PFN_xrGetRenderModelStateEXT state;
	PFN_xrLocateSpace locate;
	PFN_xrDestroySpace destroySpace;
	vkXRModel_t models[VK_XR_MODELS_MAX];
	XrRenderModelIdEXT refused[VK_XR_MODELS_REFUSED]; /* enumerated IDs whose asset can't be used; never fetched again */
	int refusedCount;
	unsigned serial;
	int retry;	 /* focused updates left before an unasked refresh */
	int retries; /* unasked refreshes left for assets the runtime didn't have ready */
} vkXRModels_t;

/* enabled says both extensions are live on the instance; otherwise every call below does nothing.
 * A failure leaves the context disabled and everything else usable. log may be NULL. */
XrResult VK_XRModels_Init( vkXRModels_t *, XrInstance, XrSession, PFN_xrGetInstanceProcAddr, int enabled,
						   vrModelAlloc_t, vrModelFree_t, void ( *log )( const char *line ) );
/* After XrEventDataInteractionRenderModelsChangedEXT: drops models whose device is gone, loads new ones. */
void VK_XRModels_Refresh( vkXRModels_t * );
/* After the frame's xrSyncActions, with the frame's base space and display time. Returns 1 when it went to the
 * runtime for models unasked, which stalls the frame like a Refresh. */
int VK_XRModels_Update( vkXRModels_t *, XrSpace base, XrTime time, int focused );
/* Before the session is destroyed. */
void VK_XRModels_Shutdown( vkXRModels_t * );
#endif
