#include <math.h>
#include "vk_mirror_math.h"

#define VKM_DEG2RAD( a ) ( (a) * 0.017453292519943295 )
#define VKM_RAD2DEG( a ) ( (a) * 57.29577951308232 )

int VKM_StabilizedFov( const float tangents[4], float aspect, float seconds ) {
	double margin = VKM_DEG2RAD( fmax( MIRROR_PAN_SPEED * seconds, MIRROR_MARGIN_FLOOR ) );
	double across = fmin( atan( -tangents[0] ), atan( tangents[1] ) ) - margin;
	double tall = fmin( atan( tangents[2] ), atan( -tangents[3] ) ) - margin;
	double halfW;
	if ( across <= 0 || tall <= 0 || aspect <= 0 )
		return 0;
	halfW = fmin( tan( across ), tan( tall ) / aspect );
	/* a span that lands on a step stays on it */
	return (int)( floor( VKM_RAD2DEG( 2 * atan( halfW ) ) / 10 + 1e-4 ) * 10 );
}

float VKM_MirrorFov( float fov, const float tangents[4], float aspect, float seconds ) {
	int limit = seconds > 0 ? VKM_StabilizedFov( tangents, aspect, seconds ) : 0;
	if ( limit && ( fov <= 0 || fov > limit ) )
		return (float)limit;
	return fov > 0 ? fov : 0;
}

float VKM_Span( float fov, const float tangents[4], int width, int height, int maxWidth, int maxHeight, float *halfW,
				float *halfH ) {
	double l = tangents[0], r = tangents[1], u = tangents[2], d = tangents[3];
	double aspect, half, fit, least;
	if ( fov <= 0 || width < 1 || height < 1 || maxWidth < 2 || maxHeight < 2 || !( l < 0 && r > 0 && u > 0 && d < 0 ) )
		return 0;
	aspect = (double)height / width;
	half = tan( VKM_DEG2RAD( fov ) / 2 );
	fit = fmin( fmin( -l, r ), fmin( u, -d ) / aspect );
	/* a pixel short of the limits, so the float viewport rounds inside them */
	least = width * fmax( ( r - l ) / ( maxWidth - 1 ), ( u - d ) / ( maxHeight - 1 ) ) / 2;
	*halfW = (float)fmax( fmin( half, fit ), least );
	*halfH = (float)( *halfW * aspect );
	if ( half <= fit && half >= least )
		return fov;
	return (float)VKM_RAD2DEG( 2 * atan( *halfW ) );
}

void VKM_Follow( float smoothed[3], const float current[3], float dt, float seconds ) {
	double dot = smoothed[0] * current[0] + smoothed[1] * current[1] + smoothed[2] * current[2];
	double angle, keep, a, b, length;
	int i;
	if ( dot > 1 )
		dot = 1;
	if ( dot < -1 )
		dot = -1;
	angle = acos( dot );
	if ( seconds <= 0 || angle < 1e-6 || sin( angle ) < 1e-6 ) {
		for ( i = 0; i < 3; i++ )
			smoothed[i] = current[i];
		return;
	}
	keep = exp( -dt / seconds );
	a = sin( ( 1 - keep ) * angle ) / sin( angle );
	b = sin( keep * angle ) / sin( angle );
	length = 0;
	for ( i = 0; i < 3; i++ ) {
		smoothed[i] = (float)( a * current[i] + b * smoothed[i] );
		length += smoothed[i] * smoothed[i];
	}
	length = sqrt( length );
	for ( i = 0; i < 3; i++ )
		smoothed[i] = (float)( smoothed[i] / length );
}

static float VKM_Hold( float value, float low, float high ) {
	if ( low > high )
		return ( low + high ) * 0.5f;
	return value < low ? low : value > high ? high : value;
}

void VKM_HoldCenter( const float tangents[4], float halfW, float halfH, float center[2] ) {
	center[0] = VKM_Hold( center[0], tangents[0] + halfW, tangents[1] - halfW );
	center[1] = VKM_Hold( center[1], tangents[3] + halfH, tangents[2] - halfH );
}

void VKM_CropCenter( const float axis[3][3], float smoothed[3], const float tangents[4], float halfW, float halfH,
					 float center[2] ) {
	float local[3], held[2], length;
	int i;
	for ( i = 0; i < 3; i++ )
		local[i] = smoothed[0] * axis[i][0] + smoothed[1] * axis[i][1] + smoothed[2] * axis[i][2];
	if ( local[2] <= 1e-3f ) {
		for ( i = 0; i < 3; i++ )
			smoothed[i] = axis[2][i];
		center[0] = center[1] = 0;
		return;
	}
	center[0] = local[0] / local[2];
	center[1] = local[1] / local[2];
	held[0] = center[0];
	held[1] = center[1];
	VKM_HoldCenter( tangents, halfW, halfH, held );
	if ( held[0] == center[0] && held[1] == center[1] )
		return;
	center[0] = held[0];
	center[1] = held[1];
	length = 0;
	for ( i = 0; i < 3; i++ ) {
		smoothed[i] = axis[2][i] + held[0] * axis[0][i] + held[1] * axis[1][i];
		length += smoothed[i] * smoothed[i];
	}
	length = sqrtf( length );
	for ( i = 0; i < 3; i++ )
		smoothed[i] /= length;
}
