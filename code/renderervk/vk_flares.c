/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_flares.c

#include "tr_local.h"

/*
=============================================================================

LIGHT FLARES

A light flare is an effect that takes place inside the eye when bright light
sources are visible.  The size of the flare relative to the screen is nearly
constant, irrespective of distance, but the intensity should be proportional to the
projected area of the light source.

A surface that has been flagged as having a light flare will calculate the depth
buffer value that its midpoint should have when the surface is added.

After all opaque surfaces have been rendered, the depth buffer is read back for
each flare in view.  If the point has not been obscured by a closer surface, the
flare should be drawn.

Surfaces that have a repeated texture should never be flagged as flaring, because
there will only be a single flare added at the midpoint of the polygon.

To prevent abrupt popping, the intensity of the flare is interpolated up and
down as it changes visibility.  This involves scene to scene state, unlike almost
all other aspects of the renderer, and is complicated by the fact that a single
frame may have multiple scenes.

RB_RenderFlares() will be called once per view (twice in a mirrored scene, potentially
up to five or more times in a frame with 3D status bar icons).

=============================================================================
*/


// flare states maintain visibility over multiple frames for fading
// layers: view, mirror, menu
typedef struct flare_s {
	struct		flare_s	*next;		// for active chain

	int			addedFrame;
	uint32_t	testCount;

	portalView_t portalView;
	int			frameSceneNum;
	stereoFrame_t stereoFrame;
	void		*surface;
	int			fogNum;

	int			fadeTime;

	qboolean	visible;			// state of last test
	float		drawIntensity;		// may be non 0 even if !visible due to fading
	float		deferredIntensity;	// drawIntensity captured at own-view test; later PV_NONE views zero drawIntensity before the deferred draw

	int			windowX, windowY;
	int viewportX, viewportY, viewportHeight;
	int			viewportWidth;		// owning view's viewport width, for corona sizing at deferred draw time
	float		eyeZ;
	float		drawZ;

	vec3_t		origin;
	vec3_t		color;
	qboolean multiview;
	viewParms_t owningView;
	vec3_t billboardLeft, billboardUp;
	qboolean sourceVisible[2], testedSource[2];
	int eyeFadeTime[2];
	float eyeIntensity[2];
	uint64_t generation;
	uint32_t result[2];
} flare_t;

static flare_t	r_flareStructs[ MAX_FLARES ];
static flare_t	*r_activeFlares, *r_inactiveFlares;

typedef struct {
	flare_t *flare;
	uint64_t generation;
	qboolean sourceVisible[2];
} flareProbe_t;
static flareProbe_t r_flareProbes[NUM_COMMAND_BUFFERS][MAX_FLARES];
static uint32_t r_flareProbeCount[NUM_COMMAND_BUFFERS];
static uint64_t r_flareGeneration;

/* Called only after this command slot's fence. Other slots may be in flight:
 * never read or clear their storage, even for a recycled flare. */
void RB_BeginFlareFrame( qboolean completed ) {
	uint32_t slot = vk.cmd_index, i;
	byte *base;
	if ( !vk.storage.buffer_ptr )
		return;
	base = vk.storage.buffer_ptr + slot * MAX_FLARES * vk.storage_alignment;
	if ( completed )
		for ( i = 0; i < r_flareProbeCount[slot]; i++ ) {
			flareProbe_t *probe = &r_flareProbes[slot][i];
			flare_t *f = probe->flare;
			if ( f->generation == probe->generation ) {
				memcpy( f->result, base + i * vk.storage_alignment, sizeof( f->result ) );
				memcpy( f->testedSource, probe->sourceVisible, sizeof( f->testedSource ) );
				f->testCount = 1;
			}
		}
	r_flareProbeCount[slot] = 0;
}

/* Unique within a recording even if a later view recycles the same flare. */
static uint32_t RB_ReserveFlareProbe( flare_t *f ) {
	uint32_t slot = vk.cmd_index, index = r_flareProbeCount[slot];
	flareProbe_t *probe;
	if ( index == MAX_FLARES )
		return UINT32_MAX;
	memset( vk.storage.buffer_ptr + (slot * MAX_FLARES + index) * vk.storage_alignment, 0,
			sizeof( f->result ) );
	r_flareProbeCount[slot]++;
	probe = &r_flareProbes[slot][index];
	probe->flare = f;
	probe->generation = f->generation;
	memcpy( probe->sourceVisible, f->sourceVisible, sizeof( probe->sourceVisible ) );
	return (slot * MAX_FLARES + index) * vk.storage_alignment;
}

