#include "vk_foveation_math.h"
#include "../vrcommon/vr_float.h"
#define FOV_PI 3.14159265358979323846f
int VK_FovMode( int requested, int attachment, int gaze ) {
	if ( !attachment || requested <= 0 )
		return 0;
	return requested >= 2 && gaze ? 2 : 1;
}
uint8_t VK_FovLegalRate( const vkFovRate_t *rates, uint32_t count, uint32_t width, uint32_t height,
						 uint32_t samples ) {
	uint32_t i, w = 1, h = 1, lw = 0, lh = 0;
	if ( !rates )
		return 0;
	for ( i = 0; i < count; i++ ) {
		uint32_t rw = rates[i].width, rh = rates[i].height;
		if ( !rw || !rh || rw > 4 || rh > 4 || (rw & (rw - 1)) || (rh & (rh - 1)) )
			continue;
		if ( rw <= width && rh <= height && (rates[i].samples & samples) && rw * rh > w * h ) {
			w = rw;
			h = rh;
		}
	}
	while ( (1u << lw) < w )
		lw++;
	while ( (1u << lh) < h )
		lh++;
	return (uint8_t)((lw << 2) | lh);
}
static int rotate( const float q[4], const float v[3], int inverse, float out[3] ) {
	float x, y, z, w, n, t[3];
	if ( !VR_FloatsFinite( q, 4 ) || !VR_FloatsFinite( v, 3 ) )
		return 0;
	n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	if ( !VR_FloatFinite( n ) || n < .000001f )
		return 0;
	n = 1 / sqrtf( n );
	x = q[0] * n;
	y = q[1] * n;
	z = q[2] * n;
	w = q[3] * n;
	if ( inverse ) {
		x = -x;
		y = -y;
		z = -z;
	}
	t[0] = 2 * (y * v[2] - z * v[1]);
	t[1] = 2 * (z * v[0] - x * v[2]);
	t[2] = 2 * (x * v[1] - y * v[0]);
	out[0] = v[0] + w * t[0] + y * t[2] - z * t[1];
	out[1] = v[1] + w * t[1] + z * t[0] - x * t[2];
	out[2] = v[2] + w * t[2] + x * t[1] - y * t[0];
	return VR_FloatsFinite( out, 3 );
}
static int project( const float head[4], const float eye[2][4], const float tangent[2][4],
					const float direction[3], float center[2][2] ) {
	float world[3];
	unsigned i;
	if ( !rotate( head, direction, 0, world ) )
		return 0;
	for ( i = 0; i < 2; i++ ) {
		float v[3], sx = tangent[i][1] - tangent[i][0], sy = tangent[i][2] - tangent[i][3];
		if ( !rotate( eye[i], world, 1, v ) || v[2] >= -.01f || sx < .000001f || sy < .000001f )
			return 0;
		center[i][0] = 2 * (v[0] / -v[2] - tangent[i][0]) / sx - 1;
		center[i][1] = 1 - 2 * (v[1] / -v[2] - tangent[i][3]) / sy;
		if ( !VR_FloatsFinite( center[i], 2 ) )
			return 0;
	}
	return 1;
}
/* Rate texels are coarse; sub-1/1024 NDC jitter must not trigger a re-upload. */
static void quantize( float center[2][2] ) {
	unsigned i, j;
	for ( i = 0; i < 2; i++ )
		for ( j = 0; j < 2; j++ )
			center[i][j] = roundf( center[i][j] * 1024.0f ) / 1024.0f;
}
void VK_FovCenters( vkFovCenters_t *s, const float head[4], const float eye[2][4], const float fov[2][4],
					int mode, int screen, int scoped, const float gaze[3], int gazeValid, int64_t time ) {
	unsigned i, j;
	float fixed[3], centers[2][2], mid;
	s->valid = 0;
	for ( i = 0; i < 2; i++ )
		for ( j = 0; j < 4; j++ ) {
			if ( !VR_FloatFinite( fov[i][j] ) || fov[i][j] <= -FOV_PI / 2 || fov[i][j] >= FOV_PI / 2 )
				return;
			s->tangent[i][j] = tanf( fov[i][j] );
		}
	if ( scoped ) {
		memset( s->center, 0, sizeof( s->center ) );
		s->eyeTracked = 0;
		s->valid = 1;
		return;
	}
	if ( mode == 2 && !screen && gazeValid && project( head, eye, s->tangent, gaze, centers ) ) {
		memcpy( s->center, centers, sizeof( centers ) );
		quantize( s->center );
		s->eyeTracked = 1;
		s->heldTime = time;
		s->valid = 1;
		return;
	}
	if ( mode == 2 && !screen && s->eyeTracked && time >= s->heldTime && time - s->heldTime <= 1000000000LL ) {
		s->valid = 1;
		return;
	}
	mid = (fov[0][2] + fov[0][3] + fov[1][2] + fov[1][3]) * .25f;
	fixed[0] = 0;
	fixed[1] = sinf( mid );
	fixed[2] = -cosf( mid );
	s->eyeTracked = 0;
	s->valid = project( head, eye, s->tangent, fixed, s->center );
	if ( s->valid )
		quantize( s->center );
}
/* Degrees from the center: full rate out to sharp, coarsest past coarse. */
static void angles( int strength, int eyeTracked, float *sharp, float *coarse ) {
	if ( eyeTracked ) {
		*sharp = strength == 1 ? 20 : strength == 2 ? 16 : 12;
		*coarse = strength == 1 ? 33 : strength == 2 ? 27 : 21;
	} else {
		*sharp = strength == 1 ? 30 : strength == 2 ? 25 : 22;
		*coarse = strength == 1 ? 41 : strength == 2 ? 35 : 28;
	}
}
int VK_FovWrite( uint8_t *dst, size_t capacity, const vkFovMap_t *m, const vkFovRate_t *rates,
				 uint32_t count ) {
	uint32_t width, height, x, y, e;
	float sharp, coarse, cs, cc;
	uint8_t middle, floor;
	if ( !dst || !m || !m->width || !m->height || !m->texelWidth || !m->texelHeight )
		return 0;
	width = m->width / m->texelWidth + (m->width % m->texelWidth != 0);
	height = m->height / m->texelHeight + (m->height % m->texelHeight != 0);
	if ( height > capacity / width / 2 )
		return 0;
	memset( dst, 0, (size_t)width * height * 2 );
	if ( m->strength <= 0 )
		return 1;
	for ( e = 0; e < 2; e++ ) {
		if ( !VR_FloatsFinite( m->center[e], 2 ) || !VR_FloatsFinite( m->tangent[e], 4 ) ||
			m->tangent[e][1] <= m->tangent[e][0] || m->tangent[e][2] <= m->tangent[e][3] )
			return 0;
		if ( m->eye[e].x > m->width || m->eye[e].width > m->width - m->eye[e].x || m->eye[e].y > m->height ||
			m->eye[e].height > m->height - m->eye[e].y )
			return 0;
	}
	angles( m->strength, m->eyeTracked, &sharp, &coarse );
	cs = cosf( sharp * FOV_PI / 180 );
	cc = cosf( coarse * FOV_PI / 180 );
	middle = VK_FovLegalRate( rates, count, 2, 2, m->samples );
	floor = VK_FovLegalRate( rates, count, 4, 4, m->samples );
	for ( y = 0; y < height; y++ )
		for ( x = 0; x < width; x++ )
			for ( e = 0; e < 2; e++ ) {
				const vkFovRect_t *r = &m->eye[e];
				uint64_t px = (uint64_t)x * m->texelWidth, py = (uint64_t)y * m->texelHeight;
				float sx, sy, gx, gy, tx, ty, dot, len, glen;
				if ( !r->width || !r->height || px < r->x || py < r->y ||
					px + m->texelWidth > (uint64_t)r->x + r->width ||
					py + m->texelHeight > (uint64_t)r->y + r->height )
					continue;
				sx = m->tangent[e][1] - m->tangent[e][0];
				sy = m->tangent[e][2] - m->tangent[e][3];
				gx = m->tangent[e][0] + (m->center[e][0] + 1) * .5f * sx;
				gy = m->tangent[e][2] - (m->center[e][1] + 1) * .5f * sy;
				tx = m->tangent[e][0] + ((float)(px - r->x) + m->texelWidth * .5f) / r->width * sx;
				ty = m->tangent[e][2] - ((float)(py - r->y) + m->texelHeight * .5f) / r->height * sy;
				dot = tx * gx + ty * gy + 1;
				len = tx * tx + ty * ty + 1;
				glen = gx * gx + gy * gy + 1;
				dst[((size_t)e * height + y) * width + x] = dot <= 0						   ? floor
															: dot * dot > cs * cs * glen * len ? 0
															: dot * dot > cc * cc * glen * len ? middle
																							   : floor;
			}
	return 1;
}
uint32_t VK_FdmMapWidth( const vkFdmGeometry_t *g ) {
	return g->texelWidth ? (g->width + g->texelWidth - 1) / g->texelWidth : 0;
}
uint32_t VK_FdmMapHeight( const vkFdmGeometry_t *g ) {
	return g->texelHeight ? (g->height + g->texelHeight - 1) / g->texelHeight : 0;
}
size_t VK_FdmBytes( const vkFdmGeometry_t *g ) {
	return (size_t)VK_FdmMapWidth( g ) * VK_FdmMapHeight( g ) * 2 * 2;
}
void VK_FdmReference( const vkFdmGeometry_t *g, const float tangent[2][4], int32_t ref[2][2] ) {
	const float width = (float)g->width, height = (float)g->height;
	unsigned e;
	for ( e = 0; e < 2; e++ ) {
		const float sx = tangent[e][1] - tangent[e][0], sy = tangent[e][2] - tangent[e][3];
		float x = sx > .000001f ? -tangent[e][0] / sx * width : .5f * width;
		float y = sy > .000001f ? tangent[e][2] / sy * height : .5f * height;
		x = x < 0 ? 0 : x > width - 1 ? width - 1 : x;
		y = y < 0 ? 0 : y > height - 1 ? height - 1 : y;
		if ( g->tileWidth && g->tileHeight ) {
			ref[e][0] = (int32_t)((uint32_t)x / g->tileWidth * g->tileWidth + g->tileWidth / 2);
			ref[e][1] = (int32_t)((uint32_t)y / g->tileHeight * g->tileHeight + g->tileHeight / 2);
		} else {
			ref[e][0] = (int32_t)x;
			ref[e][1] = (int32_t)y;
		}
	}
}
/* Densities 1x1, 2x2, 2x4, 4x4; each sits inside its fragment area so the device rounds it square. */
static uint8_t density( float value, float sharp, float mid, float coarse ) {
	return value > sharp ? 255 : value > mid ? 127 : value > coarse ? 64 : 63;
}
static int fdmBegin( uint8_t *dst, size_t capacity, const vkFdmGeometry_t *g, int strength ) {
	size_t bytes = VK_FdmBytes( g );
	if ( !dst || !bytes || !g->width || !g->height || capacity < bytes )
		return 0;
	if ( strength <= 0 )
		memset( dst, 0xFF, bytes );
	return 1;
}
int VK_FdmWriteFixed( uint8_t *dst, size_t capacity, const vkFdmGeometry_t *g, const float tangent[2][4],
					  const int32_t ref[2][2], int strength, int eyeTracked ) {
	const uint32_t width = VK_FdmMapWidth( g ), height = VK_FdmMapHeight( g );
	const int binned = g->tileWidth && g->tileHeight;
	float sharp, coarse, s2, m2, c2, t;
	uint32_t e, x, y;
	if ( !fdmBegin( dst, capacity, g, strength ) )
		return 0;
	if ( strength <= 0 )
		return 1;
	angles( strength, eyeTracked, &sharp, &coarse );
	t = tanf( sharp * FOV_PI / 180 );
	s2 = t * t;
	t = tanf( (sharp + coarse) * .5f * FOV_PI / 180 );
	m2 = t * t;
	t = tanf( coarse * FOV_PI / 180 );
	c2 = t * t;
	for ( e = 0; e < 2; e++ ) {
		const float perX = (tangent[e][1] - tangent[e][0]) / g->width;
		const float perY = (tangent[e][2] - tangent[e][3]) / g->height;
		const float rx = (float)ref[e][0], ry = (float)ref[e][1];
		for ( y = 0; y < height; y++ ) {
			float py = (y + .5f) * g->texelHeight, dy;
			/* A texel takes its bin's point nearest the reference, so a bin the sharp region reaches is sharp throughout. */
			if ( binned ) {
				const float y0 = floorf( py / g->tileHeight ) * g->tileHeight;
				py = ry < y0 ? y0 : ry > y0 + g->tileHeight ? y0 + g->tileHeight : ry;
			}
			dy = (py - ry) * perY;
			for ( x = 0; x < width; x++ ) {
				float px = (x + .5f) * g->texelWidth, dx;
				if ( binned ) {
					const float x0 = floorf( px / g->tileWidth ) * g->tileWidth;
					px = rx < x0 ? x0 : rx > x0 + g->tileWidth ? x0 + g->tileWidth : rx;
				}
				dx = (px - rx) * perX;
				dst[0] = dst[1] = density( -(dx * dx + dy * dy), -s2, -m2, -c2 );
				dst += 2;
			}
		}
	}
	return 1;
}
int VK_FdmWriteGaze( uint8_t *dst, size_t capacity, const vkFdmGeometry_t *g, const float tangent[2][4],
					 const float center[2][2], int strength, int eyeTracked ) {
	const uint32_t width = VK_FdmMapWidth( g ), height = VK_FdmMapHeight( g );
	float sharp, coarse, cs, cm, cc;
	uint32_t e, x, y;
	if ( !fdmBegin( dst, capacity, g, strength ) )
		return 0;
	if ( strength <= 0 )
		return 1;
	angles( strength, eyeTracked, &sharp, &coarse );
	cs = cosf( sharp * FOV_PI / 180 );
	cm = cosf( (sharp + coarse) * .5f * FOV_PI / 180 );
	cc = cosf( coarse * FOV_PI / 180 );
	for ( e = 0; e < 2; e++ ) {
		const float sx = tangent[e][1] - tangent[e][0], sy = tangent[e][2] - tangent[e][3];
		const float gx = tangent[e][0] + (center[e][0] + 1) * .5f * sx;
		const float gy = tangent[e][2] - (center[e][1] + 1) * .5f * sy;
		const float glen = gx * gx + gy * gy + 1;
		const float gpx = (center[e][0] + 1) * .5f * g->width, gpy = (center[e][1] + 1) * .5f * g->height;
		for ( y = 0; y < height; y++ ) {
			float py = (y + .5f) * g->texelHeight, ty;
			/* The tiler reads one density a bin, so a texel takes its bin's point nearest the gaze. */
			if ( g->tileHeight ) {
				const float y0 = floorf( py / g->tileHeight ) * g->tileHeight;
				py = gpy < y0 ? y0 : gpy > y0 + g->tileHeight ? y0 + g->tileHeight : gpy;
			}
			ty = tangent[e][2] - py / g->height * sy;
			for ( x = 0; x < width; x++ ) {
				float px = (x + .5f) * g->texelWidth, tx;
				if ( g->tileWidth ) {
					const float x0 = floorf( px / g->tileWidth ) * g->tileWidth;
					px = gpx < x0 ? x0 : gpx > x0 + g->tileWidth ? x0 + g->tileWidth : gpx;
				}
				tx = tangent[e][0] + px / g->width * sx;
				const float dot = tx * gx + ty * gy + 1, len = (tx * tx + ty * ty + 1) * glen;
				dst[0] = dst[1] = dot <= 0 ? 63 : density( dot * dot / len, cs * cs, cm * cm, cc * cc );
				dst += 2;
			}
		}
	}
	return 1;
}
int VK_FdmPlaneBounds( const float m[16], const float in[4], float rect[4] ) {
	int i;
	rect[0] = rect[1] = 1e9f;
	rect[2] = rect[3] = -1e9f;
	for ( i = 0; i < 4; i++ ) {
		const float x = i & 1 ? in[2] : in[0], y = i & 2 ? in[3] : in[1];
		const float cx = m[0] * x + m[4] * y + m[12], cy = m[1] * x + m[5] * y + m[13];
		const float w = m[3] * x + m[7] * y + m[15];
		if ( !(w > .001f) )
			return 0;
		rect[0] = fminf( rect[0], cx / w );
		rect[2] = fmaxf( rect[2], cx / w );
		rect[1] = fminf( rect[1], -cy / w );
		rect[3] = fmaxf( rect[3], -cy / w );
	}
	return VR_FloatsFinite( rect, 4 );
}
/* The point of a cell nearest target along one axis: its bin, or the texel while the bin is unknown. */
static float cellNearest( uint32_t texel, uint32_t texelSize, uint32_t tile, float target ) {
	float lo = (float)texel * texelSize, size = (float)texelSize;
	if ( tile ) {
		lo = floorf( (lo + .5f * texelSize) / tile ) * tile;
		size = (float)tile;
	}
	return target < lo ? lo : target > lo + size ? lo + size : target;
}
int VK_FdmWriteScope( uint8_t *dst, size_t capacity, const vkFdmGeometry_t *g, float radiusX, float radiusY ) {
	const uint32_t width = VK_FdmMapWidth( g ), height = VK_FdmMapHeight( g );
	const float cx = .5f * g->width, cy = .5f * g->height;
	uint32_t e, x, y;
	if ( !fdmBegin( dst, capacity, g, 1 ) )
		return 0;
	if ( !(radiusX > 0) || !(radiusY > 0) ) {
		memset( dst, 0xFF, VK_FdmBytes( g ) );
		return 1;
	}
	for ( e = 0; e < 2; e++ )
		for ( y = 0; y < height; y++ ) {
			const float dy = (cellNearest( y, g->texelHeight, g->tileHeight, cy ) - cy) / radiusY;
			for ( x = 0; x < width; x++ ) {
				const float dx = (cellNearest( x, g->texelWidth, g->tileWidth, cx ) - cx) / radiusX;
				dst[0] = dst[1] = dx * dx + dy * dy <= 1 ? 255 : 63;
				dst += 2;
			}
		}
	return 1;
}
void VK_FdmFullDensityNdc( uint8_t *map, const vkFdmGeometry_t *g, int layer, const float rect[4],
						   const int32_t offset[2] ) {
	const uint32_t width = VK_FdmMapWidth( g ), height = VK_FdmMapHeight( g );
	float x0, x1, y0, y1;
	uint32_t y, tx0, tx1, ty0, ty1;
	if ( !map || !width || !height || !VR_FloatsFinite( rect, 4 ) )
		return;
	/* Only the visible part matters; the device reads it at pixel - offset. */
	x0 = fmaxf( 0, (rect[0] * .5f + .5f) * g->width ) - offset[0];
	x1 = fminf( (float)g->width, (rect[2] * .5f + .5f) * g->width ) - offset[0];
	y0 = fmaxf( 0, (.5f - rect[3] * .5f) * g->height ) - offset[1];
	y1 = fminf( (float)g->height, (.5f - rect[1] * .5f) * g->height ) - offset[1];
	if ( x1 <= x0 || y1 <= y0 )
		return;
	/* The tiler reads one density a bin, at its center, and its bins stay put on the map, so every bin
	 * the rectangle touches goes full. */
	if ( g->tileWidth && g->tileHeight ) {
		x0 = floorf( x0 / g->tileWidth ) * g->tileWidth;
		y0 = floorf( y0 / g->tileHeight ) * g->tileHeight;
		x1 = ceilf( x1 / g->tileWidth ) * g->tileWidth;
		y1 = ceilf( y1 / g->tileHeight ) * g->tileHeight;
	}
	x0 /= g->texelWidth;
	x1 /= g->texelWidth;
	y0 /= g->texelHeight;
	y1 /= g->texelHeight;
	/* Reads past the map's edge clamp to it. */
	tx0 = x0 <= 0 ? 0 : x0 >= width ? width - 1 : (uint32_t)x0;
	ty0 = y0 <= 0 ? 0 : y0 >= height ? height - 1 : (uint32_t)y0;
	tx1 = x1 <= 1 ? 0 : x1 >= width ? width - 1 : (uint32_t)ceilf( x1 ) - 1;
	ty1 = y1 <= 1 ? 0 : y1 >= height ? height - 1 : (uint32_t)ceilf( y1 ) - 1;
	for ( y = ty0; y <= ty1; y++ )
		memset( map + (((size_t)(layer ? 1 : 0) * height + y) * width + tx0) * 2, 0xFF, (size_t)(tx1 - tx0 + 1) * 2 );
}
void VK_FdmOffsets( const vkFdmGeometry_t *g, const float center[2][2], const int32_t ref[2][2],
					uint32_t granularityWidth, uint32_t granularityHeight, int strength, int32_t offset[2][2] ) {
	const int32_t gw = granularityWidth ? (int32_t)granularityWidth : 1;
	const int32_t gh = granularityHeight ? (int32_t)granularityHeight : 1;
	unsigned e;
	for ( e = 0; e < 2; e++ ) {
		float x = (center[e][0] + 1) * .5f * g->width, y = (center[e][1] + 1) * .5f * g->height;
		if ( strength <= 0 || !VR_FloatsFinite( center[e], 2 ) ) {
			offset[e][0] = offset[e][1] = 0;
			continue;
		}
		x = x < 0 ? 0 : x > g->width ? (float)g->width : x;
		y = y < 0 ? 0 : y > g->height ? (float)g->height : y;
		offset[e][0] = (int32_t)floorf( (x - ref[e][0]) / gw + .5f ) * gw;
		offset[e][1] = (int32_t)floorf( (y - ref[e][1]) / gh + .5f ) * gh;
	}
}
int VK_FdmBlockAt( const uint8_t *map, const vkFdmGeometry_t *g, int layer, float x, float y,
				   const int32_t offset[2] ) {
	const uint32_t width = VK_FdmMapWidth( g ), height = VK_FdmMapHeight( g );
	uint32_t tx, ty;
	float d;
	int area, block = 1;
	if ( !map || !width || !height || !VR_FloatFinite( x ) || !VR_FloatFinite( y ) )
		return 1;
	x = (x - offset[0]) / g->texelWidth;
	y = (y - offset[1]) / g->texelHeight;
	tx = x < 0 ? 0 : x >= width ? width - 1 : (uint32_t)x;
	ty = y < 0 ? 0 : y >= height ? height - 1 : (uint32_t)y;
	d = map[(((size_t)(layer ? 1 : 0) * height + ty) * width + tx) * 2] / 255.0f;
	if ( d >= .999f )
		return 1;
	/* The device takes a fragment area no larger than 1/density. */
	area = (int)(1 / (d < 1 / 16.0f ? 1 / 16.0f : d));
	while ( block * 2 <= area && block < 16 )
		block *= 2;
	return block;
}
int VK_FdmBlockAtNdc( const uint8_t *map, const vkFdmGeometry_t *g, int layer, float x, float y,
					  const int32_t offset[2] ) {
	/* map rows run down the image */
	return VK_FdmBlockAt( map, g, layer, (x * .5f + .5f) * g->width, (.5f - y * .5f) * g->height, offset );
}
#define ROUND_UP( v, a ) ( ( ( v ) + ( a ) - 1 ) / ( a ) * ( a ) )
/* Whether the A7xx LRZ fast-clear flag RAM covers a two-layer depth image this size (fdl6 LRZ layout). */
static int lrzCovered( uint32_t width, uint32_t height, uint32_t samples ) {
	uint32_t pitch, rows;
	if ( samples >= 2 )
		height *= 2;
	if ( samples >= 4 )
		width *= 2;
	if ( samples >= 8 )
		height *= 2;
	pitch = ROUND_UP( (width + 7) / 8, 32 );
	rows = ROUND_UP( (height + 7) / 8, 32 );
	return ROUND_UP( (pitch * rows * 2) >> 7, 512 ) / 8 * 2 <= 1024;
}
void VK_FdmTurnipOffsetLimit( uint32_t width, uint32_t height, uint32_t samples, uint32_t *maxWidth,
							  uint32_t *maxHeight ) {
	uint32_t extra;
	*maxWidth = 2016;
	*maxHeight = 2032;
	if ( !lrzCovered( width, height, samples ) )
		return;
	/* Offset depth pads LRZ by the largest tile that keeps fast clears, and tiles are held to it (fdl6_lrz_get_max_fdm_extra_size). */
	for ( extra = 2016; extra > 192; extra -= 4 )
		if ( lrzCovered( width + extra, height + extra, samples ) ) {
			*maxWidth = extra / 16 * 16;
			*maxHeight = extra / 4 * 4;
			return;
		}
}
int VK_FdmTurnipTile( uint32_t width, uint32_t height, const uint32_t *cpp, uint32_t count, uint32_t maxWidth,
					  uint32_t maxHeight, uint32_t *tileWidth, uint32_t *tileHeight ) {
	/* 3 MB GMEM less the A750's VPC attribute buffer and a quarter of its color cache: the only budget that gives
	 * the bins the Frame draws at 1728 (288x576), 2592 and 3456 (384x448); an eighth leaves a 608-tall candidate
	 * at 1728 that trips the aspect penalty. */
	const uint32_t gmem = 3 * 1024 * 1024 - 6 * 0xc000 - (6 * 64 * 1024) / 4;
	const uint32_t alignW = 96, alignH = 32, layers = 2, gmemAlign = 8 * alignW * alignH;
	uint32_t blocks = gmem / gmemAlign, total = 0, pixels = ~0u, best = ~0u, bestW = 0, bestH = 0, i, w;
	if ( !count || !width || !height )
		return 0;
	for ( i = 0; i < count; i++ ) {
		if ( !cpp[i] )
			return 0;
		total += cpp[i];
	}
	for ( i = 0; i < count; i++ ) {
		const uint32_t align = cpp[i] >> 3 ? cpp[i] >> 3 : 1;
		uint32_t n = (blocks * cpp[i] / total) & ~(align - 1);
		if ( n < align )
			n = align;
		blocks -= n;
		total -= cpp[i];
		if ( n * gmemAlign / cpp[i] < pixels )
			pixels = n * gmemAlign / cpp[i];
	}
	/* The fewest bins with no side over twice the other. */
	for ( w = alignW; w <= maxWidth && w <= ROUND_UP( width, alignW ); w += alignW ) {
		uint32_t h = pixels / (w * layers), countW, countH, cost;
		if ( h > maxHeight )
			h = maxHeight;
		if ( h > ROUND_UP( height, alignH ) )
			h = ROUND_UP( height, alignH );
		h = h / alignH * alignH;
		if ( !h )
			continue;
		cost = w > h * 2 || h > w * 2 ? 1000 : 0;
		countW = (width + w - 1) / w;
		countH = (height + h - 1) / h;
		h = ROUND_UP( (height + countH - 1) / countH, alignH );
		cost += countW * countH;
		if ( cost < best ||
			(cost == best && (w > h ? w - h : h - w) < (bestW > bestH ? bestW - bestH : bestH - bestW)) ) {
			best = cost;
			bestW = w;
			bestH = h;
		}
	}
	if ( !bestW )
		return 0;
	*tileWidth = bestW;
	*tileHeight = bestH;
	return 1;
}
