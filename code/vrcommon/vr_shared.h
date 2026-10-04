// Engine <-> module VR state ABI, shared by trinity-engine, trinity-standalone and the mods; append fields only.
#ifndef __VR_SHARED
#define __VR_SHARED

#include "../qcommon/q_shared.h"
#include "vr_safe_types.h"

// QVM-safe: 4-byte scalar types only, no pointers, identical layout on 64-bit host and 32-bit QVM.
// Additive changes bump the MINOR; layout changes bump the MAJOR.
#define VR_API_MAJOR 1
#define VR_API_MINOR 0
#define VR_API_STR2(x) #x
#define VR_API_STR(x) VR_API_STR2(x)
#define VR_API_SENTINEL "TRINITY_VR_API/" VR_API_STR(VR_API_MAJOR) "." VR_API_STR(VR_API_MINOR)

// thumbstick_location[] holds the sticks by role, deadzone-processed
#define VR_STICK_MOVE 0
#define VR_STICK_TURN 1

// pointerMode
#define VR_POINTER_CURSOR 0 // the ray moves the hover; the module draws its cursor
#define VR_POINTER_STICK  1 // a thumbstick moves the selection: no hover, no cursor, no ray
#define VR_POINTER_DRAWN  2 // the ray moves the hover; the engine draws the ray and its pool of light

typedef struct vr_shared_s {
	int structSize;     // sizeof(vr_shared_t), set by the module before registering
	int apiVersion;     // VR_API_MAJOR the module was built against

	// ---- eng block: engine-written; module writes are local-transient ----
	float fov_x;
	float fov_y;
	float fov_angle_up;
	float fov_angle_down;
	float fov_angle_left;
	float fov_angle_right;
	float eye_fov_angle_left[2];
	float eye_fov_angle_right[2];
	int   weapon_zoomed;
	float weapon_zoomLevel;
	int   right_handed;
	int   virtual_screen;
	int   first_person_following;
	int   use_6dof;
	int   follow_mode;          // VR_FollowMode value
	int   vote_holding;
	int   clientNum;
	float clientview_yaw_delta;
	vec3_t hmdposition;
	vec3_t hmdorientation;
	vec3_t hmdorigin;
	vec3_t weaponangles;
	vec3_t weaponoffset;
	vec3_t weaponposition;
	vec3_t offhandangles;
	vec3_t offhandoffset;
	vec3_t offhandposition;
	vec2_t thumbstick_location[2];
	int   menuLeftHanded;
	int   menuCursorX;          // engine-computed cursor coords (640x480 virtual)
	int   menuCursorY;
	int   scoreboardCursorX;
	int   scoreboardCursorY;
	int   sp_intermission_active;

	// ---- cg block: cgame-writable ----
	int   weapon_select;
	int   weapon_select_autoclose;
	int   weapon_select_using_thumbstick;
	int   weapon_adjust;
	int   weapon_stabilised;
	float snapTurnYaw;
	int   realign;
	int   recenter_follow_camera;
	vec3_t clientviewangles;
	vec3_t calculated_weaponangles;
	int   vote_active;
	float sp_intermission_hud_origin[3];
	float sp_intermission_hud_radius;

	// ---- uiShared block: cgame+ui-writable ----
	float menuYaw;
	int   menuYawLocked;
	int   menuCursorActive;       // menu cursor tracking requested
	int   scoreboardCursorActive; // scoreboard cursor tracking requested

	// ---- cfg block: cgame+game-writable ----
	int   no_crosshair;
	int   local_server;
	int   single_player;

	// ---- engine-written, appended at tail (trap ABI: keep last) ----
	int   pointerMode;          // VR_POINTER_*: who presents the menu selection
} vr_shared_t;

// Block starts; vr_shared_sync.c pins their offsets at compile time.
#define VR_SHARED_CG_FIRST   weapon_select
#define VR_SHARED_UI_FIRST   menuYaw
#define VR_SHARED_CFG_FIRST  no_crosshair

#endif
