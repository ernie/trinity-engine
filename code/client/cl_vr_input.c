#include "../vrcommon/vr_float.h"
#include "client.h"
#include "cl_vr_input.h"
#include "cl_vr.h"
#include "cl_bhaptics.h"
#include "../vrcommon/vr_state.h"
#include "../vrcommon/vr_input_types.h"

#define VR_INPUT_SLOTS 30
static clBHaptics_t suitHaptics;
static qboolean suitEnabled;
static cvar_t *vrSensitivity;
static cvar_t *cgStereoSeparation;
static const char *bindingSlots[VR_INPUT_SLOTS] = {
	"PRIMARYGRIP", "SECONDARYGRIP", "PRIMARYTRIGGER", "SECONDARYTRIGGER",
	"PRIMARYTHUMBSTICK", "SECONDARYTHUMBSTICK", "A", "B", "X", "Y",
	"RTHUMBLEFT", "RTHUMBRIGHT", "RTHUMBFORWARD", "RTHUMBBACK",
	"RTHUMBFORWARDRIGHT", "RTHUMBBACKRIGHT", "RTHUMBBACKLEFT", "RTHUMBFORWARDLEFT",
	"PRIMARYTRACKPAD", "SECONDARYTRACKPAD", "PRIMARYTHUMBREST", "SECONDARYTHUMBREST",
	"LBUMPER", "RBUMPER", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
	"PRIMARYGRIPCLICK", "SECONDARYGRIPCLICK"
};
static cvar_t *bindings[VR_INPUT_SLOTS][2];
/* The runtime's FOV, before VRInput_PublishFov decides what the frame sees */
static float rawFovX = 90, rawFovUp = (float)M_PI / 4, rawFovDown = -(float)M_PI / 4;
static struct {
	clXRInputSample_t sample;
	qboolean valid, tracked, menuDown, viewDown;
	int time, previousTime;
	int navKey, navNext, navAnchorX, navAnchorY;
	int triggerKey[2], adjustStart, dualGripStart;
	qboolean triggers[2], triggerDelivered[2], triggerKeyboard[2], dualGripHeld;
	qboolean physical[VR_INPUT_SLOTS];
	qboolean faceSpace;
	qboolean scrubGrip, scrubNeedsRelease, scrubReserved;
	int hapticEnd[2];
	int scrubHand;
	char held[VR_INPUT_SLOTS][128];
	unsigned heldMask;
} input;

const char *CL_VRInput_MenuSkipName( void ) {
	return bindingSlots[6]; // hands[1]'s primary face button, fixed regardless of vr_righthanded
}
const char *CL_VRInput_MenuCancelName( void ) {
	return "MENU";
}

/* The virtual screen is a monitor: the player's cg_fov over a symmetric crop, so its crop and 2D
 * center geometrically; derived from vr.virtual_screen so the two agree within a frame. */
static void VRInput_PublishFov( void ) {
	if ( vr.virtual_screen ) {
		float fovX = Cvar_VariableValue( "cg_fov" );
		if ( !(fovX >= 1) )
			fovX = 90;
		else if ( fovX > 160 )
			fovX = 160;
		vr.fov_x = fovX;
		vr.fov_angle_up = (rawFovUp - rawFovDown) * .5f;
		vr.fov_angle_down = -vr.fov_angle_up;
	} else {
		vr.fov_x = rawFovX;
		vr.fov_angle_up = rawFovUp;
		vr.fov_angle_down = rawFovDown;
	}
}

void CL_VRInput_SetVirtualScreen( qboolean enabled ) {
	vr.virtual_screen = enabled;
	VRInput_PublishFov();
}

static float VRInput_Cvar( const cvar_t *cv, float fallback ) {
	return cv && VR_FloatFinite( cv->value ) ? cv->value : fallback;
}

float CL_VRInput_StickCurve( float value, float deadzone ) {
	float magnitude;
	if ( !VR_FloatFinite( value ) || !VR_FloatFinite( deadzone ) )
		return 0;
	magnitude = fabsf( value );
	if ( deadzone < 0 )
		deadzone = 0;
	if ( deadzone > .95f )
		deadzone = .95f;
	if ( magnitude <= deadzone )
		return 0;
	if ( magnitude > 1 )
		magnitude = 1;
	return (value < 0 ? -1 : 1) * (magnitude - deadzone) / (1 - deadzone);
}

void CL_VRInput_QuaternionAngles( const float quaternion[4], float gripPitch, vec3_t angles ) {
	float q[4], length, x, y, z, w, s, c;
	float forward[3], rightZ, upZ;
	int i;
	if ( !VR_FloatsFinite( quaternion, 4 ) || !VR_FloatFinite( gripPitch ) ) {
		VectorClear( angles );
		return;
	}
	length = sqrtf( quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
					quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3] );
	if ( !VR_FloatFinite( length ) || length < .00001f ) {
		angles[0] = angles[1] = angles[2] = 0;
		return;
	}
	for ( i = 0; i < 4; i++ )
		q[i] = quaternion[i] / length;
	/* Postmultiply a local X rotation for the grip-to-aim correction. */
	s = sinf( gripPitch * (float)M_PI / 360 );
	c = cosf( gripPitch * (float)M_PI / 360 );
	x = q[0] * c + q[3] * s;
	y = q[1] * c + q[2] * s;
	z = q[2] * c - q[1] * s;
	w = q[3] * c - q[0] * s;
	/* XR forward -Z/right +X/up +Y -> Q3 forward -Z/left -X/up +Y. */
	forward[0] = 1 - 2 * (x * x + y * y);
	forward[1] = 2 * (x * z + w * y);
	forward[2] = 2 * (w * x - y * z);
	rightZ = 2 * (x * y + w * z);
	upZ = 1 - 2 * (x * x + z * z);
	angles[PITCH] =
		atan2f( -forward[2], sqrtf( forward[0] * forward[0] + forward[1] * forward[1] ) ) * 180 / (float)M_PI;
	angles[YAW] = atan2f( forward[1], forward[0] ) * 180 / (float)M_PI;
	angles[ROLL] = atan2f( -rightZ, upZ ) * 180 / (float)M_PI;
}