/*
==================
R_ClearFlares
==================
*/
void R_ClearFlares( void ) {
	int		i;

	if ( !vk.fragmentStores )
		return;

	Com_Memset( r_flareStructs, 0, sizeof( r_flareStructs ) );
	Com_Memset( r_flareProbeCount, 0, sizeof( r_flareProbeCount ) );
	r_activeFlares = NULL;
	r_inactiveFlares = NULL;

	for ( i = 0 ; i < MAX_FLARES ; i++ ) {
		r_flareStructs[i].next = r_inactiveFlares;
		r_inactiveFlares = &r_flareStructs[i];
	}
}

static float RB_FlareRadius( float distance, float sizeSetting, float projectionScale ) {
	if ( distance < 1 )
		distance = 1;
	return 2 * distance * (sizeSetting / 640.0f + 8 / distance) / projectionScale;
}

static qboolean RB_FlareSourceVisible( const vec4_t clip ) {
	return clip[3] > 0 && fabsf( clip[0] ) < clip[3] && fabsf( clip[1] ) < clip[3] && clip[2] > 0 &&
		   clip[2] < clip[3];
}

/* Clip-space corner bounds retain a halo overlapping either eye's edge. */
static qboolean RB_FlareBoundsVisible( const vec4_t clip, const vec4_t left, const vec4_t up ) {
	int axis;
	if ( clip[3] + fabsf( left[3] ) + fabsf( up[3] ) <= 0 )
		return qfalse;
	for ( axis = 0; axis < 3; axis++ ) {
		float a = clip[axis] - clip[3], da = left[axis] - left[3], db = up[axis] - up[3];
		if ( a - fabsf( da ) - fabsf( db ) > 0 )
			return qfalse;
		a = axis == 2 ? -clip[axis] : -clip[axis] - clip[3];
		da = axis == 2 ? -left[axis] : -left[axis] - left[3];
		db = axis == 2 ? -up[axis] : -up[axis] - up[3];
		if ( a - fabsf( da ) - fabsf( db ) > 0 )
			return qfalse;
	}
	return qtrue;
}

static flare_t *R_SearchFlare( void *surface )
{
	flare_t *f;

	// see if a flare with a matching surface, scene, and view exists
	for ( f = r_activeFlares ; f ; f = f->next ) {
		if ( f->surface == surface && f->stereoFrame == backEnd.refdef.stereoFrame &&
			f->frameSceneNum == backEnd.viewParms.frameSceneNum &&
			f->portalView == backEnd.viewParms.portalView ) {
			return f;
		}
	}

	return NULL;
}


