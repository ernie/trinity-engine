/* Virtual screen placement in OpenXR meters; identical in the standalone, trinity-vr and trinity-engine. */
#ifndef VR_SCREEN_GEOMETRY_H
#define VR_SCREEN_GEOMETRY_H
#include <math.h>
#include <string.h>
/* The PC layout at scale 1: a scale shrinks it toward the eye until the bottom edge meets the floor. */
#define VR_SCREEN_ANCHOR_DISTANCE 3.0f
#define VR_SCREEN_CENTER_DISTANCE 7.5f
#define VR_SCREEN_REFERENCE_CURVATURE 0.5f
#define VR_SCREEN_ARC_LENGTH ( VR_SCREEN_CENTER_DISTANCE * 1.5707963267948966f )
#define VR_SCREEN_HEIGHT 7.875f
#define VR_SCREEN_FLAT_CURVATURE 0.02f
/* The bottom edge's depth below the eye at scale 1: the center's 0.5 m drop plus half the height. */
#define VR_SCREEN_EYE_DROP ( 0.5f + VR_SCREEN_HEIGHT * 0.5f )
#define VR_SCREEN_MIN_EYE_HEIGHT 0.5f
#define VR_SCREEN_MAX_EYE_HEIGHT 2.2f
typedef struct {
	int visible, curved;
	float curvature, scale;
	float position[3], yaw;
	float radius, arc, width, height;
} vrScreenGeometry_t;
typedef struct {
	int initialized, updating;
	float scale, current[3], target[3], yaw;
} vrScreenAnchor_t;
static inline float VR_ScreenScale( float eyeHeight ) {
	if ( !( eyeHeight > VR_SCREEN_MIN_EYE_HEIGHT ) )
		eyeHeight = VR_SCREEN_MIN_EYE_HEIGHT;
	else if ( eyeHeight > VR_SCREEN_MAX_EYE_HEIGHT )
		eyeHeight = VR_SCREEN_MAX_EYE_HEIGHT;
	return eyeHeight / VR_SCREEN_EYE_DROP;
}
static inline float VR_ScreenCurvature( float curvature ) {
	if ( !( curvature > VR_SCREEN_FLAT_CURVATURE ) )
		return 0;
	return curvature > 1 ? 1 : curvature;
}
/* Every curvature holds the arc length, so bending changes neither the size nor the center's distance. */
static inline void VR_ScreenShape( vrScreenGeometry_t *screen, float curvature, float scale ) {
	screen->curvature = VR_ScreenCurvature( curvature );
	screen->curved = screen->curvature > 0;
	screen->scale = scale;
	screen->width = VR_SCREEN_ARC_LENGTH * scale;
	screen->height = VR_SCREEN_HEIGHT * scale;
	screen->radius = screen->curved ? VR_SCREEN_CENTER_DISTANCE * scale * VR_SCREEN_REFERENCE_CURVATURE / screen->curvature : 0;
	screen->arc = screen->curved ? screen->width / screen->radius : 0;
}
/* Rectangle in one eye of the atlas, in left/top/right/bottom order. */
static inline void VR_ScreenCaptureRect( int logicalWidth, int logicalHeight,
	int eyeWidth, int eyeHeight, float up, float down, int rect[4] ) {
	float width, height, x, y, tanUp, tanDown, span;
	rect[0] = rect[1] = 0;
	rect[2] = eyeWidth;
	rect[3] = eyeHeight;
	if ( logicalWidth <= 0 || logicalHeight <= 0 || eyeWidth <= 0 || eyeHeight <= 0 )
		return;
	width = (float)logicalWidth;
	height = (logicalWidth * 3) / 4;
	if ( height > logicalHeight ) {
		height = (float)logicalHeight;
		width = (logicalHeight * 4) / 3;
	}
	x = (int)((logicalWidth - width) * 0.5f);
	y = (logicalHeight - height) * 0.5f;
	/* Match the 4:3 UI box and its optical-center offset before scaling into the eye atlas. */
	tanUp = tanf( up );
	tanDown = tanf( down );
	span = tanUp - tanDown;
	if ( fabsf( span ) > 0.001f )
		y += height * 0.5f * (tanUp + tanDown) / span;
	y = (int)y;
	if ( y < 0 )
		y = 0;
	if ( y + height > logicalHeight )
		y = logicalHeight - height;
	rect[0] = (int)floorf( x * eyeWidth / logicalWidth + 0.5f );
	rect[1] = (int)floorf( y * eyeHeight / logicalHeight + 0.5f );
	rect[2] = (int)floorf( (x + width) * eyeWidth / logicalWidth + 0.5f );
	rect[3] = (int)floorf( (y + height) * eyeHeight / logicalHeight + 0.5f );
}
/* Follow measures on the floor plane, so raising or lowering the head never moves the screen. */
static inline float VR_ScreenFloorDistance( const float a[3], const float b[3] ) {
	float x = a[0] - b[0], z = a[2] - b[2];
	return sqrtf( x * x + z * z );
}
/* Eye height is read only when anchoring, so the screen never breathes with the head. */
static inline void VR_ScreenUpdate( vrScreenAnchor_t *anchor, vrScreenGeometry_t *screen,
	const float head[3], const float orientation[4], int visible, int follow, float curvature ) {
	float forward[2], front[3], length, distance, offset, d;
	int i;
	if ( !visible ) {
		memset( anchor, 0, sizeof( *anchor ) );
		memset( screen, 0, sizeof( *screen ) );
		return;
	}
	forward[0] = -2 * (orientation[0] * orientation[2] + orientation[3] * orientation[1]);
	forward[1] = -(1 - 2 * (orientation[0] * orientation[0] + orientation[1] * orientation[1]));
	length = sqrtf( forward[0] * forward[0] + forward[1] * forward[1] );
	if ( length < 0.00001f ) {
		forward[0] = -sinf( anchor->yaw );
		forward[1] = -cosf( anchor->yaw );
		length = 1;
	}
	if ( !anchor->initialized ) {
		anchor->scale = VR_ScreenScale( head[1] );
	}
	d = VR_SCREEN_ANCHOR_DISTANCE * anchor->scale;
	front[0] = head[0] + d * forward[0] / length;
	front[1] = head[1];
	front[2] = head[2] + d * forward[1] / length;
	if ( !anchor->initialized ) {
		memcpy( anchor->current, front, sizeof( front ) );
		memcpy( anchor->target, front, sizeof( front ) );
		anchor->initialized = anchor->updating = 1;
		anchor->yaw = atan2f( head[0] - front[0], head[2] - front[2] );
	} else if ( follow ) {
		distance = VR_ScreenFloorDistance( anchor->target, front );
		if ( distance < 0.04f * d ) {
			anchor->updating = 0;
		} else if ( distance > 0.6f * d || anchor->updating ) {
			memcpy( anchor->target, front, sizeof( front ) );
			anchor->updating = 1;
			if ( distance > 1.2f * d ) {
				memcpy( anchor->current, front, sizeof( front ) );
			}
		}
		for ( i = 0; i < 3; i++ ) {
			anchor->current[i] += (anchor->target[i] - anchor->current[i]) * 0.01f;
		}
		distance = VR_ScreenFloorDistance( anchor->current, head );
		if ( distance > 0.00001f && fabsf( distance - d ) > 0.001f ) {
			anchor->current[0] = head[0] + (anchor->current[0] - head[0]) * d / distance;
			anchor->current[2] = head[2] + (anchor->current[2] - head[2]) * d / distance;
		}
		anchor->yaw = atan2f( head[0] - anchor->current[0], head[2] - anchor->current[2] );
	}
	VR_ScreenShape( screen, curvature, anchor->scale );
	screen->visible = 1;
	screen->position[0] = anchor->current[0];
	screen->position[1] = 0;
	screen->position[2] = anchor->current[2];
	screen->yaw = anchor->yaw;
	/* The surface center sits radius beyond the axis, so shifting by the radius change holds it in place. */
	offset = screen->radius - VR_SCREEN_CENTER_DISTANCE * anchor->scale;
	screen->position[0] += sinf( screen->yaw ) * offset;
	screen->position[2] += cosf( screen->yaw ) * offset;
}
static inline void VR_ScreenLocalPoint( const vrScreenGeometry_t *screen, float u, float v, float out[3] ) {
	float theta = (u - 0.5f) * screen->arc;
	out[0] = screen->curved ? screen->radius * sinf( theta ) : (u - 0.5f) * screen->width;
	out[1] = (1 - v) * screen->height;
	out[2] = screen->curved ? -screen->radius * cosf( theta ) : 0;
}
static inline void VR_ScreenPoint( const vrScreenGeometry_t *screen, float u, float v, float out[3] ) {
	float p[3], c = cosf( screen->yaw ), s = sinf( screen->yaw );
	VR_ScreenLocalPoint( screen, u, v, p );
	out[0] = screen->position[0] + c * p[0] + s * p[2];
	out[1] = screen->position[1] + p[1];
	out[2] = screen->position[2] - s * p[0] + c * p[2];
}
/* World from a mesh built at scale 1; mirror reflects it through the floor. */
static inline void VR_ScreenModelMatrix( const vrScreenGeometry_t *screen, int mirror, float m[16] ) {
	float c = cosf( screen->yaw ), s = sinf( screen->yaw ), k = screen->scale;
	memset( m, 0, 16 * sizeof( float ) );
	m[0] = c * k;
	m[2] = -s * k;
	m[5] = mirror ? -k : k;
	m[8] = s * k;
	m[10] = c * k;
	m[12] = screen->position[0];
	m[13] = screen->position[1];
	m[14] = screen->position[2];
	m[15] = 1;
}
static inline int VR_ScreenRay( const vrScreenGeometry_t *screen, const float origin[3], const float direction[3], float uv[2] ) {
	float c = cosf( screen->yaw ), s = sinf( screen->yaw ), ox = origin[0] - screen->position[0], oz = origin[2] - screen->position[2];
	float o[3] = { c * ox - s * oz, origin[1] - screen->position[1], s * ox + c * oz };
	float d[3] = { c * direction[0] - s * direction[2], direction[1], s * direction[0] + c * direction[2] };
	float roots[2], x, y, z, u, v;
	int n = 1, i;
	if ( !screen->visible ) {
		return 0;
	}
	if ( screen->curved ) {
		float a = d[0] * d[0] + d[2] * d[2], b = 2 * (o[0] * d[0] + o[2] * d[2]);
		float cc = o[0] * o[0] + o[2] * o[2] - screen->radius * screen->radius, disc = b * b - 4 * a * cc;
		if ( a < 0.00000001f || disc < 0 ) {
			return 0;
		}
		roots[0] = (-b - sqrtf( disc )) / (2 * a);
		roots[1] = (-b + sqrtf( disc )) / (2 * a);
		n = 2;
	} else {
		if ( fabsf( d[2] ) < 0.00000001f ) {
			return 0;
		}
		roots[0] = -o[2] / d[2];
	}
	for ( i = 0; i < n; i++ ) {
		if ( roots[i] <= 0 ) {
			continue;
		}
		x = o[0] + roots[i] * d[0];
		y = o[1] + roots[i] * d[1];
		z = o[2] + roots[i] * d[2];
		u = screen->curved ? 0.5f + atan2f( x, -z ) / screen->arc : 0.5f + x / screen->width;
		v = 1 - y / screen->height;
		if ( u >= 0 && u <= 1 && v >= 0 && v <= 1 ) {
			uv[0] = u;
			uv[1] = v;
			return 1;
		}
	}
	return 0;
}
#endif
