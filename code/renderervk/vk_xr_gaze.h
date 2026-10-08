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
 * head orientation/baseSpace/time must be the same predicted frame as the eyes; the view space, when given,
 * carries an older eye sample through the head's turn since. */
int VK_XRGaze_Sample( vkXRGaze_t *, XrSpace base, XrSpace view, const float head[4], XrTime, int focused,
					  float direction[3] );
/* Before destroying the borrowed action set or session. */
void VK_XRGaze_Shutdown( vkXRGaze_t * );

/* The runtime's own eye-tracked foveation center (XR_META_foveation_eye_tracked), which needs an eye-tracked
 * XR_FB_foveation profile on a swapchain created for one. */
typedef struct {
	XrSession session; /* the one that owns the profile */
	XrFoveationProfileFB profile;
	int supported, failed;
	PFN_xrCreateFoveationProfileFB create;
	PFN_xrDestroyFoveationProfileFB destroy;
	PFN_xrUpdateSwapchainFB update;
	PFN_xrGetFoveationEyeTrackedStateMETA state;
} vkXRFovCenter_t;
/* enabled: the instance has XR_META_foveation_eye_tracked and the XR_FB extensions it needs. */
XrResult VK_XRFovCenter_Init( vkXRFovCenter_t *, XrInstance, XrSystemId, PFN_xrGetInstanceProcAddr, int enabled );
/* After xrBeginFrame, on a swapchain created with XrSwapchainCreateInfoFoveationFB: 1 with each eye's center in
 * NDC (+Y down), 0 when the runtime has none this frame, -1 once when it refuses, which ends the sampling. */
int VK_XRFovCenter_Sample( vkXRFovCenter_t *, XrSession, XrSwapchain, float center[2][2] );
/* Before destroying the session. */
void VK_XRFovCenter_Shutdown( vkXRFovCenter_t * );
#endif
