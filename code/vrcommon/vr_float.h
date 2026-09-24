#ifndef VR_FLOAT_H
#define VR_FLOAT_H
#include <stdint.h>
#include <string.h>
/* IEEE binary32 exponent inspection survives -ffinite-math-only/-ffast-math.
 * memcpy avoids aliasing through an incompatible integer pointer. */
static inline int VR_FloatFinite( float value ) {
	uint32_t bits;
	memcpy( &bits, &value, sizeof( bits ) );
	return (bits & UINT32_C( 0x7f800000 )) != UINT32_C( 0x7f800000 );
}
static inline int VR_FloatsFinite( const float *values, unsigned count ) {
	unsigned i;
	for ( i = 0; i < count; i++ )
		if ( !VR_FloatFinite( values[i] ) )
			return 0;
	return 1;
}
#endif
