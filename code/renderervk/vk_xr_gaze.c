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
int VK_XRGaze_Sample( vkXRGaze_t *ctx, XrSpace base, const float head[4], XrTime time, int focused,
					  float direction[3] ) {
	XrActionStateGetInfo get;
	XrActionStatePose state;
	XrSpaceLocation loc;
	XrResult result;
	float q[4], n, v[3], x, y, z, w, t[3];
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
	memset( &loc, 0, sizeof( loc ) );
	loc.type = XR_TYPE_SPACE_LOCATION;
	result = ctx->locate( ctx->space, base, time, &loc );
	if ( XR_FAILED( result ) ||
		(loc.locationFlags &
		 (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) !=
			(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) )
		return 0;
	memcpy( q, &loc.pose.orientation, sizeof( q ) );
	if ( !VR_FloatsFinite( q, 4 ) )
		return 0;
	n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	if ( !VR_FloatFinite( n ) || n < .000001f )
		return 0;
	v[0] = -2 * (q[0] * q[2] + q[3] * q[1]) / n;
	v[1] = 2 * (q[3] * q[0] - q[1] * q[2]) / n;
	v[2] = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]) / n);
	n = head[0] * head[0] + head[1] * head[1] + head[2] * head[2] + head[3] * head[3];
	if ( !VR_FloatFinite( n ) || n < .000001f )
		return 0;
	n = 1 / sqrtf( n );
	x = -head[0] * n;
	y = -head[1] * n;
	z = -head[2] * n;
	w = head[3] * n;
	t[0] = 2 * (y * v[2] - z * v[1]);
	t[1] = 2 * (z * v[0] - x * v[2]);
	t[2] = 2 * (x * v[1] - y * v[0]);
	direction[0] = v[0] + w * t[0] + y * t[2] - z * t[1];
	direction[1] = v[1] + w * t[1] + z * t[0] - x * t[2];
	direction[2] = v[2] + w * t[2] + x * t[1] - y * t[0];
	return VR_FloatsFinite( direction, 3 );
}
void VK_XRGaze_Shutdown( vkXRGaze_t *ctx ) {
	if ( !ctx )
		return;
	if ( ctx->space && ctx->destroySpace )
		ctx->destroySpace( ctx->space );
	memset( ctx, 0, sizeof( *ctx ) );
}
