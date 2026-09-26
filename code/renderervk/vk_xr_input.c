#include "../vrcommon/vr_float.h"
#include "vk_xr_input.h"
/* Supported core/vendor profile component table; no runtime calls. */
#include <string.h>
static inline const char *CL_XRProfilePath( int profile ) {
	static const char *paths[CL_XRP_COUNT] = {
		"/interaction_profiles/oculus/touch_controller", "/interaction_profiles/bytedance/pico4_controller",
		"/interaction_profiles/bytedance/pico4s_controller", "/interaction_profiles/valve/index_controller",
		"/interaction_profiles/khr/simple_controller", "/interaction_profiles/valve/frame_controller_valve"};
	return profile >= 0 && profile < CL_XRP_COUNT ? paths[profile] : NULL;
}
static inline const char *CL_XRProfileName( int profile ) {
	static const char *names[CL_XRP_COUNT] = {[CL_XRP_TOUCH] = "Oculus Touch",	   [CL_XRP_PICO4] = "PICO 4",
											  [CL_XRP_PICO4S] = "PICO 4 Ultra", [CL_XRP_INDEX] = "Valve Index",
											  [CL_XRP_SIMPLE] = "Simple",	   [CL_XRP_FRAME] = "Steam Frame"};
	return profile >= 0 && profile < CL_XRP_COUNT ? names[profile] : NULL;
}
/* Grip poses follow each handle's angle; measured on the headset, 0 until a controller needs one. */
static inline float CL_XRProfilePitch( int profile ) {
	static const float pitch[CL_XRP_COUNT] = {[CL_XRP_TOUCH] = 0, [CL_XRP_PICO4] = 0,	[CL_XRP_PICO4S] = 0,
											  [CL_XRP_INDEX] = 0, [CL_XRP_SIMPLE] = 0, [CL_XRP_FRAME] = 20};
	return profile >= 0 && profile < CL_XRP_COUNT ? pitch[profile] : 0;
}
static inline const char *CL_XRProfileComponent( int profile, const char *action, int hand ) {
	if ( profile < 0 || profile >= CL_XRP_COUNT || hand < 0 || hand > 1 )
		return NULL;
	if ( !strcmp( action, "grip_pose" ) )
		return "input/grip/pose";
	if ( !strcmp( action, "aim_pose" ) )
		return "input/aim/pose";
	if ( !strcmp( action, "haptic" ) )
		return "output/haptic";
	if ( profile == CL_XRP_FRAME ) {
		static const char *leftOnly[][2] = {{"dpad_up", "input/dpad_up/click"},
											{"dpad_down", "input/dpad_down/click"},
											{"dpad_left", "input/dpad_left/click"},
											{"dpad_right", "input/dpad_right/click"},
											{"view", "input/view/click"}};
		static const char *rightOnly[][2] = {{"primary", "input/a/click"}, {"secondary", "input/b/click"},
											 {"x", "input/x/click"}, {"y", "input/y/click"},
											 {"menu", "input/menu/click"}};
		unsigned i;
		for ( i = 0; i < sizeof( leftOnly ) / sizeof( leftOnly[0] ); i++ )
			if ( !strcmp( action, leftOnly[i][0] ) )
				return hand ? NULL : leftOnly[i][1];
		for ( i = 0; i < sizeof( rightOnly ) / sizeof( rightOnly[0] ); i++ )
			if ( !strcmp( action, rightOnly[i][0] ) )
				return hand ? rightOnly[i][1] : NULL;
		if ( !strcmp( action, "bumper" ) )
			return "input/bumper/click";
		if ( !strcmp( action, "squeeze_click" ) )
			return "input/squeeze/click";
		if ( !strcmp( action, "trackpad" ) || !strcmp( action, "thumbrest" ) || !strcmp( action, "simple_menu" ) )
			return NULL;
	}
	if ( profile == CL_XRP_SIMPLE ) {
		if ( !strcmp( action, "trigger" ) )
			return "input/select/click";
		if ( !strcmp( action, "simple_menu" ) )
			return "input/menu/click";
		return NULL;
	}
	if ( !strcmp( action, "trigger" ) )
		return "input/trigger/value";
	if ( !strcmp( action, "squeeze" ) )
		return "input/squeeze/value";
	if ( !strcmp( action, "stick" ) )
		return "input/thumbstick";
	if ( !strcmp( action, "stick_click" ) )
		return "input/thumbstick/click";
	if ( !strcmp( action, "primary" ) )
		return hand || profile == CL_XRP_INDEX ? "input/a/click" : "input/x/click";
	if ( !strcmp( action, "secondary" ) )
		return hand || profile == CL_XRP_INDEX ? "input/b/click" : "input/y/click";
	if ( !strcmp( action, "menu" ) )
		return hand ? NULL : profile == CL_XRP_INDEX ? "input/system/click" : "input/menu/click";
	if ( !strcmp( action, "trackpad" ) )
		return profile == CL_XRP_INDEX ? "input/trackpad/force" : NULL;
	if ( !strcmp( action, "thumbrest" ) )
		return profile == CL_XRP_INDEX ? NULL : "input/thumbrest/touch";
	return NULL;
}

