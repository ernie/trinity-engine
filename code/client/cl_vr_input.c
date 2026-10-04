#include "../vrcommon/vr_float.h"
#include "client.h"
#include "cl_vr_input.h"
#include "cl_vr.h"
#include "cl_vr_bind.h"
#include "cl_bhaptics.h"
#include "../vrcommon/vr_state.h"
#include "../vrcommon/vr_input_types.h"

static clBHaptics_t suitHaptics;
static qboolean suitEnabled;
static cvar_t *vrSensitivity;
static cvar_t *cgStereoSeparation;
/* The runtime's FOV, before VRInput_PublishFov decides what the frame sees */
static float rawFovX = 90, rawFovUp = (float)M_PI / 4, rawFovDown = -(float)M_PI / 4;
static struct {
	clXRInputSample_t sample;
	qboolean valid, tracked;
	int time, previousTime;
	int hapticEnd[2];
} input;
/* Controller keys: the hold machine owns every press until its release, whatever the context does meanwhile. */
static vrHolds_t holds;
static qboolean holdsReady;
static vrStack_t stack;
static unsigned char keysNow[VRK_COUNT];
static int weaponSelectHeld, stabiliseHeld;
static int clickKey[VRK_COUNT + 1], navKey[VRK_COUNT + 1], navAnchorX, navAnchorY; // the last slot is the console's

/* The virtual screen is a monitor: cg_fov over a symmetric crop, derived from vr.virtual_screen so the two agree. */
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