static qboolean VRInput_Direct( const char *action ) {
	return !strcmp( action, "+attack" ) || !strcmp( action, "+moveup" ) || !strcmp( action, "+movedown" ) ||
			!strncmp( action, "+button", 7 ) || !strcmp( action, "+speed" );
}
static void VRInput_Action( const char *action, qboolean down, int slot ) {
	if ( !action[0] || !strcmp( action, "blank" ) || VRInput_Direct( action ) || !strcmp( action, "+alt" ) ||
		!strcmp( action, "+weapon_select" ) || !strcmp( action, "+weapon_stabilise" ) )
		return;
	if ( action[0] == '+' )
		Cbuf_AddText( va( "%c%s %d %d\n", down ? '+' : '-', action + 1, 400 + slot, cls.realtime ) );
	else if ( down ) {
		if ( !strcmp( action, "turnleft" ) || !strcmp( action, "turnright" ) || !strcmp( action, "uturn" ) ) {
			float turn = vr_snapturn->integer == 1 ? 45 : VRInput_Cvar( vr_snapturn, 45 );
			if ( !strcmp( action, "uturn" ) )
				turn = 180;
			else if ( VRInput_Cvar( vr_snapturn, 45 ) <= 0 )
				return;
			if ( !strcmp( action, "turnright" ) )
				turn = -turn;
			cl.viewangles[YAW] += turn;
		} else
			Cbuf_AddText( va( "%s\n", action ) );
	}
}
enum {
	HELD_ATTACK = 1 << 0,
	HELD_MOVEUP = 1 << 1,
	HELD_MOVEDOWN = 1 << 2,
	HELD_SPEED = 1 << 3,
	HELD_WEAPON_SELECT = 1 << 4,
	HELD_WEAPON_STABILISE = 1 << 5,
	HELD_BUTTON0 = 1 << 8 // +button0 .. +button11 occupy bits 8 to 19
};

static unsigned VRInput_Classify( const char *action ) {
	if ( !strcmp( action, "+attack" ) ) return HELD_ATTACK;
	if ( !strcmp( action, "+moveup" ) ) return HELD_MOVEUP;
	if ( !strcmp( action, "+movedown" ) ) return HELD_MOVEDOWN;
	if ( !strcmp( action, "+speed" ) ) return HELD_SPEED;
	if ( !strcmp( action, "+weapon_select" ) ) return HELD_WEAPON_SELECT;
	if ( !strcmp( action, "+weapon_stabilise" ) ) return HELD_WEAPON_STABILISE;
	if ( !Q_strncmp( action, "+button", 7 ) ) {
		int n = atoi( action + 7 );
		char exact[16];
		// atoi tolerates leading zeros and trailing junk; a round trip keeps this an exact match.
		Com_sprintf( exact, sizeof( exact ), "+button%d", n );
		if ( n >= 0 && n < 12 && !strcmp( action, exact ) )
			return HELD_BUTTON0 << n;
	}
	return 0;
}

static unsigned VRInput_HeldMask( void ) {
	unsigned mask = 0;
	int i;
	for ( i = 0; i < VR_INPUT_SLOTS; i++ )
		mask |= VRInput_Classify( input.held[i] );
	return mask;
}
void CL_VRInput_Reset( void ) {
	int i;
	qboolean needsRelease = input.scrubGrip || input.scrubNeedsRelease;

	CL_BHaptics_Stop( &suitHaptics );
	if ( input.scrubGrip )
		Cbuf_AddText( "tv_scrub_cancel\n" );
	for ( i = 0; i < VR_INPUT_SLOTS; i++ )
		VRInput_Action( input.held[i], qfalse, i );
	input.heldMask = 0;
	if ( input.navKey )
		CL_KeyEvent( input.navKey, qfalse, cls.realtime );
	if ( input.faceSpace )
		CL_KeyEvent( K_SPACE, qfalse, cls.realtime );
	for ( i = 0; i < 2; i++ )
		if ( input.triggerDelivered[i] ) {
			if ( input.triggerKeyboard[i] )
				VKeyboard_HandleOffhandKey( qfalse );
			else
				CL_KeyEvent( input.triggerKey[i], qfalse, cls.realtime );
		}
	memset( &input, 0, sizeof( input ) );
	input.scrubNeedsRelease = needsRelease;
	input.scrubReserved = needsRelease;
	vr.weapon_select = vr.weapon_select_using_thumbstick = vr.weapon_select_autoclose = qfalse;
	vr.weapon_stabilised = vr.walking = vr.vkbOffhandTriggerDown = qfalse;
	vr.vote_holding = vr.menuStickNavActive = 0;
	vr.clientview_yaw_delta = 0;
	VectorClear( vr.hmdposition_delta );
	VectorClear( vr.hmdorientation_delta );
	vr.thumbstick_location[0][0] = vr.thumbstick_location[0][1] = 0;
	vr.thumbstick_location[1][0] = vr.thumbstick_location[1][1] = 0;
}
void CL_VRInput_Shutdown( void ) {
	CL_BHaptics_Close( &suitHaptics );
	suitEnabled = qfalse;
}
void CL_VRInput_Init( void ) {
	int i, alternate;
	CL_VRInput_Shutdown();
	VR_InitCvars();
	CL_VRInput_Reset();
	vr.follow_mode = VRFM_THIRDPERSON_1;
	vr.menuCursorX = vr.offhandCursorX = 320;
	vr.menuCursorY = vr.offhandCursorY = 240;
	for ( i = 0; i < VR_INPUT_SLOTS; i++ ) {
		for ( alternate = 0; alternate < 2; alternate++ )
			bindings[i][alternate] = Cvar_Get(
				va( "vr_button_map_%s%s", bindingSlots[i], alternate ? "_ALT" : "" ), "", CVAR_ARCHIVE );
	}
	/* Share the existing mouse/VR UI setting; keep the engine's default. */
	vrSensitivity = Cvar_Get( "vr_sensitivity", "100", CVAR_ARCHIVE );
	cgStereoSeparation = Cvar_Get( "cg_stereoSeparation", "0", 0 );
	/* Modules may initialize before the first predicted frame. Keep projection
	 * denominators usable until the runtime supplies its actual stereo FOV. */
	vr.weapon_zoomLevel = 1;
	vr.fov_y = rawFovX = 90;
	vr.fov_angle_left = rawFovDown = -(float)M_PI / 4;
	vr.fov_angle_right = rawFovUp = (float)M_PI / 4;
	VRInput_PublishFov();
	vr.eye_fov_angle_left[0] = vr.eye_fov_angle_left[1] = vr.fov_angle_left;
	vr.eye_fov_angle_right[0] = vr.eye_fov_angle_right[1] = vr.fov_angle_right;
}