#include <string.h>
#include <stdio.h>

static const char *xriNames[CL_XRI_ACTIONS] = {
	"trigger",	 "squeeze",	 "stick",  "stick_click", "primary",   "secondary",	 "menu",
	"grip_pose", "aim_pose", "haptic", "trackpad",	  "thumbrest", "simple_menu", "bumper",
	"dpad_up",	 "dpad_down", "dpad_left", "dpad_right", "view", "x", "y", "squeeze_click"};
static XrResult XRI_Suggest( vkXRInput_t *ctx, int profile ) {
	XrActionSuggestedBinding bindings[CL_XRI_ACTIONS * 2];
	XrInteractionProfileSuggestedBinding suggestion;
	XrResult result;
	unsigned hand, action, count = 0;
	memset( &suggestion, 0, sizeof( suggestion ) );
	suggestion.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	result = ctx->xr.StringToPath( ctx->instance, CL_XRProfilePath( profile ), &suggestion.interactionProfile );
	if ( XR_FAILED( result ) )
		return result;
	for ( hand = 0; hand < 2; hand++ )
		for ( action = 0; action < CL_XRI_ACTIONS; action++ ) {
			char path[128];
			const char *component = CL_XRProfileComponent( profile, xriNames[action], hand );
			if ( !component )
				continue;
			snprintf( path, sizeof( path ), "/user/hand/%s/%s", hand ? "right" : "left", component );
			bindings[count].action = ctx->actions[action];
			result = ctx->xr.StringToPath( ctx->instance, path, &bindings[count].binding );
			if ( XR_FAILED( result ) )
				return result;
			count++;
		}
	suggestion.countSuggestedBindings = count;
	suggestion.suggestedBindings = bindings;
	return ctx->xr.SuggestInteractionProfileBindings( ctx->instance, &suggestion );
}

