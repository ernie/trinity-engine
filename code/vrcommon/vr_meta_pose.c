#include "vr_meta_pose.h"
#include <math.h>
#include <string.h>

/* Meta's loader poses each input and stick direction from a fixed keyframe instead of playing the clip. */
static const int metaKeyframes[VR_META_INPUT_COUNT] = { 5, 8, 24, 16, 21, 0 };
static const int metaStickKeyframes[8] = { 29, 39, 34, 40, 31, 36, 32, 37 }; /* N, NE, E, SE, S, SW, W, NW */
static const float metaStickDirections[8][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 } };
/* matched anywhere in a node's name, as Meta's loader does */
static const char *const metaInputNames[VR_META_INPUT_COUNT][2] = {
	{ "button_a", "button_x" }, { "button_b", "button_y" }, { "button_oculus", NULL },
	{ "trigger_front", NULL },	{ "trigger_grip", NULL },	{ "thumbstick", NULL } };

#define VR_META_STICK_DEADZONE 0.005f

int VR_MetaBindInputs( const vrModel_t *model, int map[VR_META_INPUT_COUNT] ) {
	int i, n, k, found = 0;
	for ( i = 0; i < VR_META_INPUT_COUNT; i++ ) {
		map[i] = -1;
		for ( n = 0; n < model->nodeCount && map[i] < 0; n++ )
			for ( k = 0; k < 2 && map[i] < 0; k++ )
				if ( metaInputNames[i][k] && strstr( model->nodes[n].name, metaInputNames[i][k] ) )
					map[i] = n;
		found += map[i] >= 0;
	}
	return found;
}

static float VRMP_Clamp( float t ) {
	return t < 0 ? 0 : t > 1 ? 1 : t;
}

/* Normalized lerp along the shorter arc, as Unity's Quaternion.Lerp blends the triggers. */
static void VRMP_Nlerp( const float a[4], const float b[4], float t, float out[4] ) {
	const float sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0f : 1.0f;
	float len = 0;
	int k;
	for ( k = 0; k < 4; k++ ) {
		out[k] = a[k] + ( sign * b[k] - a[k] ) * t;
		len += out[k] * out[k];
	}
	len = sqrtf( len );
	for ( k = 0; k < 4 && len > 0; k++ )
		out[k] /= len;
}

/* Spherical lerp along the shorter arc, t clamped as Unity's Quaternion.Slerp clamps it. */
static void VRMP_Slerp( const float a[4], const float b[4], float t, float out[4] ) {
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], wa, wb;
	const float sign = dot < 0 ? -1.0f : 1.0f;
	int k;
	t = VRMP_Clamp( t );
	dot *= sign;
	if ( dot > 0.9995f ) {
		VRMP_Nlerp( a, b, t, out );
		return;
	}
	{
		const float theta = acosf( dot ), s = sinf( theta );
		wa = sinf( ( 1 - t ) * theta ) / s;
		wb = sinf( t * theta ) / s * sign;
	}
	for ( k = 0; k < 4; k++ )
		out[k] = a[k] * wa + b[k] * wb;
}

/* The two directions the stick is between, as Meta's loader quarters the plane: index into the eight, or -1. */
static void VRMP_StickDirections( const float stick[2], int *first, int *second ) {
	const float x = stick[0], y = stick[1];
	*first = *second = -1;
	if ( sqrtf( x * x + y * y ) < VR_META_STICK_DEADZONE )
		return;
	if ( x >= 0 ) {
		if ( y >= 0 ) {
			*first = y > x ? 0 : 1; /* N NE or NE E */
			*second = y > x ? 1 : 2;
		} else {
			*first = x > -y ? 2 : 3; /* E SE or SE S */
			*second = x > -y ? 3 : 4;
		}
	} else if ( y < 0 ) {
		*first = x > y ? 4 : 5; /* S SW or SW W */
		*second = x > y ? 5 : 6;
	} else {
		*first = -x > y ? 6 : 7; /* W NW or NW N */
		*second = -x > y ? 7 : 0;
	}
}

/* The stick's barycentric coordinates against the two directions. */
static void VRMP_StickWeights( const float stick[2], int first, int second, float weights[2] ) {
	const float *a = metaStickDirections[first], *b = metaStickDirections[second];
	const float aa = a[0] * a[0] + a[1] * a[1], ab = a[0] * b[0] + a[1] * b[1], bb = b[0] * b[0] + b[1] * b[1];
	const float as = a[0] * stick[0] + a[1] * stick[1], bs = b[0] * stick[0] + b[1] * stick[1];
	const float inverse = 1 / ( aa * bb - ab * ab );
	weights[0] = ( bb * as - ab * bs ) * inverse;
	weights[1] = ( aa * bs - ab * as ) * inverse;
}

void VR_MetaPoseInputs( const vrModel_t *model, const int map[VR_META_INPUT_COUNT], const vrMetaInput_t *input,
						vrModelNodeState_t states[VR_META_INPUT_COUNT] ) {
	int i, k;
	for ( i = 0; i < VR_META_INPUT_COUNT; i++ ) {
		vrModelNodeState_t *state = &states[i];
		vrModelPose_t key;
		state->visible = 1;
		memset( &state->pose, 0, sizeof( state->pose ) );
		state->pose.orientation[3] = 1;
		if ( map[i] < 0 || map[i] >= model->nodeCount )
			continue;
		state->pose = model->nodes[map[i]].rest;
		if ( i == VR_META_THUMBSTICK ) {
			int first, second;
			float weights[2];
			VRMP_StickDirections( input->stick, &first, &second );
			if ( first < 0 )
				continue;
			VRMP_StickWeights( input->stick, first, second, weights );
			for ( k = 0; k < 2; k++ )
				if ( weights[k] != 0 && VR_ModelKeyframe( model, map[i], metaStickKeyframes[k ? second : first], &key ) )
					VRMP_Slerp( state->pose.orientation, key.orientation, weights[k], state->pose.orientation );
			continue;
		}
		if ( !VR_ModelKeyframe( model, map[i], metaKeyframes[i], &key ) )
			continue;
		if ( i == VR_META_TRIGGER_FRONT || i == VR_META_TRIGGER_GRIP ) {
			const float t = VRMP_Clamp( i == VR_META_TRIGGER_FRONT ? input->trigger : input->grip );
			for ( k = 0; k < 3; k++ )
				state->pose.position[k] += ( key.position[k] - state->pose.position[k] ) * t;
			VRMP_Nlerp( state->pose.orientation, key.orientation, t, state->pose.orientation );
		} else if ( i == VR_META_BUTTON_AX ? input->buttonAX : i == VR_META_BUTTON_BY ? input->buttonBY : input->menu )
			state->pose = key;
	}
}
