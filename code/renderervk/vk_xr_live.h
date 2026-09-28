#ifndef VK_XR_LIVE_H
#define VK_XR_LIVE_H
#define XR_NO_PROTOTYPES
#include "../thirdparty/openxr/openxr.h"
#include "../vrcommon/xr_loader.h"
typedef struct {
	void *library;
	XrInstance instance;
	XrSystemId system;
	PFN_xrGetInstanceProcAddr getproc;
	PFN_xrDestroyInstance destroy;
	int picoInteraction, displayRefresh, eyeGaze, formatList, frameInteraction, createInfoMeta;
	const char *enabled[7];
	unsigned enabledCount;
} vkXRLive_t;
/* Explicit VR requests only. Instance enables Vulkan2 plus optional extensions it finds.
 * No session/graphics creation. Close after every session/action owner is gone. */
XrResult VK_XRLive_Open( vkXRLive_t *ctx );
void VK_XRLive_Close( vkXRLive_t *ctx );
#endif
