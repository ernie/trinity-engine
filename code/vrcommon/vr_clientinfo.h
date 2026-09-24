#ifndef __VR_CLIENTINFO
#define __VR_CLIENTINFO

#include "../qcommon/q_shared.h"
#include "vr_safe_types.h"

typedef struct vr_clientinfo_s {
	float fov_x;
	float fov_y;
	float fov_angle_up;    // radians, positive = up
	float fov_angle_down;  // radians, negative = down
	float fov_angle_left;  // radians, negative = left
	float fov_angle_right; // radians, positive = right

	// Radians; separate per eye because the stereo frusta are asymmetric
	float eye_fov_angle_left[2];   // [0]=left eye, [1]=right eye
	float eye_fov_angle_right[2];  // [0]=left eye, [1]=right eye

	qboolean weapon_stabilised;
	qboolean weapon_zoomed;
	float weapon_zoomLevel;
	qboolean right_handed;
	qboolean virtual_screen;
	qboolean first_person_following;
	qboolean in_menu;
	qboolean local_server;
	qboolean single_player;
	qboolean use_6dof;
	VR_FollowMode follow_mode;
	qboolean weapon_select;
	qboolean weapon_select_autoclose;
	qboolean weapon_select_using_thumbstick;
	qboolean weapon_adjust;
	qboolean no_crosshair;
	qboolean vote_active;           // true when any yes/no dialog is visible (vote, team vote, TVD offer)
	int vote_holding;               // 0=none, 1=A held (yes), -1=B held (no): set by engine, read by cgame

	int realign; // used to realign the 6DoF playspace in a multiplayer game
	qboolean recenter_follow_camera;
	float snapTurnYaw; // set by cgame

	int clientNum;
	vec3_t clientviewangles; //read by cgame
	float clientview_yaw_last; // previous frame's yaw; delta source only
	float clientview_yaw_delta;

	vec3_t hmdposition;
	vec3_t hmdorigin; //used to recenter the mp 6DoF playspace
	vec3_t hmdposition_delta;

	vec3_t hmdorientation;
	vec3_t hmdorientation_delta;

	vec3_t weaponangles;
	vec3_t calculated_weaponangles; //Angle from the view origin to the point the controller is pointing at

	vec3_t weaponoffset;
	vec3_t weaponoffset_last[2];
	vec3_t weaponposition;

	vec3_t offhandangles;

	// Runtime aim pose without vr_weaponPitch, so tuning the weapon leaves the menu cursor put
	vec3_t weaponaimangles;
	vec3_t offhandaimangles;
	vec3_t offhandoffset;
	vec3_t offhandposition;

	vec2_t thumbstick_location[2]; //left / right

	qboolean walking;	// analog walk/run: true => assert BUTTON_WALKING (silent walk, no footsteps)

	float menuYaw;
	qboolean menuYawLocked;	// prevent renderer from overwriting menuYaw (used during timeline scrub)
	int menuCursorX;                // engine-computed menu cursor (640x480 virtual)
	int menuCursorY;
	int scoreboardCursorX;          // engine-computed scoreboard/vote cursor
	int scoreboardCursorY;
	qboolean menuCursorActive;      // module wants engine menu-cursor tracking
	qboolean scoreboardCursorActive;// module wants engine scoreboard-cursor tracking
	int menuStickNavActive;         // engine: thumbstick driving menu nav (synced to modules)
	int probeEcho;                  // ABI round-trip: module writes, engine reflects at sync
	qboolean menuLeftHanded;
	int offhandCursorX;             // 640x480 virtual coords
	int offhandCursorY;
	qboolean vkbOffhandTriggerDown; // only meaningful while the keyboard is up

	// SP intermission HUD anchoring (world-fixed UI during podium view)
	qboolean sp_intermission_active;
	float sp_intermission_yaw;  // The yaw angle the overlay is anchored to (degrees)

	float sp_intermission_hud_origin[3];    // absolute, not relative to the view
	float sp_intermission_hud_radius;
} vr_clientinfo_t;

#endif