/*
==================
RB_AddFlare

This is called at surface tesselation time
==================
*/
void RB_AddFlare( void *surface, int fogNum, vec3_t point, vec3_t color, vec3_t normal ) {
	int				i;
	flare_t			*f;
	vec3_t			local;
	float			d = 1;
	vec4_t			eye, clip, normalized, window;
	vec3_t worldPoint = {0}, left = {0}, up = {0};
	qboolean sourceVisible[2] = {qfalse, qfalse};

	backEnd.pc.c_flareAdds++;

	if ( normal && (normal[0] || normal[1] || normal[2] ) )	{
		if ( backEnd.viewParms.xrMultiview ) {
			int e, k;
			d = -1;
			for ( e = 0; e < 2; e++ ) {
				vec3_t delta;
				float facing;
				VectorSubtract( backEnd.viewParms.eyeOrigin[e], backEnd.or.origin, delta );
				/* Surface point/normal are model-local, including scaled entities. */
				for ( k = 0; k < 3; k++ ) {
					float lengthSquared = DotProduct( backEnd.or.axis[k], backEnd.or.axis[k] );
					local[k] =
						(lengthSquared > 0 ? DotProduct( delta, backEnd.or.axis[k] ) / lengthSquared : 0) -
						point[k];
				}
				VectorNormalizeFast( local );
				facing = DotProduct( local, normal );
				if ( facing > d )
					d = facing;
			}
		} else {
			VectorSubtract( backEnd.viewParms.or.origin, point, local );
			VectorNormalizeFast( local );
			d = DotProduct( local, normal );
		}
		// If the viewer is behind the flare don't add it.
		if ( d < 0 ) {
			return;
		}
	}

	if ( backEnd.viewParms.xrMultiview ) {
		vec3_t direction, corner;
		float radius, distance, scale;
		qboolean overlaps = qfalse;
		int e, k;
		for ( k = 0; k < 3; k++ )
			worldPoint[k] = backEnd.or.origin[k] + point[0] * backEnd.or
										  .axis[0][k] + point[1] * backEnd.or
										  .axis[1][k] + point[2] * backEnd.or.axis[2][k];
		VectorSubtract( worldPoint, backEnd.viewParms.or.origin, direction );
		distance = VectorLength( direction );
		if ( distance < .001f )
			return;
		VectorScale( direction, 1 / distance, direction );
		CrossProduct( backEnd.viewParms.or.axis[2], direction, left );
		if ( VectorLength( left ) < .001f )
			VectorCopy( backEnd.viewParms.or.axis[1], left );
		VectorNormalizeFast( left );
		CrossProduct( direction, left, up );
		/* Derive angular size from eye optics, never desktop viewport aspect. */
		scale = fabsf( backEnd.viewParms.eyeProjection[0][0] );
		if ( scale < .001f )
			return;
		radius = RB_FlareRadius( distance, r_flareSize->value, scale );
		VectorScale( left, radius, left );
		VectorScale( up, radius, up );
		for ( e = 0; e < 2; e++ ) {
			vec4_t l, u, c;
			R_TransformModelToClip( worldPoint, backEnd.viewParms.world.modelMatrix,
									backEnd.viewParms.eyeProjection[e], eye, c );
			sourceVisible[e] = RB_FlareSourceVisible( c );
			VectorAdd( worldPoint, left, corner );
			R_TransformModelToClip( corner, backEnd.viewParms.world.modelMatrix,
									backEnd.viewParms.eyeProjection[e], eye, l );
			VectorAdd( worldPoint, up, corner );
			R_TransformModelToClip( corner, backEnd.viewParms.world.modelMatrix,
									backEnd.viewParms.eyeProjection[e], eye, u );
			for ( k = 0; k < 4; k++ ) {
				l[k] -= c[k];
				u[k] -= c[k];
			}
			if ( RB_FlareBoundsVisible( c, l, u ) )
				overlaps = qtrue;
		}
		if ( !overlaps )
			return;
		memset( window, 0, sizeof( window ) );
		eye[2] = -distance;
		clip[3] = 1;
		clip[2] = 0;
	} else {
		// if the point is off the screen, don't bother adding it
		// calculate screen coordinates and depth
		R_TransformModelToClip( point, backEnd.or.modelMatrix, backEnd.viewParms.projectionMatrix, eye, clip );

		// check to see if the point is completely off screen
		for ( i = 0; i < 3; i++ ) {
			if ( clip[i] >= clip[3] || clip[i] <= -clip[3] ) {
				return;
			}
		}

		R_TransformClipToWindow( clip, &backEnd.viewParms, normalized, window );

		if ( window[0] < 0 || window[0] >= backEnd.viewParms.viewportWidth || window[1] < 0 ||
			window[1] >= backEnd.viewParms.viewportHeight ) {
			return; // shouldn't happen, since we check the clip[] above, except for FP rounding
		}
	}

	f = R_SearchFlare( surface );

	// allocate a new one
	if ( !f ) {
		if ( !r_inactiveFlares ) {
			// the list is completely full
			return;
		}
		f = r_inactiveFlares;
		r_inactiveFlares = r_inactiveFlares->next;
		f->next = r_activeFlares;
		r_activeFlares = f;

		f->surface = surface;
		f->frameSceneNum = backEnd.viewParms.frameSceneNum;
		f->stereoFrame = backEnd.refdef.stereoFrame;
		f->portalView = backEnd.viewParms.portalView;
		f->visible = qfalse;
		f->fadeTime = backEnd.refdef.time - 2000;
		f->testCount = 0;
		f->generation = ++r_flareGeneration;
		memset( f->result, 0, sizeof( f->result ) );
		for ( i = 0; i < 2; i++ ) {
			f->testedSource[i] = qfalse;
			f->eyeIntensity[i] = 0;
			f->eyeFadeTime[i] = backEnd.refdef.time - 2000;
		}
	}

	f->addedFrame = backEnd.viewParms.frameCount;
	f->fogNum = fogNum;

	VectorCopy( point, f->origin );
	f->multiview = backEnd.viewParms.xrMultiview;
	if ( f->multiview ) {
		f->owningView = backEnd.viewParms;
		VectorCopy( worldPoint, f->origin );
		VectorCopy( left, f->billboardLeft );
		VectorCopy( up, f->billboardUp );
		if ( f->portalView == PV_MIRROR )
			VectorScale( f->billboardLeft, -1, f->billboardLeft );
		memcpy( f->sourceVisible, sourceVisible, sizeof( sourceVisible ) );
	}
	VectorCopy( color, f->color );

	// fade the intensity of the flare down as the
	// light surface turns away from the viewer
	VectorScale( f->color, d, f->color );

	// save info needed to test
	f->windowX = backEnd.viewParms.viewportX + window[0];
	f->windowY = backEnd.viewParms.viewportY + window[1];
	// captured now (own view) so the deferred draw doesn't size off whatever view is last at the 2D boundary
	f->viewportWidth = backEnd.viewParms.viewportWidth;
	f->viewportX = backEnd.viewParms.viewportX;
	f->viewportY = backEnd.viewParms.viewportY;
	f->viewportHeight = backEnd.viewParms.viewportHeight;

	f->eyeZ = eye[2];

#ifdef USE_REVERSED_DEPTH
	f->drawZ = (clip[2]+0.20) / clip[3];
#else
	f->drawZ = (clip[2]-0.20) / clip[3];
#endif

}