static void CL_VRInput_QuaternionAngles( const float quaternion[4], float gripPitch, vec3_t angles ) {
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


static void VRInput_HoldsReady( void ) {
	if ( !holdsReady ) {
		VR_HoldsInit( &holds );
		holdsReady = qtrue;
	}
}

/* Button commands set their input state at once; the rest run from the command buffer. */
static qboolean VRInput_Immediate( const char *word ) {
	static const char *words[] = {"+attack", "+forward", "+back", "+moveleft", "+moveright", "+moveup", "+movedown",
								  "+left", "+right", "+lookup", "+lookdown", "+strafe", "+speed", "+mlook", "+key",
								  "+vr_click", "+menunav", "+weapon_select", "+weapon_stabilise", "+alt", "+vote_yes",
								  "+vote_no", "turnleft", "turnright", "uturn", "vr_recenter"};
	unsigned i;
	char plus[64];
	Com_sprintf( plus, sizeof( plus ), "+%s", word[0] == '-' || word[0] == '+' ? word + 1 : word );
	if ( !Q_strncmp( plus, "+button", 7 ) )
		return qtrue;
	for ( i = 0; i < ARRAY_LEN( words ); i++ )
		if ( !Q_stricmp( words[i][0] == '+' ? plus : word, words[i] ) )
			return qtrue;
	return qfalse;
}

static void VRInput_Run( const vrBindEvent_t *e, qboolean resetting ) {
	char buf[VR_BINDING_MAX], cmd[MAX_STRING_CHARS], word[64], *p, *end;
	int i;
	Q_strncpyz( buf, e->binding, sizeof( buf ) );
	for ( p = buf; p; p = end ) {
		end = strchr( p, ';' );
		if ( end )
			*end++ = '\0';
		while ( *p == ' ' )
			p++;
		if ( !*p )
			continue;
		if ( *p == '+' ) {
			/* Losing input mid-scrub cancels it instead of seeking to wherever the pointer was. */
			if ( resetting && !Q_stricmpn( p, "+tv_scrub", 9 ) && (p[9] == '\0' || p[9] == ' ') )
				Q_strncpyz( cmd, "tv_scrub_cancel", sizeof( cmd ) );
			else
				Com_sprintf( cmd, sizeof( cmd ), "%c%s %d %d", e->type == VRE_PRESS ? '+' : '-', p + 1,
							 K_VR_WPN_TRIGGER + e->key, com_frameTime );
		} else if ( e->type == VRE_PRESS )
			Q_strncpyz( cmd, p, sizeof( cmd ) );
		else
			continue;
		for ( i = 0; cmd[i] && cmd[i] != ' ' && i < (int)sizeof( word ) - 1; i++ )
			word[i] = cmd[i];
		word[i] = '\0';
		if ( VRInput_Immediate( word ) )
			Cmd_ExecuteString( cmd );
		else
			Cbuf_AddText( va( "%s\n", cmd ) );
	}
}

static void VRInput_RunEvents( const vrBindEvent_t *events, int count, qboolean resetting ) {
	int i;
	for ( i = 0; i < count; i++ )
		VRInput_Run( &events[i], resetting );
}

/* The VR key that ran this command, from the key number the dispatcher appends; -1 when typed at the console. */
static int VRInput_SourceKey( void ) {
	int key = Cmd_Argc() >= 3 ? atoi( Cmd_Argv( Cmd_Argc() - 2 ) ) - K_VR_WPN_TRIGGER : -1;
	return key >= 0 && key < VRK_COUNT ? key : -1;
}

static void VRInput_KeyDown_f( void ) {
	int target = Key_StringToKeynum( Cmd_Argv( 1 ) );
	if ( target > 0 )
		CL_KeyEvent( target, qtrue, com_frameTime );
}
static void VRInput_KeyUp_f( void ) {
	int target = Key_StringToKeynum( Cmd_Argv( 1 ) );
	if ( target > 0 )
		CL_KeyEvent( target, qfalse, com_frameTime );
}

/* A menu press pulses the pressing hand, as on the standalone: 200 ms at 0.8, scaled by vr_hapticIntensity. */
static void VRInput_Pulse( int hand ) {
	if ( !re.XRHaptic || cls.realtime < input.hapticEnd[hand] )
		return;
	if ( re.XRHaptic( hand, Com_Clamp( 0, 1, .8f * VRInput_Cvar( vr_hapticIntensity, .5f ) ), 200 ) )
		input.hapticEnd[hand] = cls.realtime + 200;
}

/* A hover tick never holds the motor, so a press right after it still gets its pulse. */
void CL_VRInput_HoverTick( int hand ) {
	if ( !re.XRHaptic || cls.realtime < input.hapticEnd[hand] )
		return;
	re.XRHaptic( hand, Com_Clamp( 0, 1, .25f * VRInput_Cvar( vr_hapticIntensity, .5f ) ), 20 );
}

/* Whether each hand's drawn ray met the virtual screen, as of the last frame it was drawn. */
static qboolean pointerOnScreen[2];

/* With the pointer drawn, a ray off the screen has no pool of light, so its resting cursor means nothing. */
qboolean CL_VRInput_PointerOnScreen( int hand ) {
	return vr.pointerMode != VR_POINTER_DRAWN || pointerOnScreen[hand];
}

static void VRInput_Click( qboolean down ) {
	const int key = VRInput_SourceKey(), slot = key >= 0 ? key : VRK_COUNT;
	const int menuHand = vr.menuLeftHanded ? 0 : 1;
	const int hand = key >= 0 ? VR_KeyHand( (vrKey_t)key, vr.right_handed, vr_switchThumbsticks->integer ) : menuHand;
	if ( !down ) {
		if ( clickKey[slot] < 0 ) {
			VKeyboard_HandleOffhandKey( qfalse );
			vr.vkbOffhandTriggerDown = qfalse;
		} else if ( clickKey[slot] )
			CL_KeyEvent( clickKey[slot], qfalse, com_frameTime );
		clickKey[slot] = 0;
		return;
	}
	if ( VKeyboard_IsActive() && hand != menuHand ) {
		if ( vr.pointerMode == VR_POINTER_DRAWN && !pointerOnScreen[hand] )
			return;
		vr.vkbOffhandTriggerDown = qtrue;
		VKeyboard_HandleOffhandKey( qtrue );
		VRInput_Pulse( hand );
		clickKey[slot] = -1;
		return;
	}
	if ( hand != menuHand ) {
		/* The clicking hand becomes the pointer; its cursor was tracked as the other one. */
		int x = vr.menuCursorX, y = vr.menuCursorY;
		vr.menuCursorX = vr.offhandCursorX;
		vr.menuCursorY = vr.offhandCursorY;
		vr.offhandCursorX = x;
		vr.offhandCursorY = y;
		vr.menuLeftHanded = hand == 0;
		if ( Key_GetCatcher() & KEYCATCH_UI )
			CL_MouseEvent( 0, 0 );
		/* nothing showed where that hand pointed, ray or cursor, so this press only takes the pointer over */
		if ( vr.pointerMode != VR_POINTER_STICK ) {
			VRInput_Pulse( hand );
			return;
		}
	}
	/* a ray that is off the screen clicks nothing */
	if ( vr.pointerMode == VR_POINTER_DRAWN && !pointerOnScreen[hand] )
		return;
	if ( !vr.menuCursorActive && vr.pointerMode != VR_POINTER_STICK )
		return;
	clickKey[slot] = vr.pointerMode == VR_POINTER_STICK && !VKeyboard_IsActive() ? K_ENTER : K_MOUSE1;
	CL_KeyEvent( clickKey[slot], qtrue, com_frameTime );
	VRInput_Pulse( vr.menuLeftHanded ? 0 : 1 );
}
static void VRInput_ClickDown_f( void ) {
	VRInput_Click( qtrue );
}
static void VRInput_ClickUp_f( void ) {
	VRInput_Click( qfalse );
}

static void VRInput_Nav( qboolean down ) {
	const int key = VRInput_SourceKey(), slot = key >= 0 ? key : VRK_COUNT;
	const char *dir = Cmd_Argv( 1 );
	const qboolean console = (Key_GetCatcher() & KEYCATCH_CONSOLE) != 0;
	int target;
	if ( !down ) {
		if ( navKey[slot] )
			CL_KeyEvent( navKey[slot], qfalse, com_frameTime );
		navKey[slot] = 0;
		return;
	}
	if ( !Q_stricmp( dir, "up" ) )
		target = console ? K_PGUP : K_UPARROW;
	else if ( !Q_stricmp( dir, "down" ) )
		target = console ? K_PGDN : K_DOWNARROW;
	else if ( !Q_stricmp( dir, "left" ) )
		target = K_LEFTARROW;
	else if ( !Q_stricmp( dir, "right" ) )
		target = K_RIGHTARROW;
	else
		return;
	if ( vr.pointerMode != VR_POINTER_STICK ) {
		navAnchorX = vr.menuCursorX;
		navAnchorY = vr.menuCursorY;
	}
	vr.pointerMode = VR_POINTER_STICK;
	navKey[slot] = target;
	CL_KeyEvent( target, qtrue, com_frameTime );
}
static void VRInput_NavDown_f( void ) {
	VRInput_Nav( qtrue );
}
static void VRInput_NavUp_f( void ) {
	VRInput_Nav( qfalse );
}

static void VRInput_SelectDown_f( void ) {
	weaponSelectHeld++;
}
static void VRInput_SelectUp_f( void ) {
	if ( weaponSelectHeld > 0 )
		weaponSelectHeld--;
}
static void VRInput_StabiliseDown_f( void ) {
	stabiliseHeld++;
}
static void VRInput_StabiliseUp_f( void ) {
	if ( stabiliseHeld > 0 )
		stabiliseHeld--;
}
/* Alt is read from the holds; the command only has to exist. */
static void VRInput_Alt_f( void ) {
}
static void VRInput_VoteYesDown_f( void ) {
	vr.vote_holding = 1;
}
static void VRInput_VoteNoDown_f( void ) {
	vr.vote_holding = -1;
}
static void VRInput_VoteUp_f( void ) {
	vr.vote_holding = 0;
}

static float VRInput_SnapAngle( void ) {
	return vr_snapturn->integer == 1 ? 45 : VRInput_Cvar( vr_snapturn, 45 );
}
static void VRInput_TurnLeft_f( void ) {
	if ( VRInput_Cvar( vr_snapturn, 45 ) > 0 )
		cl.viewangles[YAW] += VRInput_SnapAngle();
}
static void VRInput_TurnRight_f( void ) {
	if ( VRInput_Cvar( vr_snapturn, 45 ) > 0 )
		cl.viewangles[YAW] -= VRInput_SnapAngle();
}
static void VRInput_UTurn_f( void ) {
	cl.viewangles[YAW] += 180;
}

static void VRInput_Recenter_f( void ) {
	vr.menuYaw = vr.hmdorientation[YAW];
	CL_VR_ResetVirtualScreen();
}

/* Bindings-menu capture arms on the next input frame, so releasing holds never re-enters the UI from its own call. */
static enum { CAPTURE_IDLE, CAPTURE_ARMING, CAPTURE_ARMED } capture;

void CL_VRInput_BindCapture( void ) {
	if ( Key_GetCatcher() & KEYCATCH_UI )
		capture = CAPTURE_ARMING;
}

void CL_VRInput_CancelCapture( void ) {
	capture = CAPTURE_IDLE;
}

/* While armed the UI gets the gesture's key code once its buttons are let go, instead of a binding; qtrue while capture owns the keys. */
static qboolean VRInput_Capture( void ) {
	vrBindEvent_t events[VRK_COUNT];
	const char *global;
	int key;
	if ( capture == CAPTURE_IDLE )
		return qfalse;
	if ( !uivm || !(Key_GetCatcher() & KEYCATCH_UI) ) {
		capture = CAPTURE_IDLE;
		return qfalse;
	}
	if ( capture == CAPTURE_ARMING ) {
		VRInput_RunEvents( events, VR_ReleaseAll( &holds, events, ARRAY_LEN( events ) ), qtrue );
		capture = CAPTURE_ARMED;
	}
	key = VR_CaptureKey( &holds, keysNow );
	if ( key < 0 )
		return qtrue;
	capture = CAPTURE_IDLE;
	global = CL_VRBind_Lookup( VRC_GLOBAL, 0, (vrKey_t)key, NULL );
	VM_Call( uivm, 2, UI_KEY_EVENT, global && !Q_stricmp( global, "+key ESCAPE" ) ? K_ESCAPE : K_VR_WPN_TRIGGER + key,
			 qtrue );
	return qtrue;
}

void CL_VRInput_Reset( void ) {
	vrBindEvent_t events[VRK_COUNT];
	VRInput_HoldsReady();
	capture = CAPTURE_IDLE;
	CL_BHaptics_Stop( &suitHaptics );
	VRInput_RunEvents( events, VR_ReleaseAll( &holds, events, VRK_COUNT ), qtrue );
	memset( &input, 0, sizeof( input ) );
	memset( keysNow, 0, sizeof( keysNow ) );
	weaponSelectHeld = stabiliseHeld = 0;
	vr.weapon_select = vr.weapon_select_using_thumbstick = vr.weapon_select_autoclose = qfalse;
	vr.weapon_stabilised = vr.walking = vr.vkbOffhandTriggerDown = qfalse;
	vr.vote_holding = 0;
	vr.pointerMode = VR_POINTER_CURSOR;
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
	static const struct {
		const char *name;
		xcommand_t fn;
	} commands[] = {{"+key", VRInput_KeyDown_f},
					{"-key", VRInput_KeyUp_f},
					{"+vr_click", VRInput_ClickDown_f},
					{"-vr_click", VRInput_ClickUp_f},
					{"+menunav", VRInput_NavDown_f},
					{"-menunav", VRInput_NavUp_f},
					{"+weapon_select", VRInput_SelectDown_f},
					{"-weapon_select", VRInput_SelectUp_f},
					{"+weapon_stabilise", VRInput_StabiliseDown_f},
					{"-weapon_stabilise", VRInput_StabiliseUp_f},
					{"+alt", VRInput_Alt_f},
					{"-alt", VRInput_Alt_f},
					{"+vote_yes", VRInput_VoteYesDown_f},
					{"-vote_yes", VRInput_VoteUp_f},
					{"+vote_no", VRInput_VoteNoDown_f},
					{"-vote_no", VRInput_VoteUp_f},
					{"turnleft", VRInput_TurnLeft_f},
					{"turnright", VRInput_TurnRight_f},
					{"uturn", VRInput_UTurn_f},
					{"vr_recenter", VRInput_Recenter_f}};
	unsigned i;
	CL_VRInput_Shutdown();
	VR_InitCvars();
	CL_VRInput_Reset();
	for ( i = 0; i < ARRAY_LEN( commands ); i++ ) {
		Cmd_RemoveCommand( commands[i].name );
		Cmd_AddCommand( commands[i].name, commands[i].fn );
	}
	vr.menuCursorX = vr.offhandCursorX = 320;
	vr.menuCursorY = vr.offhandCursorY = 240;
	/* Share the existing mouse/VR UI setting; keep the engine's default. */
	vrSensitivity = Cvar_Get( "vr_sensitivity", "100", CVAR_ARCHIVE );
	cgStereoSeparation = Cvar_Get( "cg_stereoSeparation", "0", 0 );
	/* Usable projection denominators until the runtime supplies its stereo FOV. */
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

/* Where the hand's aim meets the virtual screen, if it does. */
static qboolean VRInput_AimOnScreen( const refXRFrame_t *frame, int hand, float forward[3], float uv[2] ) {
	const clXRPose_t *aim = &frame->input.hands[hand].aim;
	const float *q = aim->orientation;
	if ( !aim->positionValid || !aim->orientationValid )
		return qfalse;
	forward[0] = -2 * (q[0] * q[2] + q[3] * q[1]);
	forward[1] = -2 * (q[1] * q[2] - q[3] * q[0]);
	forward[2] = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
	return VR_ScreenRay( &frame->screen, aim->position, forward, uv );
}

/* The hand's aim ray on the virtual screen; a miss holds the cursor where it last was. */
static void VRInput_ScreenCursor( const refXRFrame_t *frame, int hand, int *x, int *y ) {
	float forward[3], uv[2];
	int targetX = *x, targetY = *y;
	if ( VRInput_AimOnScreen( frame, hand, forward, uv ) ) {
		targetX = (int)(uv[0] * 640);
		targetY = (int)(uv[1] * 480);
	}
	*x = (int)(.5f * targetX + .5f * *x);
	*y = (int)(.5f * targetY + .5f * *y);
}

/* On the screen the ray ends on the cursor, which trails the aim by its smoothing; off it there is no pool. */
static qboolean VRInput_ShowPointer( const refXRFrame_t *frame, int hand, int cursorX, int cursorY ) {
	const clXRPose_t *aim = &frame->input.hands[hand].aim;
	const float cursor[2] = {cursorX / 640.0f, cursorY / 480.0f};
	float forward[3], uv[2], end[3];
	qboolean hit;
	pointerOnScreen[hand] = qfalse;
	if ( !re.XRSetPointer || !aim->positionValid || !aim->orientationValid )
		return qfalse;
	hit = pointerOnScreen[hand] = VRInput_AimOnScreen( frame, hand, forward, uv );
	if ( hit )
		VR_ScreenPoint( &frame->screen, cursor[0], cursor[1], end );
	else
		VectorMA( aim->position, VR_ScreenReach( &frame->screen, aim->position, forward ), forward, end );
	/* the keyboard tells the hands apart by color: the left one blue, the right one red */
	re.XRSetPointer( hand, aim->position, end, hit ? cursor : NULL, hand == 0 && VKeyboard_IsActive() );
	return qtrue;
}

static qboolean VRInput_PointerLayer( void ) {
	return VR_StackHas( &stack, VRC_MENU ) || VR_StackHas( &stack, VRC_SCOREBOARD );
}

/* Movement and smooth turn only reach the game from gameplay or an overlay on it. */
static qboolean VRInput_ModalLayer( void ) {
	return VRInput_PointerLayer() || VR_StackHas( &stack, VRC_ADJUST ) || VR_StackHas( &stack, VRC_SCRUB );
}

static void VRInput_Signals( vrSignals_t *s ) {
	const int catcher = Key_GetCatcher();
	memset( s, 0, sizeof( *s ) );
	s->textEntry = VKeyboard_IsActive();
	s->menu = (catcher & (KEYCATCH_UI | KEYCATCH_CONSOLE)) != 0;
	s->offline = cls.state != CA_ACTIVE;
	s->adjust = vr.weapon_adjust;
	s->scrub = vr.menuYawLocked;
	/* The flag can outlive the cgame catcher that Escape strips; the catcher decides. */
	s->scoreboard = vr.scoreboardCursorActive && (catcher & KEYCATCH_CGAME);
	s->vote = vr.vote_active;
	s->wheel = vr.weapon_select;
	s->tv = tvPlay.active;
	s->demo = clc.demoplaying;
	s->following = (cl.snap.ps.pm_flags & PMF_FOLLOW) != 0;
	s->intermission = cl.snap.ps.pm_type == PM_INTERMISSION;
	s->dead = cl.snap.ps.stats[STAT_HEALTH] <= 0;
	s->spectating = cl.snap.ps.persistant[PERS_TEAM] == TEAM_SPECTATOR;
}

void CL_VRInput_Frame( const refXRFrame_t *frame ) {
	qboolean wheel, priorWheel, menu;
	vec3_t oldAngles, oldPosition;
	int i, primary, secondary;
	const clXRHandInput_t *weapon, *other;
	vrSignals_t signals;
	vrThresholds_t thresholds;
	vrBindEvent_t events[VRK_COUNT * 2];
	unsigned char previous[VRK_COUNT];
	VRInput_HoldsReady();
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
	weapon = &frame->input.hands[primary];
	other = &frame->input.hands[secondary];
	for ( i = 0; i < 2; i++ ) {
		vr.eye_fov_angle_left[i] = frame->eyes[i].fov[0];
		vr.eye_fov_angle_right[i] = frame->eyes[i].fov[1];
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
	CL_VRBind_SetProfile( frame->input.hands[1].profile >= 0 ? frame->input.hands[1].profile
															 : frame->input.hands[0].profile );
	/* A visible session tracks without input focus: poses publish, actions do not. */
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
	VRInput_Signals( &signals );
	VR_ResolveStack( &signals, &stack );
	menu = VR_StackHas( &stack, VRC_MENU );
	if ( VRInput_PointerLayer() ) {
		const int menuHand = vr.menuLeftHanded ? 0 : 1;
		const qboolean menuWeapon = primary == menuHand;
		vr.menuCursorActive = frame->input.hands[menuHand].aim.orientationValid;
		if ( vr.menuCursorActive ) {
			/* Smooth once for each tracked controller sample; both hands, so either can take the pointer. */
			for ( i = 0; i < 2; i++ )
				if ( frame->input.hands[i].grip.positionValid ) {
					if ( frame->screen.visible ) {
						VRInput_ScreenCursor( frame, menuHand, &vr.menuCursorX, &vr.menuCursorY );
						VRInput_ScreenCursor( frame, 1 - menuHand, &vr.offhandCursorX, &vr.offhandCursorY );
						continue;
					}
					VRInput_Cursor( menuWeapon ? vr.weaponaimangles : vr.offhandaimangles, &vr.menuCursorX,
									&vr.menuCursorY );
					VRInput_Cursor( menuWeapon ? vr.offhandaimangles : vr.weaponaimangles, &vr.offhandCursorX,
									&vr.offhandCursorY );
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
	/* Hover must update before a same-frame click, after stick ownership is published. */
	if ( (Key_GetCatcher() & KEYCATCH_UI) && vr.menuCursorActive && vr.pointerMode != VR_POINTER_STICK &&
		!VKeyboard_IsActive() && !vr.weapon_adjust && !vr.menuYawLocked )
		CL_MouseEvent( 0, 0 );
	thresholds.triggerPress = 1 - VRInput_Cvar( vr_triggerSensitivity, .25f );
	thresholds.triggerRelease = fmaxf( .1f, thresholds.triggerPress - .25f );
	thresholds.gripPress = VRInput_Cvar( vr_gripThreshold, .5f );
	thresholds.gripRelease = thresholds.gripPress - .1f;
	thresholds.padPress = VRInput_Cvar( vr_trackpadThreshold, .3f );
	thresholds.padRelease = thresholds.padPress - .1f;
	thresholds.stickPress = .5f;
	thresholds.stickRelease = .35f;
	thresholds.deadzone = VRInput_Cvar( vr_thumbstickDeadzone, .15f );
	memcpy( previous, keysNow, sizeof( previous ) );
	VR_SampleKeys( frame->input.hands, vr.right_handed, vr_switchThumbsticks->integer, &thresholds, previous, keysNow );
	VR_RoleSticks( frame->input.hands, vr.right_handed, vr_switchThumbsticks->integer, thresholds.deadzone,
				   vr.thumbstick_location[VR_STICK_MOVE], vr.thumbstick_location[VR_STICK_TURN] );
	{
		const int alt = VR_HoldsBound( &holds, "+alt" );
		if ( VR_StickTaken( &stack, alt, VRK_MOVESTICK, CL_VRBind_Lookup, NULL ) )
			vr.thumbstick_location[VR_STICK_MOVE][0] = vr.thumbstick_location[VR_STICK_MOVE][1] = 0;
		if ( VR_StickTaken( &stack, alt, VRK_TURNSTICK, CL_VRBind_Lookup, NULL ) )
			vr.thumbstick_location[VR_STICK_TURN][0] = vr.thumbstick_location[VR_STICK_TURN][1] = 0;
	}
	{
		const int mapping = vr.right_handed | (vr_switchThumbsticks->integer != 0) << 1;
		/* Role keys change names with handedness, so an open vote prompt must rename them. */
		if ( holds.mapping >= 0 && holds.mapping != mapping )
			CL_ResolveVoteKeys();
		VRInput_RunEvents( events, VR_HoldsSetMapping( &holds, mapping, events, ARRAY_LEN( events ) ), qfalse );
	}
	if ( !VRInput_Capture() )
		VRInput_RunEvents( events,
						   VR_UpdateHolds( &holds, keysNow, &stack, CL_VRBind_Lookup, NULL, com_frameTime, events,
										   ARRAY_LEN( events ) ),
						   qfalse );
	if ( vr.pointerMode == VR_POINTER_STICK ) {
		qboolean held = qfalse;
		for ( i = 0; i < (int)ARRAY_LEN( navKey ); i++ )
			held |= navKey[i] != 0;
		if ( !menu )
			vr.pointerMode = VR_POINTER_CURSOR;
		else if ( !held ) {
			int dx = vr.menuCursorX - navAnchorX, dy = vr.menuCursorY - navAnchorY;
			if ( dx * dx + dy * dy > 3600 )
				vr.pointerMode = VR_POINTER_CURSOR;
		}
	}
	/* Drawn ray or module cursor; the off hand points too while the keyboard is up. */
	if ( vr.pointerMode != VR_POINTER_STICK ) {
		const int menuHand = vr.menuLeftHanded ? 0 : 1;
		qboolean drawn = qfalse;
		if ( vr_controllerModels->integer && VRInput_PointerLayer() && frame->screen.visible && vr.menuCursorActive &&
			 !vr.weapon_adjust && !vr.menuYawLocked ) {
			drawn = VRInput_ShowPointer( frame, menuHand, vr.menuCursorX, vr.menuCursorY );
			if ( VKeyboard_IsActive() )
				VRInput_ShowPointer( frame, 1 - menuHand, vr.offhandCursorX, vr.offhandCursorY );
		}
		vr.pointerMode = drawn ? VR_POINTER_DRAWN : VR_POINTER_CURSOR;
	}
	priorWheel = vr.weapon_select;
	wheel = weaponSelectHeld > 0 && !clc.demoplaying && !(cl.snap.ps.pm_flags & PMF_FOLLOW);
	vr.weapon_select = wheel;
	vr.weapon_select_using_thumbstick = wheel && vr_weaponSelectorMode->integer == WS_HMD;
	vr.weapon_select_autoclose =
		vr.weapon_select_using_thumbstick && (keysNow[VRK_TURNSTICK_UP] || keysNow[VRK_TURNSTICK_DOWN] ||
											  keysNow[VRK_TURNSTICK_LEFT] || keysNow[VRK_TURNSTICK_RIGHT]);
	if ( priorWheel && !wheel && !menu )
		Cbuf_AddText( "weapon_select\n" );
	vr.weapon_stabilised = qfalse;
	if ( stabiliseHeld > 0 && weapon->grip.positionValid && other->grip.positionValid ) {
		vec3_t distance;
		VectorSubtract( vr.weaponposition, vr.offhandposition, distance );
		vr.weapon_stabilised = VectorLength( distance ) < .4f;
	}
	vr.weapon_zoomed = !VRInput_PointerLayer() && vr_weaponScope->integer && vr.weapon_stabilised &&
					   cl.snap.ps.weapon == WP_RAILGUN && VectorLength( vr.weaponoffset ) < .24f &&
					   cl.snap.ps.stats[STAT_HEALTH] > 0;
	if ( !VRInput_PointerLayer() && vr_twoHandedWeapons->integer && vr.weapon_stabilised ) {
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
	if ( VRInput_Cvar( vr_snapturn, 45 ) <= 0 && !VRInput_ModalLayer() && !Key_GetCatcher() && input.previousTime &&
		 !vr.weapon_select_using_thumbstick ) {
		float elapsed = Com_Clamp( 0, 100, input.time - input.previousTime ) * .001f;
		/* The reference full-stick rate is 32767 * .022 degrees/second. */
		cl.viewangles[YAW] -= vr.thumbstick_location[VR_STICK_TURN][0] * (32767.0f * .022f) *
							  (VRInput_Cvar( vrSensitivity, 100 ) / 100.0f) * elapsed;
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
	usercmd_t keys;
	int primary, i, catcher;
	if ( !VR_IsActiveMode() )
		return qfalse;
	/* Read every frame so a press made while neutral cannot fire afterwards. */
	memset( &keys, 0, sizeof( keys ) );
	CL_VRInput_KeyState( &keys );
	memset( cmd, 0, sizeof( *cmd ) );
	cmd->weapon = cl.cgameUserCmdValue;
	cmd->serverTime = cl.serverTime;
	for ( i = 0; i < 3; i++ )
		cmd->angles[i] = ANGLE2SHORT( cl.viewangles[i] );
	/* Buttons are gated by the catcher, but BUTTON_TALK still marks a headset to the server. */
	catcher = Key_GetCatcher();
	if ( catcher )
		cmd->buttons |= BUTTON_TALK;
	if ( !input.valid || cls.realtime - input.time > 250 )
		return qtrue;
	if ( catcher || VRInput_ModalLayer() || vr.weapon_adjust || vr.menuYawLocked ||
		(vr.virtual_screen && !vr.first_person_following) ) {
		CL_VRInput_FinalizePose( cmd );
		return qtrue;
	}
	vr.clientNum = cl.snap.ps.clientNum;
	primary = vr.right_handed ? 1 : 0;
	side = vr.thumbstick_location[VR_STICK_MOVE][0];
	forward = vr.thumbstick_location[VR_STICK_MOVE][1];
	angle = vr_directionMode->integer ? vr.offhandangles[YAW] : vr.hmdorientation[YAW];
	if ( vr.use_6dof )
		angle -= vr.hmdorientation[YAW];
	angle *= (float)M_PI / 180;
	x = side;
	side = cosf( angle ) * x - sinf( angle ) * forward;
	forward = cosf( angle ) * forward + sinf( angle ) * x;
	if ( vr.use_6dof && input.previousTime && input.time > input.previousTime ) {
		/* Convert room-scale meters per millisecond to command movement units. */
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
	cmd->rightmove = (signed char)Com_Clamp( -127, 127, side * 127 + keys.rightmove );
	cmd->forwardmove = (signed char)Com_Clamp( -127, 127, forward * 127 + keys.forwardmove );
	cmd->upmove = keys.upmove;
	if ( vr.use_6dof && cl.snap.ps.pm_type == PM_SPECTATOR && !(cl.snap.ps.pm_flags & PMF_FOLLOW) ) {
		float pitch = vr.offhandangles[PITCH] * (float)M_PI / 180;
		int original = cmd->forwardmove;
		cmd->forwardmove = (signed char)Com_Clamp( -127, 127, original * cosf( pitch ) );
		cmd->upmove = (signed char)Com_Clamp( -127, 127, cmd->upmove - original * sinf( pitch ) );
	}
	cmd->buttons |= keys.buttons & 0xFFF & ~BUTTON_TALK; // bits 12-25 carry the head pose
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
	if ( vr.walking || (keys.buttons & BUTTON_WALKING) )
		cmd->buttons |= BUTTON_WALKING;
	if ( !vr.use_6dof && input.sample.hands[primary].aim.orientationValid ) {
		if ( vr.realign > 0 && --vr.realign == 0 )
			VectorCopy( vr.hmdposition, vr.hmdorigin );
		VectorCopy( vr.calculated_weaponangles, angles );
		angles[PITCH] -= SHORT2ANGLE( cl.snap.ps.delta_angles[PITCH] );
		angles[YAW] += cl.viewangles[YAW] - vr.hmdorientation[YAW];
		// 32-bit servers always get head roll; vr_sendRollToServer extends it to 16-bit ones
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