void CL_VRInput_HapticEvent( const char *event, int position, int flags, int intensity, float angle,
							float height ) {
	int channels, duration = 0, hand;
	float amplitude = 0;
	(void)flags;
	if ( !event || !input.valid || !VR_IsActiveMode() || !vr_hapticIntensity )
		return;
	if ( vr_bhaptics && vr_bhaptics->integer )
		CL_BHaptics_Event( &suitHaptics, event, position, intensity, angle, height,
							VRInput_Cvar( vr_hapticIntensity, .5f ), vr.right_handed, vr.menuLeftHanded );
	if ( !re.XRHaptic )
		return;
	channels = vr.weapon_stabilised ? 3 : (vr.right_handed ? 2 : 1);
	if ( !strcmp( event, "pickup_shield" ) || !strcmp( event, "pickup_weapon" ) || strstr( event, "pickup_item" ) ) {
		duration = 100;
		channels = 3;
		amplitude = .6f;
	} else if ( !strcmp( event, "weapon_switch" ) ) {
		duration = 150;
		channels = vr.right_handed ? 2 : 1;
		amplitude = .6f;
	} else if ( !strcmp( event, "shotgun" ) || !strcmp( event, "fireball" ) ) {
		duration = 250;
		channels = 3;
		amplitude = .85f;
	} else if ( !strcmp( event, "bullet" ) ) {
		duration = 150;
		channels = 3;
		amplitude = .65f;
	} else if ( !strcmp( event, "chainsaw_fire" ) ) {
		duration = 250;
		amplitude = .9f;
	} else if ( !strcmp( event, "tesla_fire" ) || !strcmp( event, "machinegun_fire" ) ) {
		duration = 150;
		amplitude = .5f;
	} else if ( !strcmp( event, "plasmagun_fire" ) ) {
		duration = 90;
		amplitude = .75f;
	} else if ( !strcmp( event, "shotgun_fire" ) ) {
		duration = 150;
		amplitude = 1;
	} else if ( !strcmp( event, "rocket_fire" ) || !strcmp( event, "railgun_fire" ) ||
				!strcmp( event, "bfg_fire" ) || !strcmp( event, "handgrenade_fire" ) ) {
		duration = 250;
		amplitude = 1;
	} else if ( !strcmp( event, "selector_icon" ) ) {
		duration = 50;
		channels = vr.right_handed ? 2 : 1;
		amplitude = .6f;
	} else if ( !strcmp( event, "menu_move" ) ) {
		duration = 30;
		channels = vr.menuLeftHanded ? 1 : 2;
		amplitude = .3f;
	}
	/* Let an active pulse finish instead of restarting the motor on every event. */
	for ( hand = 0; hand < 2; hand++ ) {
		if ( duration && (channels & (1 << hand)) && cls.realtime >= input.hapticEnd[hand] ) {
			if ( re.XRHaptic( hand, Com_Clamp( 0, 1, amplitude * VRInput_Cvar( vr_hapticIntensity, .5f ) ), duration ) )
				input.hapticEnd[hand] = cls.realtime + duration;
		}
	}
}

static void VRInput_Cursor( const vec3_t aim, int *x, int *y ) {
	float yaw = Com_Clamp( -85, 85, AngleSubtract( aim[YAW], vr.sp_intermission_active ? vr.sp_intermission_yaw : vr.menuYaw ) );
	float pitch = Com_Clamp( -85, 85, aim[PITCH] );
	/* Quantize the target before smoothing to keep stationary cursors stable. */
	int targetX = (int)Com_Clamp( -8000, 8000, 320 - tanf( yaw * (float)M_PI / 180 ) * 800 );
	int targetY = (int)Com_Clamp( -8000, 8000, 240 + tanf( pitch * (float)M_PI / 180 ) * 800 );
	*x = (int)(.5f * targetX + .5f * *x);
	*y = (int)(.5f * targetY + .5f * *y);
}

/* The hand's aim ray on the virtual screen; a miss holds the cursor where it last was. */
static void VRInput_ScreenCursor( const refXRFrame_t *frame, int hand, int *x, int *y ) {
	const clXRPose_t *aim = &frame->input.hands[hand].aim;
	const float *q = aim->orientation;
	float forward[3], uv[2];
	int targetX = *x, targetY = *y;
	if ( aim->positionValid && aim->orientationValid ) {
		forward[0] = -2 * (q[0] * q[2] + q[3] * q[1]);
		forward[1] = -2 * (q[1] * q[2] - q[3] * q[0]);
		forward[2] = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
		if ( VR_ScreenRay( &frame->screen, aim->position, forward, uv ) ) {
			targetX = (int)(uv[0] * 640);
			targetY = (int)(uv[1] * 480);
		}
	}
	*x = (int)(.5f * targetX + .5f * *x);
	*y = (int)(.5f * targetY + .5f * *y);
}

static void VRInput_MenuNav( const refXRFrame_t *frame, qboolean menu ) {
	float ax = fabsf( frame->input.hands[0].stick[0] ) >= fabsf( frame->input.hands[1].stick[0] )
					? frame->input.hands[0].stick[0]
					: frame->input.hands[1].stick[0];
	float ay = fabsf( frame->input.hands[0].stick[1] ) >= fabsf( frame->input.hands[1].stick[1] )
					? frame->input.hands[0].stick[1]
					: frame->input.hands[1].stick[1];
	float magnitude = fmaxf( fabsf( ax ), fabsf( ay ) );
	int desired = 0;
	if ( menu ) {
		if ( fabsf( ax ) >= fabsf( ay ) ) {
			if ( ax > .5f )
				desired = K_RIGHTARROW;
			else if ( ax < -.5f )
				desired = K_LEFTARROW;
		} else if ( ay > .5f )
			desired = Key_GetCatcher() & KEYCATCH_CONSOLE ? K_PGUP : K_UPARROW;
		else if ( ay < -.5f )
			desired = Key_GetCatcher() & KEYCATCH_CONSOLE ? K_PGDN : K_DOWNARROW;
	}
	if ( input.navKey && (!menu || magnitude < .35f || (desired && desired != input.navKey)) ) {
		CL_KeyEvent( input.navKey, qfalse, cls.realtime );
		input.navKey = 0;
	}
	if ( !menu ) {
		vr.menuStickNavActive = qfalse;
		return;
	}
	if ( desired && !input.navKey ) {
		input.navAnchorX = vr.menuCursorX;
		input.navAnchorY = vr.menuCursorY;
		vr.menuStickNavActive = qtrue;
		input.navKey = desired;
		input.navNext = cls.realtime + 400;
		CL_KeyEvent( desired, qtrue, cls.realtime );
	} else if ( desired && desired == input.navKey && cls.realtime >= input.navNext ) {
		CL_KeyEvent( desired, qtrue, cls.realtime );
		input.navNext = cls.realtime + 140;
	}
	if ( vr.menuStickNavActive && !input.navKey ) {
		int dx = vr.menuCursorX - input.navAnchorX, dy = vr.menuCursorY - input.navAnchorY;
		if ( dx * dx + dy * dy > 3600 )
			vr.menuStickNavActive = qfalse;
	}
}