XrResult VK_XRInput_Init( vkXRInput_t *ctx, XrInstance instance, PFN_xrGetInstanceProcAddr getproc,
						  int picoEnabled, int frameEnabled ) {
	static const XrActionType types[CL_XRI_ACTIONS] = {
		XR_ACTION_TYPE_FLOAT_INPUT,		 XR_ACTION_TYPE_FLOAT_INPUT,   XR_ACTION_TYPE_VECTOR2F_INPUT,
		XR_ACTION_TYPE_BOOLEAN_INPUT,	 XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT,
		XR_ACTION_TYPE_BOOLEAN_INPUT,	 XR_ACTION_TYPE_POSE_INPUT,	   XR_ACTION_TYPE_POSE_INPUT,
		XR_ACTION_TYPE_VIBRATION_OUTPUT, XR_ACTION_TYPE_FLOAT_INPUT,   XR_ACTION_TYPE_BOOLEAN_INPUT,
		XR_ACTION_TYPE_BOOLEAN_INPUT,	 XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT,
		XR_ACTION_TYPE_BOOLEAN_INPUT,	 XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT,
		XR_ACTION_TYPE_BOOLEAN_INPUT,	 XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT,
		XR_ACTION_TYPE_BOOLEAN_INPUT};
	XrActionSetCreateInfo setInfo;
	XrResult result;
	unsigned i;
	int accepted = 0;
	if ( !ctx || !instance || !getproc )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( ctx, 0, sizeof( *ctx ) );
	ctx->instance = instance;
#define LOAD(name) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = getproc( instance, "xr" #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) \
			return XR_FAILED( result ) ? result : XR_ERROR_FUNCTION_UNSUPPORTED; \
		ctx->xr.name = (PFN_xr##name)fn; \
	} while ( 0 );
	CL_XRI_FUNCTIONS( LOAD )
#undef LOAD
#define CHECK(call) \
	do { \
		result = (call); \
		if ( XR_FAILED( result ) ) \
			goto fail; \
	} while ( 0 )
	CHECK( ctx->xr.StringToPath( instance, "/user/hand/left", &ctx->hands[0] ) );
	CHECK( ctx->xr.StringToPath( instance, "/user/hand/right", &ctx->hands[1] ) );
	memset( &setInfo, 0, sizeof( setInfo ) );
	setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	strcpy( setInfo.actionSetName, "trinity_gameplay" );
	strcpy( setInfo.localizedActionSetName, "Trinity gameplay" );
	CHECK( ctx->xr.CreateActionSet( instance, &setInfo, &ctx->set ) );
	for ( i = 0; i < CL_XRI_ACTIONS; i++ ) {
		XrActionCreateInfo info;
		memset( &info, 0, sizeof( info ) );
		info.type = XR_TYPE_ACTION_CREATE_INFO;
		strcpy( info.actionName, xriNames[i] );
		strcpy( info.localizedActionName, xriNames[i] );
		info.actionType = types[i];
		info.countSubactionPaths = 2;
		info.subactionPaths = ctx->hands;
		CHECK( ctx->xr.CreateAction( ctx->set, &info, &ctx->actions[i] ) );
	}
	for ( i = 0; i < CL_XRP_COUNT; i++ )
		CHECK( ctx->xr.StringToPath( instance, CL_XRProfilePath( i ), &ctx->profilePaths[i] ) );
	ctx->profile[0] = ctx->profile[1] = -1;
	for ( i = 0; i < CL_XRP_COUNT; i++ ) {
		if ( (i == CL_XRP_PICO4 || i == CL_XRP_PICO4S) && !picoEnabled )
			continue;
		if ( i == CL_XRP_FRAME && !frameEnabled )
			continue;
		result = XRI_Suggest( ctx, i );
		if ( XR_SUCCEEDED( result ) )
			accepted++;
		else if ( i != CL_XRP_FRAME && result != XR_ERROR_PATH_UNSUPPORTED )
			goto fail;
	}
	if ( !accepted ) {
		result = XR_ERROR_PATH_UNSUPPORTED;
		goto fail;
	}
	return XR_SUCCESS;
fail:
	VK_XRInput_Shutdown( ctx );
	return result;
#undef CHECK
}

XrResult VK_XRInput_Attach( vkXRInput_t *ctx, XrSession session ) {
	XrSessionActionSetsAttachInfo info;
	XrResult result;
	unsigned hand, pose;
	if ( !ctx || !ctx->set || !session || ctx->session )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( &info, 0, sizeof( info ) );
	info.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	info.countActionSets = 1;
	info.actionSets = &ctx->set;
	result = ctx->xr.AttachSessionActionSets( session, &info );
	if ( XR_FAILED( result ) )
		return result;
	ctx->session = session;
	for ( hand = 0; hand < 2; hand++ )
		for ( pose = 0; pose < 2; pose++ ) {
			XrActionSpaceCreateInfo space;
			memset( &space, 0, sizeof( space ) );
			space.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
			space.action = ctx->actions[pose ? CL_XRI_AIM_POSE : CL_XRI_GRIP_POSE];
			space.subactionPath = ctx->hands[hand];
			space.poseInActionSpace.orientation.w = 1;
			result = ctx->xr.CreateActionSpace( session, &space, &ctx->spaces[hand][pose] );
			if ( XR_FAILED( result ) ) {
				VK_XRInput_Shutdown( ctx );
				return result;
			}
		}
	VK_XRInput_UpdateProfiles( ctx );
	return XR_SUCCESS;
}

