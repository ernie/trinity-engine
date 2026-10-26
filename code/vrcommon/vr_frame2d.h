/* Geometry for cgame's 2D frame in VR. OpenXR frames: x right, y up, -z forward; quaternions x y z w. */
#ifndef VR_FRAME2D_H
#define VR_FRAME2D_H
#include <math.h>
typedef struct {
	float x, y, w, h, s1, t1, s2, t2;
} vr2dQuad;
/* Centered 4:3 box as x, y, w, h. */
static inline void VR2D_Box43( float W, float H, float box[4] ) {
	if ( W * 3 > H * 4 ) {
		box[2] = H * 4 / 3;
		box[3] = H;
	} else {
		box[2] = W;
		box[3] = W * 3 / 4;
	}
	box[0] = ( W - box[2] ) * 0.5f;
	box[1] = ( H - box[3] ) * 0.5f;
}
static inline void VR2D_RotateByQuat( const float q[4], const float v[3], float out[3] ) {
	float tx = 2 * ( q[1] * v[2] - q[2] * v[1] );
	float ty = 2 * ( q[2] * v[0] - q[0] * v[2] );
	float tz = 2 * ( q[0] * v[1] - q[1] * v[0] );
	out[0] = v[0] + q[3] * tx + q[1] * tz - q[2] * ty;
	out[1] = v[1] + q[3] * ty + q[2] * tx - q[0] * tz;
	out[2] = v[2] + q[3] * tz + q[0] * ty - q[1] * tx;
}
/* The center pose's forward axis in the eye's own frame, as tangents (x right, y up). */
static inline void VR2D_EyeCenter( const float eyeQuat[4], const float centerQuat[4], float center[2] ) {
	const float forward[3] = { 0, 0, -1 };
	const float inverse[4] = { -eyeQuat[0], -eyeQuat[1], -eyeQuat[2], eyeQuat[3] };
	float world[3], local[3];
	VR2D_RotateByQuat( centerQuat, forward, world );
	VR2D_RotateByQuat( inverse, world, local );
	center[0] = local[0] / -local[2];
	center[1] = local[1] / -local[2];
}
/* Each side reaches as far from center as the eye that sees farther, so a frame point lies in one direction for both eyes. */
static inline void VR2D_SharedTangents( const float tan[2][4], const float center[2][2], float shared[2][4] ) {
	float reach[4];
	int e, side;
	for ( side = 0; side < 4; side++ ) {
		reach[side] = 0;
		for ( e = 0; e < 2; e++ ) {
			const float d = side & 1 ? tan[e][side] - center[e][side >> 1] : center[e][side >> 1] - tan[e][side];
			if ( d > reach[side] ) {
				reach[side] = d;
			}
		}
	}
	for ( e = 0; e < 2; e++ ) {
		for ( side = 0; side < 4; side++ ) {
			shared[e][side] = side & 1 ? center[e][side >> 1] + reach[side] : center[e][side >> 1] - reach[side];
		}
	}
}
/* Eye tangent = pixel * scale + bias; tan is the eye's l, r, b, t, quadrant bit 0 the right half, bit 1 the bottom half. */
static inline void VR2D_OverlayAffine( const float tan[4], const float center[2], float W, float H, int quadrant, float scale[2], float bias[2] ) {
	if ( quadrant & 1 ) {
		scale[0] = ( tan[1] - center[0] ) * 2 / W;
		bias[0] = center[0] - scale[0] * W * 0.5f;
	} else {
		scale[0] = ( center[0] - tan[0] ) * 2 / W;
		bias[0] = tan[0];
	}
	if ( quadrant & 2 ) {
		scale[1] = ( tan[2] - center[1] ) * 2 / H;
		bias[1] = center[1] - scale[1] * H * 0.5f;
	} else {
		scale[1] = ( center[1] - tan[3] ) * 2 / H;
		bias[1] = tan[3];
	}
}
/* Pieces come out in quadrant order. */
static inline int VR2D_SplitQuad( const vr2dQuad *in, float cx, float cy, vr2dQuad out[4], int quadrant[4] ) {
	vr2dQuad v = *in, *q = &v;
	float xs[3], ss[3], ys[3], ts[3], swap;
	int nx = 1, ny = 1, i, j, n = 0;
	if ( q->w < 0 ) {
		q->x += q->w;
		q->w = -q->w;
		swap = q->s1;
		q->s1 = q->s2;
		q->s2 = swap;
	}
	if ( q->h < 0 ) {
		q->y += q->h;
		q->h = -q->h;
		swap = q->t1;
		q->t1 = q->t2;
		q->t2 = swap;
	}
	xs[0] = q->x;
	ss[0] = q->s1;
	if ( q->x < cx && cx < q->x + q->w ) {
		xs[1] = cx;
		ss[1] = q->s1 + ( q->s2 - q->s1 ) * ( cx - q->x ) / q->w;
		nx = 2;
	}
	xs[nx] = q->x + q->w;
	ss[nx] = q->s2;
	ys[0] = q->y;
	ts[0] = q->t1;
	if ( q->y < cy && cy < q->y + q->h ) {
		ys[1] = cy;
		ts[1] = q->t1 + ( q->t2 - q->t1 ) * ( cy - q->y ) / q->h;
		ny = 2;
	}
	ys[ny] = q->y + q->h;
	ts[ny] = q->t2;
	for ( j = 0; j < ny; j++ ) {
		for ( i = 0; i < nx; i++ ) {
			vr2dQuad *p = &out[n];
			p->x = xs[i];
			p->y = ys[j];
			p->w = xs[i + 1] - xs[i];
			p->h = ys[j + 1] - ys[j];
			p->s1 = ss[i];
			p->s2 = ss[i + 1];
			p->t1 = ts[j];
			p->t2 = ts[j + 1];
			quadrant[n] = ( xs[i] >= cx ? 1 : 0 ) | ( ys[j] >= cy ? 2 : 0 );
			n++;
		}
	}
	return n;
}
/* The frame rect the scope HUD spans, as x y w h: it puts the frame's 4:3 box on a centered 4:3 rect W / 2.25 wide. */
static inline void VR2D_ScopeFrame( float W, float H, float frame[4] ) {
	const float width = W / 2.25f;
	float box[4], k;
	VR2D_Box43( W, H, box );
	k = width / box[2];
	frame[0] = box[0] - ( W - width ) * 0.5f / k;
	frame[1] = box[1] - ( H - width * 0.75f ) * 0.5f / k;
	frame[2] = W / k;
	frame[3] = H / k;
}
static inline float VR2D_HudYOffsetTangent( float yOffset, float hudDepth, float worldscaleScaler ) {
	return ( yOffset / 20 ) / ( ( hudDepth + 1 ) * 9 * worldscaleScaler );
}
#endif
