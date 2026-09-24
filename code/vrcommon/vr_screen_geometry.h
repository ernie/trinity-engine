/* Shared OpenXR-meter-space geometry for screen drawing and controller rays. */
#ifndef VR_SCREEN_GEOMETRY_H
#define VR_SCREEN_GEOMETRY_H
#include <math.h>
#include <string.h>
/* The reference curvature bends a 7.5 m radius through a half-pi arc. Other
 * curvatures scale the radius inversely and hold that arc length, and the
 * surface center stays VR_SCREEN_CENTER_DISTANCE beyond the anchor. */
#define VR_SCREEN_CENTER_DISTANCE 7.5f
#define VR_SCREEN_REFERENCE_CURVATURE 0.5f
#define VR_SCREEN_ARC_LENGTH ( VR_SCREEN_CENTER_DISTANCE * 1.5707963267948966f )
#define VR_SCREEN_HEIGHT 7.875f
#define VR_SCREEN_FLAT_CURVATURE 0.02f
typedef struct {
	int visible, curved;
	float position[3], yaw;
	float radius, height, arc, width;
} vrScreenGeometry_t;
typedef struct {
	int initialized, updating;
	float current[3], target[3], yaw;
} vrScreenAnchor_t;
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
	/* Match the module's 4:3 UI box and optical-center offset before scaling
	 * from logical desktop coordinates into the native-resolution eye atlas. */
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
static inline float VR_ScreenDistance( const float a[3], const float b[3] ) {
	float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return sqrtf( x * x + y * y + z * z );
}
static inline void VR_ScreenUpdate( vrScreenAnchor_t *anchor, vrScreenGeometry_t *screen,
	const float head[3], const float orientation[4], int visible, int follow, float curvature ) {
	float forward[2], front[3], length, distance, offset;
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
	front[0] = head[0] + 3 * forward[0] / length;
	front[1] = head[1];
	front[2] = head[2] + 3 * forward[1] / length;
	if ( !anchor->initialized ) {
		memcpy( anchor->current, front, sizeof( front ) );
		memcpy( anchor->target, front, sizeof( front ) );
		anchor->initialized = anchor->updating = 1;
		anchor->yaw = atan2f( head[0] - front[0], head[2] - front[2] );
	} else if ( follow ) {
		distance = VR_ScreenDistance( anchor->target, front );
		if ( distance < 0.12f ) {
			anchor->updating = 0;
		} else if ( distance > 1.8f || anchor->updating ) {
			memcpy( anchor->target, front, sizeof( front ) );
			anchor->updating = 1;
			if ( distance > 3.6f ) {
				memcpy( anchor->current, front, sizeof( front ) );
			}
		}
		for ( i = 0; i < 3; i++ ) {
			anchor->current[i] += (anchor->target[i] - anchor->current[i]) * 0.01f;
		}
		distance = VR_ScreenDistance( anchor->current, head );
		if ( distance > 0.00001f && fabsf( distance - 3.0f ) > 0.001f ) {
			for ( i = 0; i < 3; i++ ) {
				anchor->current[i] = head[i] + (anchor->current[i] - head[i]) * 3 / distance;
			}
		}
		anchor->yaw = atan2f( head[0] - anchor->current[0], head[2] - anchor->current[2] );
	}
	if ( curvature > 1 ) {
		curvature = 1;
	}
	screen->visible = 1;
	screen->curved = curvature > VR_SCREEN_FLAT_CURVATURE;
	screen->radius = screen->curved ? VR_SCREEN_CENTER_DISTANCE * VR_SCREEN_REFERENCE_CURVATURE / curvature : 0;
	screen->arc = screen->curved ? VR_SCREEN_ARC_LENGTH / screen->radius : 0;
	screen->width = VR_SCREEN_ARC_LENGTH;
	screen->height = VR_SCREEN_HEIGHT;
	memcpy( screen->position, anchor->current, sizeof( screen->position ) );
	screen->position[1] -= 0.5f;
	screen->yaw = anchor->yaw;
	/* The surface center sits radius ahead of the position along the head axis,
	 * so shifting the position by the radius change holds that center in place. */
	offset = screen->radius - VR_SCREEN_CENTER_DISTANCE;
	screen->position[0] += sinf( screen->yaw ) * offset;
	screen->position[2] += cosf( screen->yaw ) * offset;
}
static inline void VR_ScreenPoint( const vrScreenGeometry_t *screen, float u, float v, float out[3] ) {
	float theta = (u - 0.5f) * screen->arc;
	float x = screen->curved ? screen->radius * sinf( theta ) : (u - 0.5f) * screen->width;
	float z = screen->curved ? -screen->radius * cosf( theta ) : 0;
	float c = cosf( screen->yaw ), s = sinf( screen->yaw );
	out[0] = screen->position[0] + c * x + s * z;
	out[1] = screen->position[1] + (0.5f - v) * screen->height;
	out[2] = screen->position[2] - s * x + c * z;
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
		v = 0.5f - y / screen->height;
		if ( u >= 0 && u <= 1 && v >= 0 && v <= 1 ) {
			uv[0] = u;
			uv[1] = v;
			return 1;
		}
	}
	return 0;
}
#endif