static void VRInput_MenuTriggers( const refXRFrame_t *frame, qboolean menu ) {
	int hand;
	float press = 1 - VRInput_Cvar( vr_triggerSensitivity, .25f ), release = fmaxf( .1f, press - .25f );
	for ( hand = 0; hand < 2; hand++ ) {
		qboolean down = input.triggers[hand];
		float value = frame->input.hands[hand].trigger;
		if ( !frame->input.hands[hand].active || value < release )
			down = qfalse;
		else if ( value > press )
			down = qtrue;
		if ( input.triggerDelivered[hand] && (!menu || !down) ) {
			if ( input.triggerKeyboard[hand] ) {
				VKeyboard_HandleOffhandKey( qfalse );
				vr.vkbOffhandTriggerDown = qfalse;
			} else
				CL_KeyEvent( input.triggerKey[hand], qfalse, cls.realtime );
			input.triggerDelivered[hand] = qfalse;
		}
		if ( menu && down && !input.triggers[hand] ) {
			qboolean selected = hand == (vr.menuLeftHanded ? 0 : 1);
			if ( !selected && !VKeyboard_IsActive() )
				vr.menuLeftHanded = hand == 0;
			else if ( !frame->screen.visible || vr.menuStickNavActive ||
						(selected ? vr.menuCursorActive : vr.offhandCursorX >= 0) ) {
				input.triggerKeyboard[hand] = !selected;
				input.triggerKey[hand] = vr.menuStickNavActive && !VKeyboard_IsActive() ? K_ENTER : K_MOUSE1;
				input.triggerDelivered[hand] = qtrue;
				if ( !selected ) {
					vr.vkbOffhandTriggerDown = qtrue;
					VKeyboard_HandleOffhandKey( qtrue );
				} else
					CL_KeyEvent( input.triggerKey[hand], qtrue, cls.realtime );
			}
		}
		input.triggers[hand] = down;
	}
}

static qboolean VRInput_MenuPressed( const refXRFrame_t *frame ) {
	return ((frame->input.hands[0].buttons | frame->input.hands[1].buttons) & CL_XRI_MENU_BUTTON) != 0;
}

/* Cgame owns the timeline catcher; losing the playback context cancels the seek. */
static qboolean VRInput_TVScrub( const refXRFrame_t *frame, const qboolean *pressed, int primary ) {
	qboolean blocked = input.scrubGrip || vr.menuYawLocked;
	qboolean eligible =
		tvPlay.active && tvPlay.totalDuration > 0 && !vr.weapon_adjust && !vr.in_menu &&
		!VKeyboard_IsActive() && !(Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CONSOLE | KEYCATCH_MESSAGE)) &&
		!VRInput_MenuPressed( frame ) && frame->input.hands[primary].active;
	if ( input.scrubGrip && (!eligible || input.scrubHand != primary) ) {
		Cbuf_AddText( "tv_scrub_cancel\n" );
		input.scrubGrip = qfalse;
		input.scrubNeedsRelease = qtrue;
	}
	if ( input.scrubNeedsRelease && frame->input.hands[primary].active &&
		frame->input.hands[1 - primary].active && !pressed[0] && !pressed[1] )
		input.scrubNeedsRelease = qfalse;
	if ( !eligible || input.scrubNeedsRelease )
		return blocked;
	if ( pressed[1] && !input.physical[1] ) {
		if ( input.scrubGrip || vr.menuYawLocked )
			Cbuf_AddText( "tv_scrub_cancel\n" );
		input.scrubGrip = qfalse;
		input.scrubNeedsRelease = qtrue;
	} else if ( pressed[0] && !input.physical[0] && !vr.menuYawLocked ) {
		Cbuf_AddText( "+tv_scrub\n" );
		input.scrubGrip = qtrue;
		input.scrubHand = primary;
	}
	if ( input.scrubGrip && !pressed[0] ) {
		Cbuf_AddText( "-tv_scrub\n" );
		input.scrubGrip = qfalse;
	}
	return blocked || input.scrubGrip;
}

