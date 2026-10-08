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
	int foveationCenter; /* XR_META_foveation_eye_tracked and the XR_FB foveation extensions it builds on */
	int models; /* XR_EXT_render_model and XR_EXT_interaction_render_model are both enabled */
	XrResult modelsRefused; /* why the runtime turned down an instance with them, when it lists them */
	const char *enabled[15];
	unsigned enabledCount;
	char runtimeName[XR_MAX_RUNTIME_NAME_SIZE];
} vkXRLive_t;
/* OpenXR 1.1 then 1.0, Vulkan2 plus the optional extensions found; a runtime that lists the controller model
 * extensions but refuses the instance gets a second attempt without them. No session or graphics creation. */
XrResult VK_XRLive_Open( vkXRLive_t *ctx );
void VK_XRLive_Close( vkXRLive_t *ctx );
#endif
