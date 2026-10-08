#include "vk_xr_gaze.h"
#include "../vrcommon/vr_float.h"
#include <string.h>
#include <math.h>
XrResult VK_XRGaze_Init( vkXRGaze_t *ctx, XrInstance instance, XrSystemId system, XrActionSet set,
						 PFN_xrGetInstanceProcAddr proc, int enabled ) {
	PFN_xrGetSystemProperties properties;
	PFN_xrCreateAction create;
	PFN_xrStringToPath path;
	PFN_xrSuggestInteractionProfileBindings suggest;
	XrSystemProperties props;
	XrSystemEyeGazeInteractionPropertiesEXT gaze;
	XrActionCreateInfo action;
	XrActionSuggestedBinding binding;
	XrInteractionProfileSuggestedBinding profile;
	XrResult result;
	if ( !ctx )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( ctx, 0, sizeof( *ctx ) );
	if ( !enabled )
		return XR_SUCCESS;
	if ( !instance || !system || !set || !proc )
		return XR_ERROR_VALIDATION_FAILURE;
#define LOAD(out, name) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = proc( instance, #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) \
			return XR_FAILED( result ) ? result : XR_ERROR_FUNCTION_UNSUPPORTED; \
		out = (PFN_##name)fn; \
	} while ( 0 )
	LOAD( properties, xrGetSystemProperties );
	LOAD( create, xrCreateAction );
	LOAD( path, xrStringToPath );
	LOAD( suggest, xrSuggestInteractionProfileBindings );
	LOAD( ctx->createSpace, xrCreateActionSpace );
	LOAD( ctx->destroySpace, xrDestroySpace );
	LOAD( ctx->state, xrGetActionStatePose );
	LOAD( ctx->locate, xrLocateSpace );
#undef LOAD
	memset( &gaze, 0, sizeof( gaze ) );
	gaze.type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT;
	memset( &props, 0, sizeof( props ) );
	props.type = XR_TYPE_SYSTEM_PROPERTIES;
	props.next = &gaze;
	result = properties( instance, system, &props );
	if ( XR_FAILED( result ) || !gaze.supportsEyeGazeInteraction )
		return result;
	memset( &action, 0, sizeof( action ) );
	action.type = XR_TYPE_ACTION_CREATE_INFO;
	action.actionType = XR_ACTION_TYPE_POSE_INPUT;
	strcpy( action.actionName, "eye_gaze" );
	strcpy( action.localizedActionName, "Eye gaze" );
	result = create( set, &action, &ctx->action );
	if ( XR_FAILED( result ) )
		return result;
	memset( &profile, 0, sizeof( profile ) );
	profile.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	result = path( instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile.interactionProfile );
	if ( XR_FAILED( result ) )
		return result;
	binding.action = ctx->action;
	result = path( instance, "/user/eyes_ext/input/gaze_ext/pose", &binding.binding );
	if ( XR_FAILED( result ) )
		return result;
	profile.countSuggestedBindings = 1;
	profile.suggestedBindings = &binding;
	result = suggest( instance, &profile );
	if ( XR_SUCCEEDED( result ) )
		ctx->supported = 1;
	return result;
}
XrResult VK_XRGaze_Attach( vkXRGaze_t *ctx, XrSession session ) {
	XrActionSpaceCreateInfo info;
	XrResult result;
	if ( !ctx || !ctx->supported )
		return XR_SUCCESS;
	if ( !session || ctx->space )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( &info, 0, sizeof( info ) );
	info.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
	info.action = ctx->action;
	info.poseInActionSpace.orientation.w = 1;
	result = ctx->createSpace( session, &info, &ctx->space );
	if ( XR_SUCCEEDED( result ) )
		ctx->session = session;
	else
		ctx->supported = 0;
	return result;
}
/* v turned by the unit-normalized q, or by its inverse; 0 when q is degenerate. */
static int VKXRGaze_Rotate( const float q[4], int inverse, float v[3] ) {
	float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], x, y, z, w, t[3], r[3];
	if ( !VR_FloatFinite( n ) || n < .000001f )
		return 0;
	n = 1 / sqrtf( n );
	x = (inverse ? -q[0] : q[0]) * n;
	y = (inverse ? -q[1] : q[1]) * n;
	z = (inverse ? -q[2] : q[2]) * n;
	w = q[3] * n;
	t[0] = 2 * (y * v[2] - z * v[1]);
	t[1] = 2 * (z * v[0] - x * v[2]);
	t[2] = 2 * (x * v[1] - y * v[0]);
	r[0] = v[0] + w * t[0] + y * t[2] - z * t[1];
	r[1] = v[1] + w * t[1] + z * t[0] - x * t[2];
	r[2] = v[2] + w * t[2] + x * t[1] - y * t[0];
	memcpy( v, r, sizeof( r ) );
	return VR_FloatsFinite( v, 3 );
}
/* An eye sample older than this is no longer a fixation worth carrying forward. */
#define VK_XRGAZE_MAX_SAMPLE_AGE 200000000LL
int VK_XRGaze_Sample( vkXRGaze_t *ctx, XrSpace base, XrSpace view, const float head[4], XrTime time, int focused,
					  float direction[3] ) {
	const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	XrActionStateGetInfo get;
	XrActionStatePose state;
	XrSpaceLocation loc;
	XrEyeGazeSampleTimeEXT sampled;
	XrResult result;
	float q[4], n;
	memset( direction, 0, 3 * sizeof( float ) );
	direction[2] = -1;
	if ( !ctx || !ctx->space || !focused || !base || time <= 0 || !VR_FloatsFinite( head, 4 ) )
		return 0;
	memset( &get, 0, sizeof( get ) );
	get.type = XR_TYPE_ACTION_STATE_GET_INFO;
	get.action = ctx->action;
	memset( &state, 0, sizeof( state ) );
	state.type = XR_TYPE_ACTION_STATE_POSE;
	result = ctx->state( ctx->session, &get, &state );
	if ( XR_FAILED( result ) || !state.isActive )
		return 0;
	memset( &sampled, 0, sizeof( sampled ) );
	sampled.type = XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT;
	memset( &loc, 0, sizeof( loc ) );
	loc.type = XR_TYPE_SPACE_LOCATION;
	loc.next = &sampled;
	result = ctx->locate( ctx->space, base, time, &loc );
	if ( XR_FAILED( result ) || (loc.locationFlags & tracked) != tracked )
		return 0;
	memcpy( q, &loc.pose.orientation, sizeof( q ) );
	if ( !VR_FloatsFinite( q, 4 ) )
		return 0;
	n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	if ( !VR_FloatFinite( n ) || n < .000001f )
		return 0;
	direction[0] = -2 * (q[0] * q[2] + q[3] * q[1]) / n;
	direction[1] = 2 * (q[3] * q[0] - q[1] * q[2]) / n;
	direction[2] = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]) / n);
	if ( !VKXRGaze_Rotate( head, 1, direction ) )
		return 0;
	/* The runtime pairs its last eye sample with the head predicted for the display time. Eyes fixed on a point
	 * in the world counter-rotate through a head turn, so the sample is turned by the head's rotation since it;
	 * eyes riding along with the head (reading the HUD mid-turn) end up that far behind instead. */
	if ( view && sampled.time > 0 && sampled.time < time && time - sampled.time <= VK_XRGAZE_MAX_SAMPLE_AGE ) {
		XrSpaceLocation past;
		float then[4], world[3];
		memset( &past, 0, sizeof( past ) );
		past.type = XR_TYPE_SPACE_LOCATION;
		if ( XR_SUCCEEDED( ctx->locate( view, base, sampled.time, &past ) ) && (past.locationFlags & tracked) == tracked ) {
			memcpy( then, &past.pose.orientation, sizeof( then ) );
			memcpy( world, direction, sizeof( world ) );
			if ( VR_FloatsFinite( then, 4 ) && VKXRGaze_Rotate( then, 0, world ) && VKXRGaze_Rotate( head, 1, world ) )
				memcpy( direction, world, sizeof( world ) );
		}
	}
	return VR_FloatsFinite( direction, 3 );
}
void VK_XRGaze_Shutdown( vkXRGaze_t *ctx ) {
	if ( !ctx )
		return;
	if ( ctx->space && ctx->destroySpace )
		ctx->destroySpace( ctx->space );
	memset( ctx, 0, sizeof( *ctx ) );
}
XrResult VK_XRFovCenter_Init( vkXRFovCenter_t *ctx, XrInstance instance, XrSystemId system,
							  PFN_xrGetInstanceProcAddr proc, int enabled ) {
	PFN_xrGetSystemProperties properties;
	XrSystemProperties props;
	XrSystemFoveationEyeTrackedPropertiesMETA tracked;
	XrResult result;
	if ( !ctx )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( ctx, 0, sizeof( *ctx ) );
	if ( !enabled )
		return XR_SUCCESS;
	if ( !instance || !system || !proc )
		return XR_ERROR_VALIDATION_FAILURE;
#define LOAD(out, name) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = proc( instance, #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) \
			return XR_FAILED( result ) ? result : XR_ERROR_FUNCTION_UNSUPPORTED; \
		out = (PFN_##name)fn; \
	} while ( 0 )
	LOAD( properties, xrGetSystemProperties );
	LOAD( ctx->create, xrCreateFoveationProfileFB );
	LOAD( ctx->destroy, xrDestroyFoveationProfileFB );
	LOAD( ctx->update, xrUpdateSwapchainFB );
	LOAD( ctx->state, xrGetFoveationEyeTrackedStateMETA );
#undef LOAD
	memset( &tracked, 0, sizeof( tracked ) );
	tracked.type = XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META;
	memset( &props, 0, sizeof( props ) );
	props.type = XR_TYPE_SYSTEM_PROPERTIES;
	props.next = &tracked;
	result = properties( instance, system, &props );
	if ( XR_SUCCEEDED( result ) && tracked.supportsFoveationEyeTracked )
		ctx->supported = 1;
	return result;
}
int VK_XRFovCenter_Sample( vkXRFovCenter_t *ctx, XrSession session, XrSwapchain swapchain, float center[2][2] ) {
	XrSwapchainStateFoveationFB apply;
	XrFoveationEyeTrackedStateMETA state;
	unsigned eye;
	if ( !ctx || !ctx->supported || ctx->failed || !session || !swapchain )
		return 0;
	if ( ctx->session != session || !ctx->profile ) {
		XrFoveationEyeTrackedProfileCreateInfoMETA tracked;
		XrFoveationLevelProfileCreateInfoFB level;
		XrFoveationProfileCreateInfoFB info;
		memset( &tracked, 0, sizeof( tracked ) );
		tracked.type = XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META;
		/* the engine draws its own density map; the level only has to foveate */
		memset( &level, 0, sizeof( level ) );
		level.type = XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB;
		level.next = &tracked;
		level.level = XR_FOVEATION_LEVEL_HIGH_FB;
		level.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
		memset( &info, 0, sizeof( info ) );
		info.type = XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB;
		info.next = &level;
		/* a profile from an earlier session went with it */
		ctx->profile = XR_NULL_HANDLE;
		ctx->session = session;
		if ( XR_FAILED( ctx->create( session, &info, &ctx->profile ) ) ) {
			ctx->profile = XR_NULL_HANDLE;
			ctx->failed = 1;
			return -1;
		}
	}
	/* the runtime recenters a profile only when it is handed to the swapchain again */
	memset( &apply, 0, sizeof( apply ) );
	apply.type = XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB;
	apply.profile = ctx->profile;
	memset( &state, 0, sizeof( state ) );
	state.type = XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META;
	if ( XR_FAILED( ctx->update( swapchain, (const XrSwapchainStateBaseHeaderFB *)&apply ) ) ||
		 XR_FAILED( ctx->state( session, &state ) ) ) {
		ctx->failed = 1;
		return -1;
	}
	if ( !(state.flags & XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META) )
		return 0;
	for ( eye = 0; eye < 2; eye++ ) {
		center[eye][0] = state.foveationCenter[eye].x;
		center[eye][1] = state.foveationCenter[eye].y;
	}
	return 1;
}
void VK_XRFovCenter_Shutdown( vkXRFovCenter_t *ctx ) {
	if ( !ctx )
		return;
	if ( ctx->profile && ctx->destroy )
		ctx->destroy( ctx->profile );
	memset( ctx, 0, sizeof( *ctx ) );
}
