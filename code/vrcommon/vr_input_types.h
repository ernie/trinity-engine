#ifndef VR_INPUT_TYPES_H
#define VR_INPUT_TYPES_H
enum { CL_XRI_PRIMARY_BUTTON = 1, CL_XRI_SECONDARY_BUTTON = 2, CL_XRI_MENU_BUTTON = 4, CL_XRI_STICK_BUTTON = 8, CL_XRI_TRACKPAD_BUTTON = 16, CL_XRI_THUMBREST_BUTTON = 32,
	CL_XRI_BUMPER_BUTTON = 64, CL_XRI_DPAD_UP_BUTTON = 128, CL_XRI_DPAD_DOWN_BUTTON = 256, CL_XRI_DPAD_LEFT_BUTTON = 512,
	CL_XRI_DPAD_RIGHT_BUTTON = 1024, CL_XRI_VIEW_BUTTON = 2048, CL_XRI_X_BUTTON = 4096, CL_XRI_Y_BUTTON = 8192,
	CL_XRI_SQUEEZE_CLICK_BUTTON = 16384 };
enum { CL_XRP_TOUCH, CL_XRP_PICO4, CL_XRP_PICO4S, CL_XRP_INDEX, CL_XRP_SIMPLE, CL_XRP_FRAME, CL_XRP_COUNT };
typedef struct {
	float position[3], orientation[4]; /* OpenXR meters; quaternion x,y,z,w. */
	int positionValid, orientationValid, positionTracked, orientationTracked;
} clXRPose_t;
typedef struct {
	float trigger, squeeze, stick[2], trackpad;
	float pitchCorrection; /* degrees the active controller's grip pose needs on top of vr_weaponPitch */
	unsigned buttons;
	int active;
	int profile; /* CL_XRP_* the runtime reports for this hand, -1 until known */
	clXRPose_t grip, aim;
} clXRHandInput_t;
typedef struct { int focused; clXRHandInput_t hands[2]; } clXRInputSample_t;

#endif