/*
==================
RB_AddDlightFlares
==================
*/
void RB_AddDlightFlares( void ) {
	dlight_t		*l;
	int				i, j, k;
	fog_t			*fog = NULL;

	if ( !r_flares->integer ) {
		return;
	}

	l = backEnd.refdef.dlights;

	if ( tr.world )
		fog = tr.world->fogs;

	for ( i = 0 ; i < backEnd.refdef.num_dlights; i++, l++ ) {

		if ( fog )
		{
			// find which fog volume the light is in
			for ( j = 1 ; j < tr.world->numfogs ; j++ ) {
				fog = &tr.world->fogs[j];
				for ( k = 0 ; k < 3 ; k++ ) {
					if ( l->origin[k] < fog->bounds[0][k] || l->origin[k] > fog->bounds[1][k] ) {
						break;
					}
				}
				if ( k == 3 ) {
					break;
				}
			}
			if ( j == tr.world->numfogs ) {
				j = 0;
			}
		}
		else
			j = 0;

		RB_AddFlare( (void *)l, j, l->origin, l->color, NULL );
	}
}

/*
===============================================================================

FLARE BACK END

===============================================================================
*/


static float *vk_ortho( float x1, float x2,
						float y2, float y1,
						float z1, float z2 ) {

	static float m[16] = { 0 };

	m[0] = 2.0f / (x2 - x1);
	m[5] = 2.0f / (y2 - y1);
	m[10] = 1.0f / (z1 - z2);
	m[12] = -(x2 + x1) / (x2 - x1);
	m[13] = -(y2 + y1) / (y2 - y1);
	m[14] = z1 / (z1 - z2);
	m[15] = 1.0f;

	return m;
}


/*
==================
RB_TestFlare
==================
*/
static void RB_TestMultiviewFlare( flare_t *f ) {
	uint32_t offset;
	vec3_t center, direction, left, up;
	static const int corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	float distance = -f->eyeZ, radius, scale;
	int e, i, k;
	backEnd.pc.c_flareTests++;
	for ( e = 0; e < 2; e++ ) {
		qboolean visible = f->testCount && f->testedSource[e] && f->sourceVisible[e] && f->result[e];
		float step = (backEnd.refdef.time - f->eyeFadeTime[e]) * .001f;
		/* An untestable center is occluded, never promoted by the other eye. */
		f->eyeFadeTime[e] = backEnd.refdef.time;
		if ( step < 0 )
			step = 0;
		else if ( step > .25f )
			step = .25f;
		step *= r_flareFade->value;
		f->eyeIntensity[e] += visible ? step : -step;
		if ( f->eyeIntensity[e] < 0 )
			f->eyeIntensity[e] = 0;
		else if ( f->eyeIntensity[e] > 1 )
			f->eyeIntensity[e] = 1;
	}
	f->drawIntensity = f->eyeIntensity[0] > f->eyeIntensity[1] ? f->eyeIntensity[0] : f->eyeIntensity[1];
	offset = RB_ReserveFlareProbe( f );
	if ( offset == UINT32_MAX )
		return;
	/* Tiny world-space triangles avoid the NVIDIA ViewIndex/PointSize hang.
	 * Move the probe toward its captured camera to avoid fixture z fighting. */
	VectorSubtract( f->owningView.or.origin, f->origin, direction );
	VectorNormalizeFast( direction );
	VectorMA( f->origin, .1f, direction, center );
	scale = fabsf( f->owningView.eyeProjection[0][0] );
	radius = 2 * (distance < 1 ? 1 : distance) / (scale * (vk.sceneWidth ? vk.sceneWidth : 1));
	VectorCopy( f->billboardLeft, left );
	VectorCopy( f->billboardUp, up );
	VectorNormalizeFast( left );
	VectorNormalizeFast( up );
	for ( i = 0; i < 6; i++ )
		for ( k = 0; k < 3; k++ )
			tess.xyz[i][k] = center[k] + radius * (corners[i][0] * left[k] + corners[i][1] * up[k]);
	tess.numVertexes = 6;
#ifdef USE_VBO
	tess.vboIndex = 0;
#endif
	backEnd.viewParms = f->owningView;
	vk_update_mvp( f->owningView.world.modelMatrix );
	for ( i = 0; i < VK_DESC_COUNT; i++ )
		vk_reset_descriptor( i );
	vk_bind_pipeline( vk.dot_pipeline );
	vk_bind_geometry( TESS_XYZ );
	vk_draw_dot( offset );
}

