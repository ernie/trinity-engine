#ifndef VK_XR_INPUT_H
#define VK_XR_INPUT_H
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include "../thirdparty/openxr/openxr.h"

enum {
	CL_XRI_TRIGGER,
	CL_XRI_SQUEEZE,
	CL_XRI_STICK,
	CL_XRI_STICK_CLICK,
	CL_XRI_PRIMARY,
	CL_XRI_SECONDARY,
	CL_XRI_MENU,
	CL_XRI_GRIP_POSE,
	CL_XRI_AIM_POSE,
	CL_XRI_HAPTIC,
	CL_XRI_TRACKPAD,
	CL_XRI_THUMBREST,
	CL_XRI_BUMPER,
	CL_XRI_DPAD_UP,
	CL_XRI_DPAD_DOWN,
	CL_XRI_DPAD_LEFT,
	CL_XRI_DPAD_RIGHT,
	CL_XRI_VIEW,
	CL_XRI_X,
	CL_XRI_Y,
	CL_XRI_SQUEEZE_CLICK,
	CL_XRI_ACTIONS
};
#include "../vrcommon/vr_input_types.h"

#define CL_XRI_FUNCTIONS(X) \
	X( StringToPath ) \
	X( CreateActionSet ) X( DestroyActionSet ) X( CreateAction ) X( SuggestInteractionProfileBindings ) \
		X( AttachSessionActionSets ) X( CreateActionSpace ) X( DestroySpace ) X( SyncActions ) \
			X( GetActionStateFloat ) X( GetActionStateVector2f ) X( GetActionStateBoolean ) \
				X( GetActionStatePose ) X( LocateSpace ) X( ApplyHapticFeedback ) X( StopHapticFeedback ) \
					X( GetCurrentInteractionProfile ) X( PathToString )
typedef struct {
#define CL_XRI_PROC(name) PFN_xr##name name;
	CL_XRI_FUNCTIONS( CL_XRI_PROC )
#undef CL_XRI_PROC
} vkXRInputDispatch_t;
typedef struct {
	vkXRInputDispatch_t xr;
	XrInstance instance;
	XrSession session;
	XrActionSet set;
	XrAction actions[CL_XRI_ACTIONS];
	XrPath hands[2];
	XrSpace spaces[2][2]; /* grip, aim */
	int focused;
	XrPath profilePaths[CL_XRP_COUNT];
	int profile[2]; /* CL_XRP_* per hand, -1 until the runtime reports one */
} vkXRInput_t;
/* Init once per instance before Attach; Pico profiles require the instance's
 * optional XR_BD_controller_interaction extension to have been enabled. */
XrResult VK_XRInput_Init( vkXRInput_t *ctx, XrInstance instance, PFN_xrGetInstanceProcAddr getproc,
						  int picoEnabled, int frameEnabled );
XrResult VK_XRInput_Attach( vkXRInput_t *ctx, XrSession session );
/* Re-reads the runtime's active interaction profile for each hand; call after attach and on
 * XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED. */
void VK_XRInput_UpdateProfiles( vkXRInput_t *ctx );
const char *VK_XRInput_ProfileName( const vkXRInput_t *ctx, int hand );
float VK_XRInput_PitchCorrection( const vkXRInput_t *ctx, int hand );
/* focused means active VR and XR FOCUSED; poses stay in OpenXR coordinates and baseSpace must match the rendered
 * views at the predicted display time. */
XrResult VK_XRInput_Sample( vkXRInput_t *ctx, XrSpace baseSpace, XrTime time, int focused,
							clXRInputSample_t *sample );
void VK_XRInput_Reset( vkXRInput_t *ctx );
XrResult VK_XRInput_Haptic( vkXRInput_t *ctx, int hand, float amplitude, int durationMs );
/* Before destroying session; destroys action spaces and set (owns actions). */
void VK_XRInput_Shutdown( vkXRInput_t *ctx );
#endif
