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
	if ( m->eyeTracked ) {
		sharp = m->strength == 1 ? 20 : m->strength == 2 ? 16 : 12;
		coarse = m->strength == 1 ? 33 : m->strength == 2 ? 27 : 21;
	} else {
		sharp = m->strength == 1 ? 30 : m->strength == 2 ? 25 : 22;
		coarse = m->strength == 1 ? 41 : m->strength == 2 ? 35 : 28;
	}
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