static void RB_TestFlare( flare_t *f ) {
	qboolean		visible;
	float			fade;
	float			*m;
	uint32_t		offset;
	int				i;
	if ( f->multiview ) {
		RB_TestMultiviewFlare( f );
		return;
	}

	backEnd.pc.c_flareTests++;

	/*
		We don't have equivalent of glReadPixels() in vulkan
		and explicit depth buffer reading may be very slow and require surface conversion.

		So we will use storage buffer and exploit early depth tests by
		rendering test dot in orthographic projection at projected flare coordinates
		window-x, window-y and world-z: if test dot is not covered by
		any world geometry - it will invoke fragment shader which will
		fill storage buffer at desired location, then we discard fragment.
		When the owning command slot fence completes we read its storage region: a non-zero value means
		our flare showed in that submitted frame;
		multisampled image will cause multiple fragment shader invocations.
	*/

	offset = RB_ReserveFlareProbe( f );

	if ( f->testCount ) {
		if ( f->result[0] )
			visible = qtrue;
		else
			visible = qfalse;

		f->testCount = 1;
	} else {
		visible = qfalse;
	}

	if ( offset != UINT32_MAX ) {
		m = vk_ortho( backEnd.viewParms.viewportX,
					  backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
					  backEnd.viewParms.viewportY,
					  backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight, 0, 1 );
		vk_update_mvp( m );

		tess.xyz[0][0] = f->windowX;
		tess.xyz[0][1] = f->windowY;
		tess.xyz[0][2] = -f->drawZ;
		tess.numVertexes = 1;
		if ( VK_FlareProbeTriangles( vk.multiview, vk.renderPassIndex ) ) {
			static const int corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
			/* The camera is mono, but the array-view dot shader consumes triangles.
			 * One screen pixel per side gives the same footprint as a point probe. */
			for ( i = 0; i < 6; i++ ) {
				tess.xyz[i][0] = f->windowX + 0.5f + corners[i][0] * 0.5f;
				tess.xyz[i][1] = f->windowY + 0.5f + corners[i][1] * 0.5f;
				tess.xyz[i][2] = -f->drawZ;
			}
			tess.numVertexes = 6;
		}

#ifdef USE_VBO
	tess.vboIndex = 0;
#endif
	// invalidate descriptors
	for ( i = 0; i < VK_DESC_COUNT; i++ ) {
		vk_reset_descriptor( i );
	}
	// render test dot
	vk_bind_pipeline( vk.dot_pipeline );
	vk_bind_geometry( TESS_XYZ );
	vk_draw_dot( offset );
	}

	//Com_Memcpy( vk_world.modelview_transform, modelMatrix_original, sizeof( modelMatrix_original ) );
	//vk_update_mvp( NULL );

	if ( visible ) {
		if ( !f->visible ) {
			f->visible = qtrue;
			f->fadeTime = backEnd.refdef.time - 1;
		}
		fade = ( ( backEnd.refdef.time - f->fadeTime ) /1000.0f ) * r_flareFade->value;
	} else {
		if ( f->visible ) {
			f->visible = qfalse;
			f->fadeTime = backEnd.refdef.time - 1;
		}
		fade = 1.0f - ( ( backEnd.refdef.time - f->fadeTime ) / 1000.0f ) * r_flareFade->value;
	}

	if ( fade < 0 ) {
		fade = 0;
	} else if ( fade > 1 ) {
		fade = 1;
	}

	f->drawIntensity = fade;
}


