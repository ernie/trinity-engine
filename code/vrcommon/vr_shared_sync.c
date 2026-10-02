// Engine <-> module VR state sync (see vr_shared.h for the ABI).
// Sync-in copies every block within the module's registered ABI size, so each
// module sees fresh cross-module state without overrunning older mirrors.
// Sync-out copies only blocks the module type is a declared writer of, so a
// stale mirror cannot clobber another writer.

#include <stddef.h>
#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "vr_clientinfo.h"
#include "vr_shared.h"
#include "vr_state.h"

// ABI v1.0 is frozen: these fail to compile if vr_shared_t's size or block
// boundaries drift. Moving or removing an existing field is a MAJOR bump
// (update the pins); a new tail field is a MINOR bump.
#define VR_ABI_ASSERT( name, expr ) typedef char name[ (expr) ? 1 : -1 ]
VR_ABI_ASSERT( vr_abi_v1_size, sizeof( vr_shared_t ) == 352 );
VR_ABI_ASSERT( vr_abi_v1_cg_first, offsetof( vr_shared_t, VR_SHARED_CG_FIRST ) == 240 );
VR_ABI_ASSERT( vr_abi_v1_ui_first, offsetof( vr_shared_t, VR_SHARED_UI_FIRST ) == 320 );
VR_ABI_ASSERT( vr_abi_v1_cfg_first, offsetof( vr_shared_t, VR_SHARED_CFG_FIRST ) == 336 );

vr_clientinfo_t vr;
static qboolean activeMode;

void VR_SetActiveMode( qboolean active ) {
	active = active ? qtrue : qfalse;
	if ( activeMode == active ) {
		return;
	}
	activeMode = active;
	vr.weapon_select = qfalse;
	vr.weapon_adjust = qfalse;
	vr.weapon_stabilised = qfalse;
	vr.weapon_zoomed = qfalse;
	vr.menuYawLocked = qfalse;
	vr.menuCursorActive = qfalse;
	vr.scoreboardCursorActive = qfalse;
	vr.snapTurnYaw = 0;
	vr.realign = 0;
	vr.recenter_follow_camera = qfalse;
}

qboolean VR_IsActiveMode( void ) {
	return activeMode;
}

void VR_SharedSyncIn( vr_shared_t *dst, int structSize ) {
	// Fill a full scratch, then copy only the bytes the module's struct has, so an
	// older (smaller) module's mirror is never overwritten. structSize is clamped
	// to [0,sizeof] and captured at registration, never re-read from module memory:
	// a hostile QVM can shrink what we write but never push it past its own block.
	vr_shared_t scratch;
	vr_shared_t *s = &scratch;
	// eng block
	s->fov_x = vr.fov_x;
	s->fov_y = vr.fov_y;
	s->fov_angle_up = vr.fov_angle_up;
	s->fov_angle_down = vr.fov_angle_down;
	s->fov_angle_left = vr.fov_angle_left;
	s->fov_angle_right = vr.fov_angle_right;
	s->eye_fov_angle_left[0] = vr.eye_fov_angle_left[0];
	s->eye_fov_angle_left[1] = vr.eye_fov_angle_left[1];
	s->eye_fov_angle_right[0] = vr.eye_fov_angle_right[0];
	s->eye_fov_angle_right[1] = vr.eye_fov_angle_right[1];
	s->weapon_zoomed = vr.weapon_zoomed;
	s->weapon_zoomLevel = vr.weapon_zoomLevel;
	s->right_handed = vr.right_handed;
	s->virtual_screen = vr.virtual_screen;
	s->first_person_following = vr.first_person_following;
	s->use_6dof = vr.use_6dof;
	s->follow_mode = vr.follow_mode;
	s->vote_holding = vr.vote_holding;
	s->clientNum = vr.clientNum;
	s->clientview_yaw_delta = vr.clientview_yaw_delta;
	VectorCopy( vr.hmdposition, s->hmdposition );
	VectorCopy( vr.hmdorientation, s->hmdorientation );
	VectorCopy( vr.hmdorigin, s->hmdorigin );
	VectorCopy( vr.weaponangles, s->weaponangles );
	VectorCopy( vr.weaponoffset, s->weaponoffset );
	VectorCopy( vr.weaponposition, s->weaponposition );
	VectorCopy( vr.offhandangles, s->offhandangles );
	VectorCopy( vr.offhandoffset, s->offhandoffset );
	VectorCopy( vr.offhandposition, s->offhandposition );
	s->thumbstick_location[0][0] = vr.thumbstick_location[0][0];
	s->thumbstick_location[0][1] = vr.thumbstick_location[0][1];
	s->thumbstick_location[1][0] = vr.thumbstick_location[1][0];
	s->thumbstick_location[1][1] = vr.thumbstick_location[1][1];
	s->menuLeftHanded = vr.menuLeftHanded;
	s->menuCursorX = vr.menuCursorX;
	s->menuCursorY = vr.menuCursorY;
	s->scoreboardCursorX = vr.scoreboardCursorX;
	s->scoreboardCursorY = vr.scoreboardCursorY;
	s->sp_intermission_active = vr.sp_intermission_active;
	s->probeEchoBack = vr.probeEcho;
	s->pointerMode = vr.pointerMode;

	// cg block
	s->weapon_select = vr.weapon_select;
	s->weapon_select_autoclose = vr.weapon_select_autoclose;
	s->weapon_select_using_thumbstick = vr.weapon_select_using_thumbstick;
	s->weapon_adjust = vr.weapon_adjust;
	s->weapon_stabilised = vr.weapon_stabilised;
	s->snapTurnYaw = vr.snapTurnYaw;
	s->realign = vr.realign;
	s->recenter_follow_camera = vr.recenter_follow_camera;
	VectorCopy( vr.clientviewangles, s->clientviewangles );
	VectorCopy( vr.calculated_weaponangles, s->calculated_weaponangles );
	s->vote_active = vr.vote_active;
	s->sp_intermission_hud_origin[0] = vr.sp_intermission_hud_origin[0];
	s->sp_intermission_hud_origin[1] = vr.sp_intermission_hud_origin[1];
	s->sp_intermission_hud_origin[2] = vr.sp_intermission_hud_origin[2];
	s->sp_intermission_hud_radius = vr.sp_intermission_hud_radius;
	s->probeEcho = vr.probeEcho;

	// uiShared block
	s->menuYaw = vr.menuYaw;
	s->menuYawLocked = vr.menuYawLocked;
	s->menuCursorActive = vr.menuCursorActive;
	s->scoreboardCursorActive = vr.scoreboardCursorActive;

	// cfg block
	s->no_crosshair = vr.no_crosshair;
	s->local_server = vr.local_server;
	s->single_player = vr.single_player;

	// copy only the module-declared bytes; the module owns the header
	// (structSize/apiVersion at offset 0), so start at the first engine field.
	if ( structSize > (int)sizeof( scratch ) )
		structSize = (int)sizeof( scratch );
	if ( structSize > (int)offsetof( vr_shared_t, fov_x ) )
		memcpy( (char *)dst + offsetof( vr_shared_t, fov_x ),
			(char *)&scratch + offsetof( vr_shared_t, fov_x ),
			structSize - offsetof( vr_shared_t, fov_x ) );
}

