#include "vk_shadowtile_math.h"

static float Dot3( const float a[3], const float b[3] ) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void Cross3( const float a[3], const float b[3], float out[3] ) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float Normalize3( float v[3] ) {
	float l = sqrtf( Dot3( v, v ) );
	if ( l > 0.0f ) {
		v[0] /= l; v[1] /= l; v[2] /= l;
	}
	return l;
}

void ShadowTile_Basis( const float travel[3], float axis[3][3] ) {
	float ref[3] = { 0, 0, 0 };
	float a0 = fabsf( travel[0] ), a1 = fabsf( travel[1] ), a2 = fabsf( travel[2] );

	memcpy( axis[0], travel, sizeof( axis[0] ) );
	Normalize3( axis[0] );
	ref[ ( a0 <= a1 && a0 <= a2 ) ? 0 : ( a1 <= a2 ) ? 1 : 2 ] = 1.0f;
	Cross3( axis[0], ref, axis[1] );
	Normalize3( axis[1] );
	Cross3( axis[0], axis[1], axis[2] );
}

static void ShadowTile_FitInBasis( const float mins[3], const float maxs[3], float depthRange, shadowTileFit_t *out ) {
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	float mid1, mid2, half;
	int i, k;

	for ( i = 0; i < 8; i++ ) {
		const float c[3] = { ( i & 1 ) ? maxs[0] : mins[0], ( i & 2 ) ? maxs[1] : mins[1], ( i & 4 ) ? maxs[2] : mins[2] };
		for ( k = 0; k < 3; k++ ) {
			float d = Dot3( c, out->axis[k] );
			if ( d < lo[k] ) lo[k] = d;
			if ( d > hi[k] ) hi[k] = d;
		}
	}
	mid1 = 0.5f * ( lo[1] + hi[1] );
	mid2 = 0.5f * ( lo[2] + hi[2] );
	/* a square footprint keeps texels square, so one uv-per-unit serves both tile axes */
	half = 0.5f * ( ( hi[1] - lo[1] ) > ( hi[2] - lo[2] ) ? ( hi[1] - lo[1] ) : ( hi[2] - lo[2] ) );
	if ( half < 1.0f ) {
		half = 1.0f;
	}
	out->halfW = out->halfH = half;
	out->nearDepth = lo[0];
	out->casterDepth = hi[0] - lo[0];
	out->depthRange = depthRange;
	for ( k = 0; k < 3; k++ ) {
		out->center[k] = mid1 * out->axis[1][k] + mid2 * out->axis[2][k] + lo[0] * out->axis[0][k];
	}
}

int ShadowTile_Fit( const float mins[3], const float maxs[3], const float travel[3], float depthRange, shadowTileFit_t *out ) {
	if ( Dot3( travel, travel ) <= 0.0f ) {
		return 0;
	}
	ShadowTile_Basis( travel, out->axis );
	ShadowTile_FitInBasis( mins, maxs, depthRange, out );
	return 1;
}

int ShadowTile_FitAligned( const float mins[3], const float maxs[3], const float travel[3], const float side[3], float depthRange,
		shadowTileFit_t *out ) {
	float along;
	int k;

	if ( Dot3( travel, travel ) <= 0.0f ) {
		return 0;
	}
	memcpy( out->axis[0], travel, sizeof( out->axis[0] ) );
	Normalize3( out->axis[0] );
	along = Dot3( side, out->axis[0] );
	for ( k = 0; k < 3; k++ ) {
		out->axis[1][k] = side[k] - along * out->axis[0][k];
	}
	if ( Normalize3( out->axis[1] ) < 1e-3f ) {
		ShadowTile_Basis( travel, out->axis );
	} else {
		Cross3( out->axis[0], out->axis[1], out->axis[2] );
	}
	ShadowTile_FitInBasis( mins, maxs, depthRange, out );
	return 1;
}

void ShadowTile_WorldMatrix( const shadowTileFit_t *fit, float m[16] ) {
	const float *rows[3] = { fit->axis[1], fit->axis[2], fit->axis[0] };
	const float scale[3] = { 1.0f / fit->halfW, 1.0f / fit->halfH, 1.0f / fit->depthRange };
	const float offset[3] = { Dot3( fit->center, fit->axis[1] ), Dot3( fit->center, fit->axis[2] ), fit->nearDepth };
	int i, j;

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			m[j * 4 + i] = rows[i][j] * scale[i];
		}
		m[3 * 4 + i] = -offset[i] * scale[i];
	}
	m[3] = m[7] = m[11] = 0.0f;
	m[15] = 1.0f;
}

void ShadowTile_CasterMatrix( const shadowTileFit_t *fit, const float origin[3], const float axis[3][3], float mvp[16] ) {
	float w[16], model[16];
	int i, j, k;

	ShadowTile_WorldMatrix( fit, w );
	for ( j = 0; j < 3; j++ ) {
		for ( i = 0; i < 3; i++ ) {
			model[j * 4 + i] = axis[j][i];
		}
		model[j * 4 + 3] = 0.0f;
		model[3 * 4 + j] = origin[j];
	}
	model[15] = 1.0f;
	for ( j = 0; j < 4; j++ ) {
		for ( i = 0; i < 4; i++ ) {
			float s = 0.0f;
			for ( k = 0; k < 4; k++ ) {
				s += w[k * 4 + i] * model[j * 4 + k];
			}
			mvp[j * 4 + i] = s;
		}
	}
}