/* Corona color at one fade level; qfalse when falloff or fog leave it black. Fog math uses tess, so never mid-batch. */
static qboolean RB_FlareColor( const flare_t *f, float fade, float *size, color4ub_t *c ) {
	float distance, intensity, factor;
	byte fogFactors[3] = {255, 255, 255};
	int k;

	// We don't want too big values anyways when dividing by distance.
	if ( f->eyeZ > -1.0f )
		distance = 1.0f;
	else
		distance = -f->eyeZ;

	// use the flare's own captured viewport width so a deferred draw isn't mis-sized by the last 3D view's viewParms
	*size = f->viewportWidth * ( r_flareSize->value/640.0f + 8 / distance );

/*
 * This is an alternative to intensity scaling. It changes the size of the flare on screen instead
 * with growing distance. See in the description at the top why this is not the way to go.
	// size will change ~ 1/r.
	size = f->viewportWidth * (r_flareSize->value / (distance * -2.0f));
*/

/*
 * As flare sizes stay nearly constant with increasing distance we must decrease the intensity
 * to achieve a reasonable visual result. The intensity is ~ (size^2 / distance^2) which can be
 * got by considering the ratio of
 * (flaresurface on screen) : (Surface of sphere defined by flare origin and distance from flare)
 * An important requirement is:
 * intensity <= 1 for all distances.
 *
 * The formula used here to compute the intensity is as follows:
 * intensity = flareCoeff * size^2 / (distance + size*sqrt(flareCoeff))^2
 * As you can see, the intensity will have a max. of 1 when the distance is 0.
 * The coefficient flareCoeff will determine the falloff speed with increasing distance.
 */

	factor = distance + *size * sqrt( r_flareCoeff->value );

	intensity = r_flareCoeff->value * *size * *size / ( factor * factor );

	// Calculations for fogging
	if ( tr.world && f->fogNum > 0 && f->fogNum < tr.world->numfogs )
	{
		tess.numVertexes = 1;
		VectorCopy( f->origin, tess.xyz[0] );
		tess.fogNum = f->fogNum;

		RB_CalcModulateColorsByFog( fogFactors );

		// We don't need to render the flare if colors are 0 anyways.
		if ( !(fogFactors[0] || fogFactors[1] || fogFactors[2]) )
			return qfalse;
	}

	for ( k = 0; k < 3; k++ )
		c->rgba[k] = f->color[k] * fade * intensity * fogFactors[k];
	c->rgba[3] = 255;

	// an additive black quad contributes nothing but still pays its fill
	return ( c->rgba[0] | c->rgba[1] | c->rgba[2] ) != 0;
}


/* Draws a multiview corona set in one draw per eye; the opposite layer is clipped so each eye keeps its own fade. */
static void RB_RenderFlareEyes( flare_t **flares, color4ub_t (*colors)[2], int count ) {
	const viewParms_t saved = backEnd.viewParms;
	const orientationr_t savedOrientation = backEnd.or;
	int e, i, fogNum, scene;

	for ( e = 0; e < 2; e++ ) {
		fogNum = -1;
		scene = -1;
		for ( i = 0; i < count; i++ ) {
			const flare_t *f = flares[i];
			if ( !( colors[i][e].rgba[0] | colors[i][e].rgba[1] | colors[i][e].rgba[2] ) )
				continue;
			if ( f->owningView.frameSceneNum != scene ) {
				float *hidden;
				if ( fogNum >= 0 )
					RB_EndSurface();
				backEnd.viewParms = f->owningView;
				backEnd.or = f->owningView.world;
				hidden = backEnd.viewParms.eyeProjection[1 - e];
				memset( hidden, 0, 16 * sizeof( float ) );
				hidden[14] = 2;
				hidden[15] = 1;
				vk_update_mvp( f->owningView.world.modelMatrix );
				vk.cmd->depth_range = DEPTH_RANGE_COUNT;
				RB_BeginSurface( tr.flareShader, f->fogNum );
				fogNum = f->fogNum;
				scene = f->owningView.frameSceneNum;
			} else if ( f->fogNum != fogNum ) {
				RB_EndSurface();
				RB_BeginSurface( tr.flareShader, f->fogNum );
				fogNum = f->fogNum;
			}
			RB_AddQuadStamp( f->origin, f->billboardLeft, f->billboardUp, colors[i][e] );
		}
		if ( fogNum >= 0 )
			RB_EndSurface();
	}
	backEnd.viewParms = saved;
	backEnd.or = savedOrientation;
	vk_update_mvp( saved.world.modelMatrix );
}