void VK_XRInput_Reset( vkXRInput_t *ctx ) {
	unsigned hand;
	if ( !ctx )
		return;
	if ( ctx->session && ctx->focused && ctx->xr.StopHapticFeedback )
		for ( hand = 0; hand < 2; hand++ ) {
			XrHapticActionInfo info;
			memset( &info, 0, sizeof( info ) );
			info.type = XR_TYPE_HAPTIC_ACTION_INFO;
			info.action = ctx->actions[CL_XRI_HAPTIC];
			info.subactionPath = ctx->hands[hand];
			ctx->xr.StopHapticFeedback( ctx->session, &info );
		}
	ctx->focused = 0;
}

void VK_XRInput_UpdateProfiles( vkXRInput_t *ctx ) {
	unsigned hand;
	int p;
	if ( !ctx || !ctx->session )
		return;
	for ( hand = 0; hand < 2; hand++ ) {
		XrInteractionProfileState state;
		memset( &state, 0, sizeof( state ) );
		state.type = XR_TYPE_INTERACTION_PROFILE_STATE;
		ctx->profile[hand] = -1;
		if ( XR_FAILED( ctx->xr.GetCurrentInteractionProfile( ctx->session, ctx->hands[hand], &state ) ) )
			continue;
		for ( p = 0; p < CL_XRP_COUNT; p++ )
			if ( state.interactionProfile != XR_NULL_PATH && state.interactionProfile == ctx->profilePaths[p] )
				ctx->profile[hand] = p;
	}
}

const char *VK_XRInput_ProfileName( const vkXRInput_t *ctx, int hand ) {
	if ( !ctx || !ctx->session || hand < 0 || hand > 1 )
		return NULL;
	return CL_XRProfileName( ctx->profile[hand] );
}
float VK_XRInput_PitchCorrection( const vkXRInput_t *ctx, int hand ) {
	return ctx && ctx->session && hand >= 0 && hand < 2 ? CL_XRProfilePitch( ctx->profile[hand] ) : 0;
}

