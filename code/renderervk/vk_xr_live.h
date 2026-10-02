#ifndef VK_XR_LIVE_H
#define VK_XR_LIVE_H
#define XR_NO_PROTOTYPES
#include "../thirdparty/openxr/openxr.h"
#include "../vrcommon/xr_loader.h"
typedef struct {
	void *library;
	XrInstance instance;
	XrSystemId system;
	XrVersion apiVersion; /* what the instance was created for: 1.1, or 1.0 on a runtime without it */
	PFN_xrGetInstanceProcAddr getproc;
	PFN_xrDestroyInstance destroy;
	int picoInteraction, displayRefresh, eyeGaze, formatList, frameInteraction, createInfoMeta;
	int models; /* XR_EXT_render_model and XR_EXT_interaction_render_model are both enabled */
	XrResult modelsRefused; /* why the runtime turned down an instance with them, when it lists them */
	const char *enabled[10];
	unsigned enabledCount;
	char runtimeName[XR_MAX_RUNTIME_NAME_SIZE];
} vkXRLive_t;
/* Explicit VR requests only. Instance asks for OpenXR 1.1, then 1.0, and enables Vulkan2 plus optional extensions it finds.
 * The controller model extensions are among them; a runtime that lists them but refuses
 * the instance gets a second attempt without them.
 * No session/graphics creation. Close after every session/action owner is gone. */
XrResult VK_XRLive_Open( vkXRLive_t *ctx );
void VK_XRLive_Close( vkXRLive_t *ctx );
#endif