/*
==================
RB_RenderFlare
==================
*/
static void RB_RenderFlare( flare_t *f ) {
	float size;
	color4ub_t c;

	backEnd.pc.c_flareRenders++;

	if ( f->multiview ) {
		color4ub_t colors[1][2];
		int e;
		Com_Memset( colors, 0, sizeof( colors ) );
		for ( e = 0; e < 2; e++ )
			if ( f->eyeIntensity[e] > 0 )
				RB_FlareColor( f, f->eyeIntensity[e], &size, &colors[0][e] );
		RB_RenderFlareEyes( &f, colors, 1 );
		return;
	}

	if ( !RB_FlareColor( f, f->drawIntensity, &size, &c ) )
		return;
	RB_BeginSurface( tr.flareShader, f->fogNum );
	RB_AddQuadStamp2( f->windowX - size, f->windowY - size, size * 2, size * 2, 0, 0, 1, 1, c );
	RB_EndSurface();
}


/*
==================
RB_RenderFlares

Because flares are simulating an occular effect, they should be drawn after
everything (all views) in the entire frame has been drawn.

Because of the way portals use the depth buffer to mark off areas, the
needed information would be lost after each view, so we are forced to draw
flares after each view.

The resulting artifact is that flares in mirrors or portals don't dim properly
when occluded by something in the main view, and portal flares that should
extend past the portal edge will be overwritten.
==================
*/
void RB_RenderFlares( void ) {
	flare_t		*f;
	flare_t		**prev;
	qboolean	draw;
	float		*m;

	if ( !r_flares->integer ) {
		return;
	}

	// probes read world depth, which post-scene 2D doesn't have
	if ( vk.renderPassIndex == RENDER_PASS_SCREENMAP || VK_PassIsPostScene2D( vk.renderPassIndex ) ) {
		return;
	}

	if ( backEnd.isHyperspace ) {
		return;
	}

	// Reset currentEntity to world so that any previously referenced entities
	// don't have influence on the rendering of these flares (i.e. RF_ renderer flags).
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.or = backEnd.viewParms.world;

	//RB_AddDlightFlares();

	// perform z buffer readback on each flare in this view
	draw = qfalse;
	prev = &r_activeFlares;
	while ( ( f = *prev ) != NULL ) {
		if ( f->stereoFrame != backEnd.refdef.stereoFrame ) {
			prev = &f->next;
			continue;
		}
		// throw out any flares that weren't added last frame
		if ( backEnd.viewParms.frameCount - f->addedFrame > 0 && f->portalView == backEnd.viewParms.portalView ) {
			*prev = f->next;
			f->next = r_inactiveFlares;
			r_inactiveFlares = f;
			continue;
		}

		// don't draw any here that aren't from this scene / portal
		f->drawIntensity = 0;
		if ( f->frameSceneNum == backEnd.viewParms.frameSceneNum && f->portalView == backEnd.viewParms.portalView ) {
			RB_TestFlare( f );
			// deferred draw runs after later PV_NONE views (3D HUD icons) zero drawIntensity; preserve it here
			f->deferredIntensity = f->drawIntensity;
			if ( f->testCount == 0 ) {
				// Recently added: wait until its first command-slot readback completes.
			} else if ( f->drawIntensity ) {
				draw = qtrue;
			} else {
				// this flare has completely faded out, so remove it from the chain
				*prev = f->next;
				f->next = r_inactiveFlares;
				r_inactiveFlares = f;
				continue;
			}
		}

		prev = &f->next;
	}

	if ( !draw ) {
		return;		// none visible
	}

	// Main-view coronas draw later via RB_RenderDeferredFlares (after bloom's bright-pass, so
	// they aren't re-bloomed); portal/mirror keep classic timing or a deferred draw would leak past the portal edge.
	if ( backEnd.viewParms.portalView == PV_NONE ) {
		return;
	}

#ifdef USE_REVERSED_DEPTH
	m = vk_ortho( backEnd.viewParms.viewportX, backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
		backEnd.viewParms.viewportY, backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight, 1.0, 0.0 );
#else
	m = vk_ortho( backEnd.viewParms.viewportX, backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
		backEnd.viewParms.viewportY, backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight, 0.0, 1.0 );
#endif

	vk_update_mvp( backEnd.viewParms.xrMultiview ? backEnd.viewParms.world.modelMatrix : m );

	for ( f = r_activeFlares ; f ; f = f->next ) {
		if ( f->stereoFrame == backEnd.refdef.stereoFrame &&
			f->frameSceneNum == backEnd.viewParms.frameSceneNum &&
			f->portalView == backEnd.viewParms.portalView && f->drawIntensity ) {
			RB_RenderFlare( f );
		}
	}

	//Com_Memcpy( vk_world.modelview_transform, modelMatrix_original, sizeof( modelMatrix_original ) );
	//vk_update_mvp( NULL );
}


