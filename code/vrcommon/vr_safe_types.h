#ifndef __VR_SAFE_TYPES
#define __VR_SAFE_TYPES

typedef enum
{
	VRFM_NONE = 0,      // Not following / mirror not yet synced (zero must stay inert)
	VRFM_THIRDPERSON_1, // Camera will auto move to keep up with player
	VRFM_THIRDPERSON_2,	// Camera is completely free movement with the thumbstick
	VRFM_FIRSTPERSON,
	VRFM_NUM_FOLLOWMODES,

	VRFM_QUERY = 99	// Used to query which mode is active
} VR_FollowMode;

typedef enum
{
	WS_CONTROLLER,
	WS_HMD,
	WS_ALTKEY,
	WS_PREVNEXT
} VR_WeaponSelectorType;

#endif
