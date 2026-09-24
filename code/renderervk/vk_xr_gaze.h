#ifndef VK_XR_GAZE_H
#define VK_XR_GAZE_H
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include "../thirdparty/openxr/openxr.h"
typedef struct {
	XrAction action;
	XrSpace space;
	XrSession session;
	int supported;
	PFN_xrCreateActionSpace createSpace;
	PFN_xrDestroySpace destroySpace;
	PFN_xrGetActionStatePose state;
	PFN_xrLocateSpace locate;
} vkXRGaze_t;
/* Call after controller action-set creation, BEFORE attaching that set.
 * The borrowed set owns the gaze action. Failure leaves controller input usable. */
XrResult VK_XRGaze_Init( vkXRGaze_t *, XrInstance, XrSystemId, XrActionSet, PFN_xrGetInstanceProcAddr,
						 int enabled );
XrResult VK_XRGaze_Attach( vkXRGaze_t *, XrSession );
/* After successful controller sync. Gaze direction returned in head-local space.
 * head orientation/baseSpace/time must be the same predicted frame as the eyes. */
int VK_XRGaze_Sample( vkXRGaze_t *, XrSpace, const float head[4], XrTime, int focused, float direction[3] );
/* Before destroying the borrowed action set or session. */
void VK_XRGaze_Shutdown( vkXRGaze_t * );
#endif