static void VR_SyncOutCG( const vr_shared_t *s ) {
	vr.weapon_select = s->weapon_select;
	vr.weapon_select_autoclose = s->weapon_select_autoclose;
	vr.weapon_select_using_thumbstick = s->weapon_select_using_thumbstick;
	vr.weapon_adjust = s->weapon_adjust;
	vr.weapon_stabilised = s->weapon_stabilised;
	vr.snapTurnYaw = s->snapTurnYaw;
	vr.realign = s->realign;
	vr.recenter_follow_camera = s->recenter_follow_camera;
	VectorCopy( s->clientviewangles, vr.clientviewangles );
	VectorCopy( s->calculated_weaponangles, vr.calculated_weaponangles );
	vr.vote_active = s->vote_active;
	vr.sp_intermission_hud_origin[0] = s->sp_intermission_hud_origin[0];
	vr.sp_intermission_hud_origin[1] = s->sp_intermission_hud_origin[1];
	vr.sp_intermission_hud_origin[2] = s->sp_intermission_hud_origin[2];
	vr.sp_intermission_hud_radius = s->sp_intermission_hud_radius;
	vr.probeEcho = s->probeEcho;
}

static void VR_SyncOutUI( const vr_shared_t *s ) {
	vr.menuYaw = s->menuYaw;
	vr.menuYawLocked = s->menuYawLocked;
	vr.menuCursorActive = s->menuCursorActive;
	vr.scoreboardCursorActive = s->scoreboardCursorActive;
}

static void VR_SyncOutCFG( const vr_shared_t *s ) {
	vr.no_crosshair = s->no_crosshair;
	vr.local_server = s->local_server;
	vr.single_player = s->single_player;
}

void VR_SharedSyncOut( const vr_shared_t *src, int writer, int structSize ) {
	// Read only the module-declared bytes (clamped/captured at registration),
	// zero beyond; a hostile or older module can shrink what we read but never
	// push us past its block.
	vr_shared_t scratch;
	const vr_shared_t *s = &scratch;
	if ( structSize > (int)sizeof( scratch ) )
		structSize = (int)sizeof( scratch );
	if ( structSize < 0 )
		structSize = 0;
	memset( &scratch, 0, sizeof( scratch ) );
	memcpy( &scratch, src, structSize );
	switch ( writer ) {
	case VR_WRITER_CGAME:
		VR_SyncOutCG( s );
		VR_SyncOutUI( s );
		VR_SyncOutCFG( s );
		break;
	case VR_WRITER_UI:
		VR_SyncOutUI( s );
		break;
	case VR_WRITER_GAME:
		VR_SyncOutCFG( s );
		break;
	default:
		Com_Error( ERR_FATAL, "VR_SharedSyncOut: bad writer %d", writer );
	}
}

// A uiShared writer went away without a clean shutdown (error drop, forced
// unload, restart): release the interaction latches it may hold, or the renderer
// stays locked out of menuYaw re-anchoring and input stays routed to the cursor
// branches. menuYaw needs no reset; the renderer re-anchors it once unlocked.
void VR_SharedModuleUnloaded( int writer ) {
	if ( writer == VR_WRITER_UI ) vr.menuCursorActive = qfalse;
	if ( writer == VR_WRITER_CGAME || writer == VR_WRITER_UI ) {
		vr.menuYawLocked = qfalse;
		vr.scoreboardCursorActive = qfalse;
	}
	// cgame owns these; left set, they pin the input router in weapon adjust or a vote after the game is gone
	if ( writer == VR_WRITER_CGAME ) {
		vr.weapon_adjust = qfalse;
		vr.vote_active = qfalse;
	}
}