void ShadowTile_Project( const shadowTileFit_t *fit, const int rect[4], int atlasWidth, int atlasHeight, const float world[3], float out[3] ) {
	float x = ( Dot3( world, fit->axis[1] ) - Dot3( fit->center, fit->axis[1] ) ) / fit->halfW;
	float y = ( Dot3( world, fit->axis[2] ) - Dot3( fit->center, fit->axis[2] ) ) / fit->halfH;

	out[0] = ( rect[0] + ( x * 0.5f + 0.5f ) * rect[2] ) / atlasWidth;
	out[1] = ( rect[1] + ( y * 0.5f + 0.5f ) * rect[3] ) / atlasHeight;
	out[2] = ( Dot3( world, fit->axis[0] ) - fit->nearDepth ) / fit->depthRange;
}

float ShadowTile_UvPerUnit( const shadowTileFit_t *fit, const int rect[4], int atlasWidth ) {
	return rect[2] / ( 2.0f * fit->halfW ) / atlasWidth;
}

void ShadowTile_Footprint( const shadowTileFit_t *fit, float points[4][3] ) {
	static const float sign[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	int i, k;

	for ( i = 0; i < 4; i++ ) {
		for ( k = 0; k < 3; k++ ) {
			points[i][k] = fit->center[k] + sign[i][0] * fit->halfW * fit->axis[1][k] + sign[i][1] * fit->halfH * fit->axis[2][k];
		}
	}
}

int ShadowTile_Allocate( const int *large, int count, int atlasWidth, int atlasHeight, int largeSize, int smallSize, int maxLarge, int (*rects)[4] ) {
	int perLargeRow = atlasWidth / largeSize, perSmallRow = atlasWidth / smallSize;
	int largeRows = atlasHeight / largeSize, numLarge = 0, numSmall = 0, smallTop, smallCap, i;

	if ( maxLarge > perLargeRow * largeRows ) {
		maxLarge = perLargeRow * largeRows;
	}
	for ( i = 0; i < count; i++ ) {
		rects[i][0] = rects[i][1] = rects[i][2] = rects[i][3] = 0;
		if ( large[i] && numLarge < maxLarge ) {
			rects[i][0] = ( numLarge % perLargeRow ) * largeSize;
			rects[i][1] = ( numLarge / perLargeRow ) * largeSize;
			rects[i][2] = rects[i][3] = largeSize;
			numLarge++;
		}
	}
	smallTop = ( ( numLarge + perLargeRow - 1 ) / perLargeRow ) * largeSize;
	smallCap = ( ( atlasHeight - smallTop ) / smallSize ) * perSmallRow;
	for ( i = 0; i < count; i++ ) {
		if ( !rects[i][2] && numSmall < smallCap ) {
			rects[i][0] = ( numSmall % perSmallRow ) * smallSize;
			rects[i][1] = smallTop + ( numSmall / perSmallRow ) * smallSize;
			rects[i][2] = rects[i][3] = smallSize;
			numSmall++;
		}
	}
	return numLarge + numSmall;
}

float ShadowTile_Darkness( const float ambient[3], const float directed[3], float strength ) {
	float a = sqrtf( Dot3( ambient, ambient ) ), d = sqrtf( Dot3( directed, directed ) );

	if ( a + d <= 0.0f ) {
		return 0.0f;
	}
	return strength * d / ( a + d );
}

void ShadowTile_SpinBounds( const float origin[3], const float localMins[3], const float localMaxs[3], float xyScale, float zScale,
		float cellHeight, float mins[3], float maxs[3] ) {
	float x = fmaxf( fabsf( localMins[0] ), fabsf( localMaxs[0] ) ), y = fmaxf( fabsf( localMins[1] ), fabsf( localMaxs[1] ) );
	float r = xyScale * sqrtf( x * x + y * y );
	float base = floorf( origin[2] / cellHeight ) * cellHeight;

	mins[0] = origin[0] - r;
	mins[1] = origin[1] - r;
	mins[2] = base + localMins[2] * zScale;
	maxs[0] = origin[0] + r;
	maxs[1] = origin[1] + r;
	maxs[2] = base + cellHeight + localMaxs[2] * zScale;
}

/* rounding each component to 1/64 turns a unit travel by at most asin( sqrt( 3 ) / 128 ) */
float ShadowTile_QuantizePad( float reach, float half ) {
	return ( reach + 3.0f * half ) * 0.01354f + 0.1f;
}

void ShadowTile_QuantizeTravel( const float travel[3], int key[3], float out[3] ) {
	float len;
	int k;

	for ( k = 0; k < 3; k++ ) {
		key[k] = (int)floorf( travel[k] * 64.0f + 0.5f );
	}
	len = sqrtf( (float)( key[0] * key[0] + key[1] * key[1] + key[2] * key[2] ) );
	for ( k = 0; k < 3; k++ ) {
		out[k] = len > 0.0f ? key[k] / len : 0.0f;
	}
}

int ShadowTile_SweptBoxCulled( const float mins[3], const float maxs[3], const float travel[3], float reach,
		const float (*planes)[4], int numPlanes ) {
	int p, k;

	for ( p = 0; p < numPlanes; p++ ) {
		float front = 0.0f, sweep = reach * Dot3( planes[p], travel );
		for ( k = 0; k < 3; k++ ) {
			front += planes[p][k] * ( planes[p][k] > 0.0f ? maxs[k] : mins[k] );
		}
		if ( front + ( sweep > 0.0f ? sweep : 0.0f ) < planes[p][3] ) {
			return 1;
		}
	}
	return 0;
}