void CL_VRInput_Frame( const refXRFrame_t *frame ) {
	qboolean pressed[VR_INPUT_SLOTS], alt, wheel, menu, priorWheel, scrubBlocked, tvGrips;
	vec3_t oldAngles, oldPosition;
	int i, primary, secondary, turning, altHeld = 0;
	const clXRHandInput_t *weapon, *other;
	if ( !frame || !VR_IsActiveMode() || !frame->running || !frame->renderable ) {
		CL_VRInput_Reset();
		return;
	}
	input.sample = frame->input;
	input.valid = qtrue;
	input.previousTime = input.time;
	input.time = cls.realtime;
	vr.use_6dof = vr.single_player && vr_6dof->integer;
	VectorCopy( vr.hmdorientation, oldAngles );
	VectorCopy( vr.hmdposition, oldPosition );
	CL_VRInput_QuaternionAngles( frame->head.orientation, 0, vr.hmdorientation );
	if ( !vr.menuYawLocked && !frame->screen.visible )
		vr.menuYaw = vr.hmdorientation[YAW];
	VectorCopy( frame->head.position, vr.hmdposition );
	vr.hmdposition[1] += VRInput_Cvar( vr_heightAdjust, 0 );
	if ( input.tracked ) {
		for ( i = 0; i < 3; i++ )
			vr.hmdorientation_delta[i] = AngleSubtract( oldAngles[i], vr.hmdorientation[i] );
		VectorSubtract( oldPosition, vr.hmdposition, vr.hmdposition_delta );
		cl.viewangles[YAW] -= vr.hmdorientation_delta[YAW];
	} else {
		VectorClear( vr.hmdorientation_delta );
		VectorClear( vr.hmdposition_delta );
		VectorCopy( vr.hmdposition, vr.hmdorigin );
		vr.clientview_yaw_last = cl.viewangles[YAW] - vr.hmdorientation[YAW];
	}
	input.tracked = qtrue;
	cl.viewangles[PITCH] = vr.hmdorientation[PITCH];
	cl.viewangles[ROLL] = Com_Clamp( -60, 60, vr.hmdorientation[ROLL] );
	if ( vr.snapTurnYaw ) {
		cl.viewangles[YAW] -= vr.snapTurnYaw;
		vr.snapTurnYaw = 0;
	}
	vr.right_handed = vr_righthanded->integer != 0;
	primary = vr.right_handed ? 1 : 0;
	secondary = 1 - primary;
	turning = vr_switchThumbsticks->integer ? 0 : 1;
	weapon = &frame->input.hands[primary];
	other = &frame->input.hands[secondary];
	for ( i = 0; i < 2; i++ ) {
		vr.eye_fov_angle_left[i] = frame->eyes[i].fov[0];
		vr.eye_fov_angle_right[i] = frame->eyes[i].fov[1];
		vr.thumbstick_location[i][0] = frame->input.hands[i].stick[0];
		vr.thumbstick_location[i][1] = frame->input.hands[i].stick[1];
	}
	vr.fov_angle_left = (frame->eyes[0].fov[0] + frame->eyes[1].fov[0]) * .5f;
	vr.fov_angle_right = (frame->eyes[0].fov[1] + frame->eyes[1].fov[1]) * .5f;
	rawFovUp = (frame->eyes[0].fov[2] + frame->eyes[1].fov[2]) * .5f;
	rawFovDown = (frame->eyes[0].fov[3] + frame->eyes[1].fov[3]) * .5f;
	rawFovX = (vr.fov_angle_right - vr.fov_angle_left) * 180 / (float)M_PI;
	VRInput_PublishFov();
	vr.fov_y = (rawFovUp - rawFovDown) * 180 / (float)M_PI;
	if ( weapon->grip.positionValid && weapon->grip.orientationValid ) {
		VectorCopy( vr.weaponoffset_last[1], vr.weaponoffset_last[0] );
		VectorCopy( vr.weaponoffset, vr.weaponoffset_last[1] );
		VectorCopy( weapon->grip.position, vr.weaponposition );
		vr.weaponposition[1] += VRInput_Cvar( vr_heightAdjust, 0 );
		VectorSubtract( vr.weaponposition, vr.hmdposition, vr.weaponoffset );
		CL_VRInput_QuaternionAngles( weapon->grip.orientation, -90 + weapon->pitchCorrection + VRInput_Cvar( vr_weaponPitch, 0 ),
									vr.weaponangles );
	}
	if ( other->grip.positionValid && other->grip.orientationValid ) {
		VectorCopy( other->grip.position, vr.offhandposition );
		vr.offhandposition[1] += VRInput_Cvar( vr_heightAdjust, 0 );
		VectorSubtract( vr.offhandposition, vr.hmdposition, vr.offhandoffset );
		CL_VRInput_QuaternionAngles( other->grip.orientation, -90 + other->pitchCorrection + VRInput_Cvar( vr_weaponPitch, 0 ),
									vr.offhandangles );
	}
	if ( weapon->aim.orientationValid )
		CL_VRInput_QuaternionAngles( weapon->aim.orientation, 0, vr.weaponaimangles );
	if ( other->aim.orientationValid )
		CL_VRInput_QuaternionAngles( other->aim.orientation, 0, vr.offhandaimangles );
	/* A visible session renders valid tracked views without input focus.
	 * Publishing their FOV/poses is independent of allowing gameplay actions. */
	if ( !frame->focused || !frame->input.focused ) {
		CL_VRInput_Reset();
		input.tracked = qtrue;
		input.time = cls.realtime;
		VectorCopy( cl.viewangles, vr.clientviewangles );
		return;
	}
	if ( suitEnabled != (vr_bhaptics && vr_bhaptics->integer != 0) ) {
		suitEnabled = vr_bhaptics && vr_bhaptics->integer != 0;
		if ( suitEnabled )
			CL_BHaptics_Open( &suitHaptics );
		else
			CL_BHaptics_Close( &suitHaptics );
	}
	vr.in_menu = (Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CONSOLE)) != 0;
	menu = vr.in_menu || cls.state != CA_ACTIVE || (vr.virtual_screen && !vr.first_person_following) ||
			cl.snap.ps.pm_type == PM_INTERMISSION || vr.scoreboardCursorActive;
	if ( menu || VKeyboard_IsActive() ) {
		qboolean menuWeapon = primary == (vr.menuLeftHanded ? 0 : 1);
		vr.menuCursorActive = frame->input.hands[vr.menuLeftHanded ? 0 : 1].aim.orientationValid;
		if ( vr.menuCursorActive ) {
			/* Smooth once for each tracked controller sample. */
			for ( i = 0; i < 2; i++ )
				if ( frame->input.hands[i].grip.positionValid ) {
					if ( frame->screen.visible ) {
						VRInput_ScreenCursor( frame, vr.menuLeftHanded ? 0 : 1, &vr.menuCursorX, &vr.menuCursorY );
						if ( VKeyboard_IsActive() )
							VRInput_ScreenCursor( frame, vr.menuLeftHanded ? 1 : 0, &vr.offhandCursorX,
												  &vr.offhandCursorY );
						continue;
					}
					VRInput_Cursor( menuWeapon ? vr.weaponaimangles : vr.offhandaimangles, &vr.menuCursorX,
									&vr.menuCursorY );
					if ( VKeyboard_IsActive() )
						VRInput_Cursor( menuWeapon ? vr.offhandaimangles : vr.weaponaimangles,
										&vr.offhandCursorX, &vr.offhandCursorY );
				}
		}
		if ( vr.scoreboardCursorActive ) {
			if ( frame->screen.visible ) {
				vr.scoreboardCursorX = vr.menuCursorX;
				vr.scoreboardCursorY = vr.menuCursorY;
			} else {
				const float *aim = menuWeapon ? vr.weaponaimangles : vr.offhandaimangles;
				vr.scoreboardCursorX = (int)Com_Clamp(
					0, 640,
					320 - tanf( Com_Clamp( -85, 85, AngleSubtract( aim[YAW], vr.menuYaw ) ) * (float)M_PI / 180 ) *
								400 );
				vr.scoreboardCursorY = (int)Com_Clamp(
					0, 480, 240 + tanf( Com_Clamp( -85, 85, aim[PITCH] ) * (float)M_PI / 180 ) * 400 );
			}
		}
	}
	/* Keep console scrolling and caret navigation available while
	 * the virtual keyboard is open. It uses pointing, not the thumbsticks. */
	VRInput_MenuNav( frame, (vr.in_menu || (vr.virtual_screen && !vr.first_person_following)) &&
								!vr.weapon_adjust && !vr.menuYawLocked );
	/* The UI refresh draws the pointer, but UI_MOUSE_EVENT updates hover/focus.
	 * Deliver it before a same-frame trigger click, with stick ownership already
	 * published so pointing cannot undo navigation selection. */
	if ( (Key_GetCatcher() & KEYCATCH_UI) && vr.menuCursorActive && !vr.menuStickNavActive &&
		!VKeyboard_IsActive() && !vr.weapon_adjust && !vr.menuYawLocked )
		CL_MouseEvent( 0, 0 );
	VRInput_MenuTriggers( frame, (menu || VKeyboard_IsActive()) && !vr.weapon_adjust && !vr.menuYawLocked );
	if ( VRInput_MenuPressed( frame ) && !input.menuDown ) {
		CL_KeyEvent( K_ESCAPE, qtrue, cls.realtime );
		CL_KeyEvent( K_ESCAPE, qfalse, cls.realtime );
	}
	input.menuDown = VRInput_MenuPressed( frame );
	// Hard-wired like Menu: slot bindings are suppressed while the console is up, so a slot could not close it
	if ( (frame->input.hands[0].buttons & CL_XRI_VIEW_BUTTON) && !input.viewDown )
		Cbuf_AddText( "toggleconsole\n" );
	input.viewDown = !!(frame->input.hands[0].buttons & CL_XRI_VIEW_BUTTON);
	pressed[0] = weapon->squeeze > .5f;
	pressed[1] = other->squeeze > .5f;
	pressed[2] = input.triggers[primary];
	pressed[3] = input.triggers[secondary];
	pressed[4] = !!(weapon->buttons & CL_XRI_STICK_BUTTON);
	pressed[5] = !!(other->buttons & CL_XRI_STICK_BUTTON);
	pressed[6] = !!(frame->input.hands[1].buttons & CL_XRI_PRIMARY_BUTTON);
	pressed[7] = !!(frame->input.hands[1].buttons & CL_XRI_SECONDARY_BUTTON);
	pressed[8] = !!(frame->input.hands[0].buttons & CL_XRI_PRIMARY_BUTTON);
	pressed[9] = !!(frame->input.hands[0].buttons & CL_XRI_SECONDARY_BUTTON);
	for ( i = 10; i < VR_INPUT_SLOTS; i++ )
		pressed[i] = qfalse;
	pressed[18] = !!(weapon->buttons & CL_XRI_TRACKPAD_BUTTON);
	pressed[19] = !!(other->buttons & CL_XRI_TRACKPAD_BUTTON);
	pressed[20] = !!(weapon->buttons & CL_XRI_THUMBREST_BUTTON);
	pressed[21] = !!(other->buttons & CL_XRI_THUMBREST_BUTTON);
	/* The Frame puts X/Y on the right controller; they keep the shared X/Y slots */
	pressed[8] = pressed[8] || (frame->input.hands[1].buttons & CL_XRI_X_BUTTON);
	pressed[9] = pressed[9] || (frame->input.hands[1].buttons & CL_XRI_Y_BUTTON);
	pressed[22] = !!(frame->input.hands[0].buttons & CL_XRI_BUMPER_BUTTON);
	pressed[23] = !!(frame->input.hands[1].buttons & CL_XRI_BUMPER_BUTTON);
	pressed[24] = !!(frame->input.hands[0].buttons & CL_XRI_DPAD_UP_BUTTON);
	pressed[25] = !!(frame->input.hands[0].buttons & CL_XRI_DPAD_DOWN_BUTTON);
	pressed[26] = !!(frame->input.hands[0].buttons & CL_XRI_DPAD_LEFT_BUTTON);
	pressed[27] = !!(frame->input.hands[0].buttons & CL_XRI_DPAD_RIGHT_BUTTON);
	pressed[28] = !!(weapon->buttons & CL_XRI_SQUEEZE_CLICK_BUTTON);
	pressed[29] = !!(other->buttons & CL_XRI_SQUEEZE_CLICK_BUTTON);
	for ( i = 0; i < VR_INPUT_SLOTS; i++ )
		if ( !strcmp( input.held[i], "+alt" ) )
			altHeld++;
	{
		float sx = CL_VRInput_StickCurve( frame->input.hands[turning].stick[0],
											VRInput_Cvar( vr_thumbstickDeadzone, .15f ) );
		float sy = CL_VRInput_StickCurve( frame->input.hands[turning].stick[1],
											VRInput_Cvar( vr_thumbstickDeadzone, .15f ) );
		qboolean diagonal = qfalse;
		for ( i = 14; i < 18; i++ ) {
			const char *binding = bindings[i][altHeld != 0]->string;
			if ( binding[0] )
				diagonal = qtrue;
		}
		if ( sqrtf( sx * sx + sy * sy ) > .05f ) {
			float direction = atan2f( sx, sy ) * 180 / (float)M_PI;
			int sector;
			if ( direction < 0 )
				direction += 360;
			if ( diagonal ) {
				static const int index[8] = {12, 14, 11, 15, 13, 16, 10, 17};
				sector = ((int)((direction + 22.5f) / 45)) & 7;
				pressed[index[sector]] = qtrue;
			} else {
				static const int index[4] = {12, 11, 13, 10};
				sector = ((int)((direction + 45) / 90)) & 3;
				pressed[index[sector]] = qtrue;
			}
		}
	}
	scrubBlocked = VRInput_TVScrub( frame, pressed, primary );
	tvGrips = (tvPlay.active && tvPlay.totalDuration > 0) || input.scrubGrip ||
				(input.scrubReserved && (pressed[0] || pressed[1] || input.scrubNeedsRelease));
	input.scrubReserved = tvGrips;
	vr.vote_holding = !vr.vote_active || scrubBlocked || vr.weapon_adjust ? 0
					: pressed[7] ? -1
					: pressed[6] ? 1
					: 0;
	if ( !tvGrips && !scrubBlocked && vr_weaponAdjust->integer && pressed[0] && pressed[1] ) {
		if ( !input.dualGripHeld ) {
			input.dualGripHeld = qtrue;
			input.dualGripStart = cls.realtime;
		} else if ( input.dualGripStart && cls.realtime - input.dualGripStart > 1000 ) {
			Cbuf_AddText( "weapon_adjust\n" );
			input.dualGripStart = 0;
		}
	} else {
		input.dualGripHeld = qfalse;
		input.dualGripStart = 0;
	}
	if ( vr.weapon_adjust ) {
		if ( pressed[6] && !input.physical[6] )
			Cbuf_AddText( "weapon_adjust\n" );
		if ( pressed[7] ) {
			if ( !input.physical[7] )
				input.adjustStart = cls.realtime;
			else if ( input.adjustStart && cls.realtime - input.adjustStart > 2000 ) {
				Cbuf_AddText( "weapon_adjust_reset_all\n" );
				input.adjustStart = 0;
			}
		} else {
			if ( input.physical[7] && input.adjustStart )
				Cbuf_AddText( "weapon_adjust_reset\n" );
			input.adjustStart = 0;
		}
	} else if ( !scrubBlocked ) {
		input.adjustStart = 0;
		if ( pressed[5] && !input.physical[5] )
			vr.realign = 3;
		if ( pressed[4] && !input.physical[4] && (clc.demoplaying || tvPlay.active) )
			Cbuf_AddText( "demopause\n" );
		if ( pressed[6] && !input.physical[6] ) {
			if ( cl.snap.ps.pm_flags & PMF_FOLLOW )
				Cbuf_AddText( "cmd team spectator\n" );
			else if ( tvPlay.active )
				Cbuf_AddText( "tv_view_next\n" );
		}
		if ( pressed[7] && !input.physical[7] ) {
			if ( ((cl.snap.ps.pm_flags & PMF_FOLLOW) || clc.demoplaying) &&
				(vr.follow_mode == VRFM_THIRDPERSON_1 || vr.follow_mode == VRFM_THIRDPERSON_2) )
				vr.recenter_follow_camera = qtrue;
			else {
				vr.menuYaw = vr.hmdorientation[YAW];
				CL_VR_ResetVirtualScreen();
			}
		}
		if ( pressed[8] && !input.physical[8] && ((cl.snap.ps.pm_flags & PMF_FOLLOW) || clc.demoplaying) ) {
			vr.follow_mode++;
			if ( vr.follow_mode >= VRFM_NUM_FOLLOWMODES )
				vr.follow_mode = VRFM_THIRDPERSON_1;
			if ( vr.follow_mode == VRFM_THIRDPERSON_1 && !tvPlay.active )
				Cbuf_AddText( "follow\n" );
			vr.realign = 3;
		}
	}
	if ( menu && !vr.weapon_adjust && !scrubBlocked && pressed[6] != input.faceSpace ) {
		CL_KeyEvent( K_SPACE, pressed[6], cls.realtime );
		input.faceSpace = pressed[6];
	} else if ( (!menu || vr.weapon_adjust || scrubBlocked) && input.faceSpace ) {
		CL_KeyEvent( K_SPACE, qfalse, cls.realtime );
		input.faceSpace = qfalse;
	}
	memcpy( input.physical, pressed, sizeof( pressed ) );
	if ( tvGrips )
		pressed[0] = pressed[1] = qfalse;
	if ( (cl.snap.ps.pm_flags & PMF_FOLLOW) || tvPlay.active )
		pressed[6] = qfalse;
	if ( (cl.snap.ps.pm_flags & PMF_FOLLOW) || clc.demoplaying )
		pressed[8] = qfalse;
	priorWheel = vr.weapon_select;
	{
		qboolean changed = qfalse;
		for ( i = 0; i < VR_INPUT_SLOTS; i++ ) {
			char action[128];
			alt = altHeld != 0;
			Q_strncpyz( action, bindings[i][alt]->string, sizeof( action ) );
			if ( alt && !action[0] )
				Q_strncpyz( action, bindings[i][0]->string, sizeof( action ) );
			if ( !pressed[i] || menu || vr.weapon_adjust || scrubBlocked )
				action[0] = 0;
			if ( strcmp( action, input.held[i] ) ) {
				if ( !strcmp( input.held[i], "+alt" ) )
					altHeld--;
				if ( !strcmp( action, "+alt" ) )
					altHeld++;
				VRInput_Action( input.held[i], qfalse, i );
				Q_strncpyz( input.held[i], action, sizeof( input.held[i] ) );
				VRInput_Action( action, qtrue, i );
				changed = qtrue;
			}
		}
		if ( changed )
			input.heldMask = VRInput_HeldMask();
	}
	wheel = (input.heldMask & HELD_WEAPON_SELECT) && !clc.demoplaying && !(cl.snap.ps.pm_flags & PMF_FOLLOW);
	vr.weapon_select = wheel;
	vr.weapon_select_using_thumbstick = wheel && vr_weaponSelectorMode->integer == WS_HMD;
	vr.weapon_select_autoclose =
		vr.weapon_select_using_thumbstick && (pressed[10] || pressed[11] || pressed[12] || pressed[13]);
	if ( priorWheel && !wheel && !menu )
		Cbuf_AddText( "weapon_select\n" );
	vr.weapon_stabilised = qfalse;
	if ( (input.heldMask & HELD_WEAPON_STABILISE) && weapon->grip.positionValid && other->grip.positionValid ) {
		vec3_t distance;
		VectorSubtract( vr.weaponposition, vr.offhandposition, distance );
		vr.weapon_stabilised = VectorLength( distance ) < .4f;
	}
	vr.weapon_zoomed = !menu && vr_weaponScope->integer && vr.weapon_stabilised &&
						cl.snap.ps.weapon == WP_RAILGUN && VectorLength( vr.weaponoffset ) < .24f &&
						cl.snap.ps.stats[STAT_HEALTH] > 0;
	if ( !menu && vr_twoHandedWeapons->integer && vr.weapon_stabilised ) {
		vec3_t direction;
		if ( vr_twoHandedWeapons->integer == 2 ) {
			float yaw = -vr.hmdorientation[YAW] * (float)M_PI / 180;
			float separation = cgStereoSeparation->value * .5f;
			direction[0] = vr.offhandposition[0] - vr.hmdposition[0] - cosf( yaw ) * separation;
			direction[1] = vr.offhandposition[1] - vr.hmdposition[1] + .1f;
			direction[2] = vr.offhandposition[2] - vr.hmdposition[2] - sinf( yaw ) * separation;
		} else
			for ( i = 0; i < 3; i++ )
				direction[i] =
					vr.offhandoffset[i] -
					(vr.weaponoffset[i] + vr.weaponoffset_last[0][i] + vr.weaponoffset_last[1][i]) / 3;
		if ( fabsf( direction[0] ) + fabsf( direction[2] ) > .0001f ) {
			vr.weaponangles[PITCH] =
				-atan2f( direction[1], sqrtf( direction[0] * direction[0] + direction[2] * direction[2] ) ) *
				180 / (float)M_PI;
			vr.weaponangles[YAW] = -atan2f( direction[0], -direction[2] ) * 180 / (float)M_PI;
			vr.weaponangles[ROLL] = vr_twoHandedWeapons->integer == 2 ? 0 : vr.weaponangles[ROLL] * .5f;
		}
	}
	if ( VRInput_Cvar( vr_snapturn, 45 ) <= 0 && !menu && input.previousTime ) {
		float elapsed = Com_Clamp( 0, 100, input.time - input.previousTime ) * .001f;
		if ( !wheel && !vr.weapon_adjust && !scrubBlocked && vr_weaponSelectorMode->integer != WS_HMD ) {
			float axis = CL_VRInput_StickCurve( frame->input.hands[turning].stick[0],
												VRInput_Cvar( vr_thumbstickDeadzone, .15f ) );
			/* VR sensitivity is independent of mouse sensitivity; 100 is normal speed.
			 * The reference full-stick rate is 32767 * .022 degrees/second. */
			cl.viewangles[YAW] -=
				axis * (32767.0f * .022f) * (VRInput_Cvar( vrSensitivity, 100 ) / 100.0f) * elapsed;
		}
	}
	{
		float yaw = cl.viewangles[YAW] - vr.hmdorientation[YAW];
		vr.clientview_yaw_delta = AngleSubtract( vr.clientview_yaw_last, yaw );
		vr.clientview_yaw_last = yaw;
	}
	VectorCopy( cl.viewangles, vr.clientviewangles );
}