/*
==================
RB_RenderDeferredFlares

Draws main-view (PV_NONE) coronas once per frame at the 3D->2D boundary, after
vk_bloom()'s bright-pass, so coronas aren't re-bloomed. doneFlares guards the once-per-frame.
==================
*/
void RB_RenderDeferredFlares( void ) {
	static flare_t		*batchFlares[MAX_FLARES];
	static color4ub_t	batchColors[MAX_FLARES][2];
	int					batchCount = 0;
	flare_t				*f;
	float				*m;
	const trRefEntity_t	*savedEntity;
	viewParms_t savedView;
	orientationr_t savedOrientation;
	qboolean savedProjection2D;

	if ( !r_flares->integer || backEnd.doneFlares )
		return;

	// Skip pure-2D frames (menu/disconnect): frameCount is frozen on the last 3D
	// frame there, so stale flares would still match and paint coronas over the UI.
	if ( !backEnd.doneSurfaces )
		return;

	// checked before marking done, so a screenmap pass can't suppress the real deferred draw
	if ( vk.renderPassIndex == RENDER_PASS_SCREENMAP )
		return;

	backEnd.doneFlares = qtrue;

	// save/restore currentEntity so the following 2D batch flushes with the entity the caller expects
	savedEntity = backEnd.currentEntity;
	savedView = backEnd.viewParms;
	savedOrientation = backEnd.or ;
	savedProjection2D = backEnd.projection2D;
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.projection2D = qfalse;

	for ( f = r_activeFlares ; f ; f = f->next ) {
		if ( f->portalView == PV_NONE && f->addedFrame == backEnd.viewParms.frameCount ) {
			// restore intensity zeroed by later PV_NONE (3D HUD icon) views since this flare's own test
			f->drawIntensity = f->deferredIntensity;
			if ( f->drawIntensity ) {
				if ( f->multiview ) {
					float size;
					int e;
					backEnd.pc.c_flareRenders++;
					Com_Memset( batchColors[batchCount], 0, sizeof( batchColors[batchCount] ) );
					for ( e = 0; e < 2; e++ )
						if ( f->eyeIntensity[e] > 0 )
							RB_FlareColor( f, f->eyeIntensity[e], &size, &batchColors[batchCount][e] );
					batchFlares[batchCount++] = f;
					continue;
				}
				backEnd.viewParms.viewportX = f->viewportX;
				backEnd.viewParms.viewportY = f->viewportY;
				backEnd.viewParms.viewportWidth = f->viewportWidth;
				backEnd.viewParms.viewportHeight = f->viewportHeight;
				backEnd.viewParms.portalView = PV_NONE;
#ifdef USE_REVERSED_DEPTH
				m = vk_ortho( f->viewportX, f->viewportX + f->viewportWidth, f->viewportY,
							  f->viewportY + f->viewportHeight, 1.0, 0.0 );
#else
				m = vk_ortho( f->viewportX, f->viewportX + f->viewportWidth, f->viewportY,
							  f->viewportY + f->viewportHeight, 0.0, 1.0 );
#endif
				vk_update_mvp( m );
				vk.cmd->depth_range = DEPTH_RANGE_COUNT;
				RB_RenderFlare( f );
			}
		}
	}

	if ( batchCount )
		RB_RenderFlareEyes( batchFlares, batchColors, batchCount );

	// Reached from the 3D boundary and from the end-of-frame fallback.
	backEnd.viewParms = savedView;
	backEnd.or = savedOrientation;
	backEnd.projection2D = savedProjection2D;
	backEnd.currentEntity = savedEntity;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	vk_update_mvp( NULL );
}