XrResult VK_XRInput_Sample( vkXRInput_t *ctx, XrSpace baseSpace, XrTime time, int focused,
							clXRInputSample_t *sample ) {
	XrActiveActionSet active;
	XrActionsSyncInfo sync;
	XrResult result;
	unsigned hand, i;
	if ( !sample )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( sample, 0, sizeof( *sample ) );
	if ( !ctx || !ctx->session )
		return XR_ERROR_HANDLE_INVALID;
	if ( !focused ) {
		VK_XRInput_Reset( ctx );
		return XR_SUCCESS;
	}
	if ( !baseSpace || time <= 0 ) {
		VK_XRInput_Reset( ctx );
		return XR_ERROR_VALIDATION_FAILURE;
	}
	active.actionSet = ctx->set;
	active.subactionPath = XR_NULL_PATH;
	memset( &sync, 0, sizeof( sync ) );
	sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
#define SAMPLE(call) \
	do { \
		result = (call); \
		if ( result != XR_SUCCESS ) \
			goto fail; \
	} while ( 0 )
	SAMPLE( ctx->xr.SyncActions( ctx->session, &sync ) );
	for ( hand = 0; hand < 2; hand++ ) {
		clXRHandInput_t *out = &sample->hands[hand];
		XrActionStateGetInfo get;
		out->pitchCorrection = CL_XRProfilePitch( ctx->profile[hand] );
		memset( &get, 0, sizeof( get ) );
		get.type = XR_TYPE_ACTION_STATE_GET_INFO;
		get.subactionPath = ctx->hands[hand];
		for ( i = 0; i < 3; i++ ) {
			XrActionStateFloat state;
			memset( &state, 0, sizeof( state ) );
			state.type = XR_TYPE_ACTION_STATE_FLOAT;
			get.action = ctx->actions[i == 2 ? CL_XRI_TRACKPAD : i ? CL_XRI_SQUEEZE : CL_XRI_TRIGGER];
			SAMPLE( ctx->xr.GetActionStateFloat( ctx->session, &get, &state ) );
			if ( state.isActive && VR_FloatFinite( state.currentState ) ) {
				if ( i == 2 ) {
					if ( state.currentState > .3f )
						out->buttons |= CL_XRI_TRACKPAD_BUTTON;
				} else if ( i )
					out->squeeze = state.currentState;
				else
					out->trigger = state.currentState;
				out->active = 1;
			}
		}
		{
			XrActionStateVector2f state;
			memset( &state, 0, sizeof( state ) );
			state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
			get.action = ctx->actions[CL_XRI_STICK];
			SAMPLE( ctx->xr.GetActionStateVector2f( ctx->session, &get, &state ) );
			if ( state.isActive && VR_FloatFinite( state.currentState.x ) &&
				VR_FloatFinite( state.currentState.y ) ) {
				out->stick[0] = state.currentState.x;
				out->stick[1] = state.currentState.y;
				out->active = 1;
			}
		}
		for ( i = CL_XRI_STICK_CLICK; i <= CL_XRI_MENU; i++ ) {
			static const unsigned bits[] = {CL_XRI_STICK_BUTTON, CL_XRI_PRIMARY_BUTTON,
											CL_XRI_SECONDARY_BUTTON, CL_XRI_MENU_BUTTON};
			XrActionStateBoolean state;
			memset( &state, 0, sizeof( state ) );
			state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
			get.action = ctx->actions[i];
			SAMPLE( ctx->xr.GetActionStateBoolean( ctx->session, &get, &state ) );
			if ( state.isActive ) {
				out->active = 1;
				if ( state.currentState )
					out->buttons |= bits[i - CL_XRI_STICK_CLICK];
			}
		}
		for ( i = CL_XRI_THUMBREST; i <= CL_XRI_SIMPLE_MENU; i++ ) {
			XrActionStateBoolean state;
			memset( &state, 0, sizeof( state ) );
			state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
			get.action = ctx->actions[i];
			SAMPLE( ctx->xr.GetActionStateBoolean( ctx->session, &get, &state ) );
			if ( state.isActive ) {
				out->active = 1;
				if ( state.currentState ) {
					if ( i == CL_XRI_THUMBREST )
						out->buttons |= CL_XRI_THUMBREST_BUTTON;
					/* Simple-controller menus map left to A and right to X, the other hand's primary. */
					else {
						sample->hands[1 - hand].buttons |= CL_XRI_PRIMARY_BUTTON;
						sample->hands[1 - hand].active = 1;
					}
				}
			}
		}
		for ( i = CL_XRI_BUMPER; i <= CL_XRI_SQUEEZE_CLICK; i++ ) {
			static const unsigned bits[] = {CL_XRI_BUMPER_BUTTON,	  CL_XRI_DPAD_UP_BUTTON,	CL_XRI_DPAD_DOWN_BUTTON,
											CL_XRI_DPAD_LEFT_BUTTON,  CL_XRI_DPAD_RIGHT_BUTTON, CL_XRI_VIEW_BUTTON,
											CL_XRI_X_BUTTON,		  CL_XRI_Y_BUTTON,			CL_XRI_SQUEEZE_CLICK_BUTTON};
			XrActionStateBoolean state;
			memset( &state, 0, sizeof( state ) );
			state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
			get.action = ctx->actions[i];
			SAMPLE( ctx->xr.GetActionStateBoolean( ctx->session, &get, &state ) );
			if ( state.isActive ) {
				out->active = 1;
				if ( state.currentState )
					out->buttons |= bits[i - CL_XRI_BUMPER];
			}
		}
		for ( i = 0; i < 2; i++ ) {
			XrActionStatePose state;
			XrSpaceLocation location;
			clXRPose_t *pose = i ? &out->aim : &out->grip;
			memset( &state, 0, sizeof( state ) );
			state.type = XR_TYPE_ACTION_STATE_POSE;
			get.action = ctx->actions[i ? CL_XRI_AIM_POSE : CL_XRI_GRIP_POSE];
			SAMPLE( ctx->xr.GetActionStatePose( ctx->session, &get, &state ) );
			if ( !state.isActive )
				continue;
			out->active = 1;
			memset( &location, 0, sizeof( location ) );
			location.type = XR_TYPE_SPACE_LOCATION;
			SAMPLE( ctx->xr.LocateSpace( ctx->spaces[hand][i], baseSpace, time, &location ) );
			pose->positionValid = !!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
			pose->orientationValid = !!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
			pose->positionTracked = !!(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
			pose->orientationTracked = !!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
			if ( !VR_FloatsFinite( (const float *)&location.pose.position, 3 ) )
				pose->positionValid = pose->positionTracked = 0;
			if ( !VR_FloatsFinite( (const float *)&location.pose.orientation, 4 ) )
				pose->orientationValid = pose->orientationTracked = 0;
			if ( pose->positionValid )
				memcpy( pose->position, &location.pose.position, sizeof( pose->position ) );
			if ( pose->orientationValid )
				memcpy( pose->orientation, &location.pose.orientation, sizeof( pose->orientation ) );
		}
	}
	ctx->focused = sample->focused = 1;
	return XR_SUCCESS;
fail:
	memset( sample, 0, sizeof( *sample ) );
	VK_XRInput_Reset( ctx );
	return result;
#undef SAMPLE
}

XrResult VK_XRInput_Haptic( vkXRInput_t *ctx, int hand, float amplitude, int durationMs ) {
	XrHapticActionInfo info;
	XrHapticVibration vibration;
	if ( !ctx || !ctx->session || hand < 0 || hand > 1 )
		return XR_ERROR_VALIDATION_FAILURE;
	if ( !ctx->focused )
		return XR_SESSION_NOT_FOCUSED;
	if ( !VR_FloatFinite( amplitude ) || !(amplitude >= 0) )
		amplitude = 0;
	if ( amplitude > 1 )
		amplitude = 1;
	if ( durationMs < 0 )
		durationMs = 0;
	if ( durationMs > 5000 )
		durationMs = 5000;
	memset( &info, 0, sizeof( info ) );
	info.type = XR_TYPE_HAPTIC_ACTION_INFO;
	info.action = ctx->actions[CL_XRI_HAPTIC];
	info.subactionPath = ctx->hands[hand];
	if ( !amplitude || !durationMs )
		return ctx->xr.StopHapticFeedback( ctx->session, &info );
	memset( &vibration, 0, sizeof( vibration ) );
	vibration.type = XR_TYPE_HAPTIC_VIBRATION;
	vibration.amplitude = amplitude;
	vibration.duration = (XrDuration)durationMs * 1000000;
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	return ctx->xr.ApplyHapticFeedback( ctx->session, &info, (const XrHapticBaseHeader *)&vibration );
}

void VK_XRInput_Shutdown( vkXRInput_t *ctx ) {
	unsigned hand, pose;
	if ( !ctx )
		return;
	VK_XRInput_Reset( ctx );
	for ( hand = 0; hand < 2; hand++ )
		for ( pose = 0; pose < 2; pose++ )
			if ( ctx->spaces[hand][pose] && ctx->xr.DestroySpace )
				ctx->xr.DestroySpace( ctx->spaces[hand][pose] );
	if ( ctx->set && ctx->xr.DestroyActionSet )
		ctx->xr.DestroyActionSet( ctx->set );
	memset( ctx, 0, sizeof( *ctx ) );
}