static void CL_VRInput_FinalizePose( usercmd_t *cmd ) {
	int pitch, yaw;

	/* Head orientation rides in bits 12-25, which only a 32-bit command carries. */
	if ( CL_VR_UsercmdButtonBits() != 32 )
		return;
	pitch = (int)((Com_Clamp( -80, 80, vr.hmdorientation[PITCH] ) + 90) * 127 / 180);
	yaw = (int)((Com_Clamp( -80, 80, AngleSubtract( vr.hmdorientation[YAW], vr.weaponangles[YAW] ) ) + 90) * 127 /
				180);
	cmd->buttons |= (pitch & 127) << 12 | (yaw & 127) << 19;
}

qboolean CL_VRInput_ApplyMove( usercmd_t *cmd ) {
	float side, forward, angle, x, magnitude;
	vec3_t angles;
	int moveHand, primary, i, catcher;
	if ( !VR_IsActiveMode() )
		return qfalse;
	memset( cmd, 0, sizeof( *cmd ) );
	cmd->weapon = cl.cgameUserCmdValue;
	cmd->serverTime = cl.serverTime;
	for ( i = 0; i < 3; i++ )
		cmd->angles[i] = ANGLE2SHORT( cl.viewangles[i] );
	/* The catcher owns actions, but command metadata identifies the
	 * headset to the server while a menu, chat, or scoreboard is open. */
	catcher = Key_GetCatcher();
	if ( catcher )
		cmd->buttons |= BUTTON_TALK;
	if ( !input.valid || cls.realtime - input.time > 250 )
		return qtrue;
	if ( catcher || vr.weapon_adjust || vr.menuYawLocked || input.scrubGrip || vr.scoreboardCursorActive ||
		(vr.virtual_screen && !vr.first_person_following) ) {
		CL_VRInput_FinalizePose( cmd );
		return qtrue;
	}
	vr.clientNum = cl.snap.ps.clientNum;
	moveHand = vr_switchThumbsticks->integer ? 1 : 0;
	primary = vr.right_handed ? 1 : 0;
	side = CL_VRInput_StickCurve( input.sample.hands[moveHand].stick[0],
									VRInput_Cvar( vr_thumbstickDeadzone, .15f ) );
	forward = CL_VRInput_StickCurve( input.sample.hands[moveHand].stick[1],
									VRInput_Cvar( vr_thumbstickDeadzone, .15f ) );
	angle = vr_directionMode->integer ? vr.offhandangles[YAW] : vr.hmdorientation[YAW];
	if ( vr.use_6dof )
		angle -= vr.hmdorientation[YAW];
	angle *= (float)M_PI / 180;
	x = side;
	side = cosf( angle ) * x - sinf( angle ) * forward;
	forward = cosf( angle ) * forward + sinf( angle ) * x;
	if ( vr.use_6dof && input.previousTime && input.time > input.previousTime ) {
		/* Convert room-scale metres per millisecond to command movement units. */
		float factor = 10000.0f / (72.0f * (input.time - input.previousTime));
		float px = -vr.hmdposition_delta[0] * factor, py = vr.hmdposition_delta[2] * factor;
		float yaw = -vr.hmdorientation[YAW] * (float)M_PI / 180;
		side += cosf( yaw ) * px - sinf( yaw ) * py;
		forward += cosf( yaw ) * py + sinf( yaw ) * px;
	}
	magnitude = sqrtf( side * side + forward * forward );
	if ( magnitude >= VRInput_Cvar( vr_thumbstickFullDeflection, .85f ) ) {
		float maximum = fmaxf( fabsf( side ), fabsf( forward ) );
		if ( maximum > 0 ) {
			side /= maximum;
			forward /= maximum;
		}
	}
	cmd->rightmove = (signed char)Com_Clamp( -127, 127, side * 127 );
	cmd->forwardmove = (signed char)Com_Clamp( -127, 127, forward * 127 );
	if ( input.heldMask & HELD_MOVEUP )
		cmd->upmove = 127;
	if ( input.heldMask & HELD_MOVEDOWN )
		cmd->upmove = -127;
	if ( vr.use_6dof && cl.snap.ps.pm_type == PM_SPECTATOR && !(cl.snap.ps.pm_flags & PMF_FOLLOW) ) {
		float pitch = vr.offhandangles[PITCH] * (float)M_PI / 180;
		int original = cmd->forwardmove;
		cmd->forwardmove = (signed char)Com_Clamp( -127, 127, original * cosf( pitch ) );
		cmd->upmove = (signed char)Com_Clamp( -127, 127, cmd->upmove - original * sinf( pitch ) );
	}
	if ( (input.heldMask & HELD_ATTACK) && input.sample.hands[primary].aim.orientationValid &&
		input.sample.hands[primary].grip.positionValid )
		cmd->buttons |= BUTTON_ATTACK;
	cmd->buttons |= (input.heldMask >> 8) & 0xFFF; // +button0..11 map to bits 0..11
	if ( !input.sample.hands[primary].aim.orientationValid || !input.sample.hands[primary].grip.positionValid )
		cmd->buttons &= ~BUTTON_ATTACK;
	if ( vr_analogWalk->integer ) {
		int speed = abs( cmd->rightmove ) > abs( cmd->forwardmove ) ? abs( cmd->rightmove ) : abs( cmd->forwardmove );
		if ( vr.walking && speed > 64 )
			vr.walking = qfalse;
		else if ( !vr.walking && speed < 64 - .04f * 127 )
			vr.walking = qtrue;
	} else
		vr.walking = qfalse;
	if ( vr.walking || (input.heldMask & HELD_SPEED) )
		cmd->buttons |= BUTTON_WALKING;
	if ( !vr.use_6dof && input.sample.hands[primary].aim.orientationValid ) {
		if ( vr.realign > 0 && --vr.realign == 0 )
			VectorCopy( vr.hmdposition, vr.hmdorigin );
		VectorCopy( vr.calculated_weaponangles, angles );
		angles[PITCH] -= SHORT2ANGLE( cl.snap.ps.delta_angles[PITCH] );
		angles[YAW] += cl.viewangles[YAW] - vr.hmdorientation[YAW];
		// Servers reading 32-bit commands take head roll from the angles; this option sends it
		// to the rest.
		angles[ROLL] = (vr_sendRollToServer->integer || CL_VR_UsercmdButtonBits() == 32)
							? Com_Clamp( -60, 60, vr.hmdorientation[ROLL] )
							: 0;
		for ( i = 0; i < 3; i++ )
			cmd->angles[i] = ANGLE2SHORT( angles[i] );
		angle = -vr.calculated_weaponangles[YAW] * (float)M_PI / 180;
		x = cmd->rightmove;
		cmd->rightmove = (signed char)Com_Clamp( -127, 127, cosf( angle ) * x - sinf( angle ) * cmd->forwardmove );
		cmd->forwardmove =
			(signed char)Com_Clamp( -127, 127, cosf( angle ) * cmd->forwardmove + sinf( angle ) * x );
	}
	CL_VRInput_FinalizePose( cmd );
	return qtrue;
}
