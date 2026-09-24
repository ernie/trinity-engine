#ifndef VR_INPUT_TYPES_H
#define VR_INPUT_TYPES_H
enum { CL_XRI_PRIMARY_BUTTON = 1, CL_XRI_SECONDARY_BUTTON = 2, CL_XRI_MENU_BUTTON = 4, CL_XRI_STICK_BUTTON = 8, CL_XRI_TRACKPAD_BUTTON = 16, CL_XRI_THUMBREST_BUTTON = 32 };
typedef struct {
	float position[3], orientation[4]; /* OpenXR meters; quaternion x,y,z,w. */
	int positionValid, orientationValid, positionTracked, orientationTracked;
} clXRPose_t;
typedef struct {
	float trigger, squeeze, stick[2];
	unsigned buttons;
	int active;
	clXRPose_t grip, aim;
} clXRHandInput_t;
typedef struct { int focused; clXRHandInput_t hands[2]; } clXRInputSample_t;

#endif
