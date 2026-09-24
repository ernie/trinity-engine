#ifndef VR_RENDER_EXTENT_H
#define VR_RENDER_EXTENT_H
#include <math.h>
#include <stdint.h>
#include "vr_float.h"
/* Scale both axes together when either runtime or graphics limit is reached. */
static inline int VR_RenderExtent( uint32_t recommendedWidth, uint32_t recommendedHeight,
	uint32_t maxWidth, uint32_t maxHeight, float requested, uint32_t *width, uint32_t *height ) {
	double factor = requested, limit;
	if ( !recommendedWidth || !recommendedHeight || !maxWidth || !maxHeight ) {
		return 0;
	}
	if ( !VR_FloatFinite( requested ) || factor <= 0 ) {
		factor = 1;
	}
	limit = fmin( (double)maxWidth / recommendedWidth, (double)maxHeight / recommendedHeight );
	if ( factor > limit ) {
		factor = limit;
	}
	*width = (uint32_t)(recommendedWidth * factor);
	*height = (uint32_t)(recommendedHeight * factor);
	if ( !*width ) {
		*width = 1;
	}
	if ( !*height ) {
		*height = 1;
	}
	return 1;
}
#endif
