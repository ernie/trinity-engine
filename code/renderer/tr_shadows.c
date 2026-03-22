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
#include "tr_local.h"


/*
=================
R_ShadowClipDist

Trace a ray through the clip BSP (which includes detail brushes) to find
the nearest solid surface, then place the shadow back face just past it.
=================
*/
static float R_ShadowClipDist( const vec3_t start, const vec3_t dir, float maxDist ) {
	trace_t	trace;
	vec3_t	end;

	VectorMA( start, maxDist, dir, end );

	ri.CM_PointTrace( &trace, start, end, CONTENTS_SOLID );

	if ( trace.fraction >= 1.0f ) {
		return maxDist;	// no solid hit, use full distance
	}

	// place back face just past the wall surface
	return trace.fraction * maxDist + r_shadowClipPenetration->value;
}


/*

  for a projection shadow:

  point[x] += light vector * ( z - shadow plane )
  point[y] +=
  point[z] = shadow plane

  1 0 light[x] / light[z]

*/

typedef struct {
	int		i2;
	int		facing;
} edgeDef_t;

#define	MAX_EDGE_DEFS	32

static	edgeDef_t	edgeDefs[SHADER_MAX_VERTEXES][MAX_EDGE_DEFS];
static	int			numEdgeDefs[SHADER_MAX_VERTEXES];
static	int			facing[SHADER_MAX_INDEXES/3];
static	int			numLitTris;
static	int			litTriIndexes[SHADER_MAX_INDEXES];
static	float		clipDists[SHADER_MAX_VERTEXES];
static	qboolean	needsTrace[SHADER_MAX_VERTEXES];

static void R_AddEdgeDef( int i1, int i2, int f ) {
	int		c;

	c = numEdgeDefs[ i1 ];
	if ( c == MAX_EDGE_DEFS ) {
		return;		// overflow
	}
	edgeDefs[ i1 ][ c ].i2 = i2;
	edgeDefs[ i1 ][ c ].facing = f;

	numEdgeDefs[ i1 ]++;
}


static void R_CalcShadowEdges( void ) {
	qboolean sil_edge;
	int		i;
	int		c, c2;
	int		j, k;
	int		i2;

	tess.numIndexes = 0;

	// an edge is NOT a silhouette edge if its face doesn't face the light,
	// or if it has a reverse paired edge that also faces the light.
	// A well behaved polyhedron would have exactly two faces for each edge,
	// but lots of models have dangling edges or overfanned edges
	for ( i = 0; i < tess.numVertexes; i++ ) {
		c = numEdgeDefs[ i ];
		for ( j = 0 ; j < c ; j++ ) {
			if ( !edgeDefs[ i ][ j ].facing ) {
				continue;
			}

			sil_edge = qtrue;
			i2 = edgeDefs[ i ][ j ].i2;
			c2 = numEdgeDefs[ i2 ];
			for ( k = 0 ; k < c2 ; k++ ) {
				if ( edgeDefs[ i2 ][ k ].i2 == i && edgeDefs[ i2 ][ k ].facing ) {
					sil_edge = qfalse;
					break;
				}
			}

			// if it doesn't share the edge with another front facing
			// triangle, it is a sil edge
			if ( sil_edge ) {
				if ( tess.numIndexes > ARRAY_LEN( tess.indexes ) - 6 ) {
					i = tess.numVertexes;
					break;
				}
				tess.indexes[ tess.numIndexes + 0 ] = i;
				tess.indexes[ tess.numIndexes + 1 ] = i + tess.numVertexes;
				tess.indexes[ tess.numIndexes + 2 ] = i2;
				tess.indexes[ tess.numIndexes + 3 ] = i2;
				tess.indexes[ tess.numIndexes + 4 ] = i + tess.numVertexes;
				tess.indexes[ tess.numIndexes + 5 ] = i2 + tess.numVertexes;
				tess.numIndexes += 6;
			}
		}
	}
}


/*
=================
RB_ShadowTessEnd

triangleFromEdge[ v1 ][ v2 ]


  set triangle from edge( v1, v2, tri )
  if ( facing[ triangleFromEdge[ v1 ][ v2 ] ] && !facing[ triangleFromEdge[ v2 ][ v1 ] ) {
  }
=================
*/
void RB_ShadowTessEnd( void ) {
	int		i;
	int		numTris;
	vec3_t	lightDir;
	GLboolean rgba[4];

	if ( glConfig.stencilBits < 4 ) {
		return;
	}

#ifdef USE_PMLIGHT
	if ( R_GetDlightMode() == 2 && r_shadows->integer == 2 )
		VectorCopy( backEnd.currentEntity->shadowLightDir, lightDir );
	else
#endif
		VectorCopy( backEnd.currentEntity->lightDir, lightDir );

	// project vertexes away from light direction, clipped to BSP walls
	{
		float	extrusionDist = r_shadowDistance->value;
		vec3_t	worldNegLightDir;

		// decide which triangles face the light (must happen before clip distance
		// computation so we can identify silhouette edge vertices and only trace those)
		Com_Memset( numEdgeDefs, 0, tess.numVertexes * sizeof( numEdgeDefs[0] ) );

		numTris = tess.numIndexes / 3;
		for ( i = 0 ; i < numTris ; i++ ) {
			int		i1, i2, i3;
			vec3_t	d1, d2, normal;
			float	*v1, *v2, *v3;
			float	d;

			i1 = tess.indexes[ i*3 + 0 ];
			i2 = tess.indexes[ i*3 + 1 ];
			i3 = tess.indexes[ i*3 + 2 ];

			v1 = tess.xyz[ i1 ];
			v2 = tess.xyz[ i2 ];
			v3 = tess.xyz[ i3 ];

			VectorSubtract( v2, v1, d1 );
			VectorSubtract( v3, v1, d2 );
			CrossProduct( d1, d2, normal );

			d = DotProduct( normal, lightDir );
			if ( d > 0 ) {
				facing[ i ] = 1;
			} else {
				facing[ i ] = 0;
			}

			// create the edges
			R_AddEdgeDef( i1, i2, facing[ i ] );
			R_AddEdgeDef( i2, i3, facing[ i ] );
			R_AddEdgeDef( i3, i1, facing[ i ] );
		}

		// save lit-facing triangle indices for back cap generation
		numLitTris = 0;
		for ( i = 0; i < numTris; i++ ) {
			if ( facing[i] ) {
				litTriIndexes[ numLitTris*3 + 0 ] = tess.indexes[ i*3 + 0 ];
				litTriIndexes[ numLitTris*3 + 1 ] = tess.indexes[ i*3 + 1 ];
				litTriIndexes[ numLitTris*3 + 2 ] = tess.indexes[ i*3 + 2 ];
				numLitTris++;
			}
		}

		if ( r_shadowClip->integer && tr.world ) {
			int j;
			// entity-local lightDir to world space, negated for extrusion
			for ( j = 0; j < 3; j++ )
				worldNegLightDir[j] = -( lightDir[0] * backEnd.or.axis[0][j]
									   + lightDir[1] * backEnd.or.axis[1][j]
									   + lightDir[2] * backEnd.or.axis[2][j] );

			// mark vertices on lit-facing triangles — back-facing vertices
			// don't contribute to silhouette edges or back caps, so skip them
			Com_Memset( needsTrace, 0, tess.numVertexes * sizeof( needsTrace[0] ) );
			for ( i = 0; i < numTris; i++ ) {
				if ( facing[i] ) {
					needsTrace[ tess.indexes[ i*3 + 0 ] ] = qtrue;
					needsTrace[ tess.indexes[ i*3 + 1 ] ] = qtrue;
					needsTrace[ tess.indexes[ i*3 + 2 ] ] = qtrue;
				}
			}
		}

		// Phase A: compute per-vertex clip distances (skip back-facing vertices)
		for ( i = 0; i < tess.numVertexes; i++ ) {
			clipDists[i] = extrusionDist;

			if ( r_shadowClip->integer && tr.world && needsTrace[i] ) {
				int j;
				vec3_t worldPos;
				float clipped;
				for ( j = 0; j < 3; j++ )
					worldPos[j] = tess.xyz[i][0] * backEnd.or.axis[0][j]
								+ tess.xyz[i][1] * backEnd.or.axis[1][j]
								+ tess.xyz[i][2] * backEnd.or.axis[2][j]
								+ backEnd.or.origin[j];

				clipped = R_ShadowClipDist( worldPos, worldNegLightDir, clipDists[i] );
				if ( clipped < clipDists[i] )
					clipDists[i] = clipped;
			}
		}

		// Phase B: fix corner gaps by extending clip distances across triangles.
		// Capped per-vertex to avoid bleeding through thin walls.
		if ( r_shadowClip->integer && tr.world ) {
			int t, numTrisLocal = tess.numIndexes / 3;
			for ( t = 0; t < numTrisLocal; t++ ) {
				int i1 = tess.indexes[ t*3 + 0 ];
				int i2 = tess.indexes[ t*3 + 1 ];
				int i3 = tess.indexes[ t*3 + 2 ];
				float maxD = clipDists[i1];
				float capD;
				if ( clipDists[i2] > maxD ) maxD = clipDists[i2];
				if ( clipDists[i3] > maxD ) maxD = clipDists[i3];
				// skip if any vertex didn't hit solid
				if ( maxD >= extrusionDist )
					continue;
				// cap: max r_shadowClipExtension units extension per vertex
				capD = maxD;
				if ( capD > clipDists[i1] + r_shadowClipExtension->value ) {
					if ( clipDists[i1] + r_shadowClipExtension->value > clipDists[i1] )
						clipDists[i1] += r_shadowClipExtension->value;
				} else {
					clipDists[i1] = capD;
				}
				if ( capD > clipDists[i2] + r_shadowClipExtension->value ) {
					if ( clipDists[i2] + r_shadowClipExtension->value > clipDists[i2] )
						clipDists[i2] += r_shadowClipExtension->value;
				} else {
					clipDists[i2] = capD;
				}
				if ( capD > clipDists[i3] + r_shadowClipExtension->value ) {
					if ( clipDists[i3] + r_shadowClipExtension->value > clipDists[i3] )
						clipDists[i3] += r_shadowClipExtension->value;
				} else {
					clipDists[i3] = capD;
				}
			}
		}

		// Phase C: extrude vertices using final clip distances
		for ( i = 0; i < tess.numVertexes; i++ ) {
			VectorMA( tess.xyz[i], -clipDists[i], lightDir, tess.xyz[i+tess.numVertexes] );
		}
	}

	R_CalcShadowEdges();

	// back cap: lit-facing triangles at extruded positions, reversed winding
	for ( i = 0; i < numLitTris; i++ ) {
		if ( tess.numIndexes > ARRAY_LEN( tess.indexes ) - 3 )
			break;
		tess.indexes[ tess.numIndexes + 0 ] = litTriIndexes[ i*3 + 0 ] + tess.numVertexes;
		tess.indexes[ tess.numIndexes + 1 ] = litTriIndexes[ i*3 + 2 ] + tess.numVertexes;
		tess.indexes[ tess.numIndexes + 2 ] = litTriIndexes[ i*3 + 1 ] + tess.numVertexes;
		tess.numIndexes += 3;
	}


	GL_ClientState( 1, CLS_NONE );
	GL_ClientState( 0, CLS_NONE );

	qglVertexPointer( 3, GL_FLOAT, sizeof( tess.xyz[0] ), tess.xyz );

	if ( qglLockArraysEXT )
		qglLockArraysEXT( 0, tess.numVertexes*2 );

	// draw the silhouette edges

	qglDisable( GL_TEXTURE_2D );
	//GL_Bind( tr.whiteImage );
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	qglColor4f( 0.2f, 0.2f, 0.2f, 1.0f );

	// don't write to the color buffer
	qglGetBooleanv( GL_COLOR_WRITEMASK, rgba );
	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );

	qglEnable( GL_STENCIL_TEST );
	qglStencilFunc( GL_EQUAL, 0, 0x80 );   // skip entity-marked pixels (bit 7 set)
	qglStencilMask( 0x7F );                 // only write shadow count to bits 0-6

	GL_Cull( CT_BACK_SIDED );
	qglStencilOp( GL_KEEP, GL_KEEP, GL_INCR );

	R_DrawElements( tess.numIndexes, tess.indexes );

	GL_Cull( CT_FRONT_SIDED );
	qglStencilOp( GL_KEEP, GL_KEEP, GL_DECR );

	R_DrawElements( tess.numIndexes, tess.indexes );

	if ( qglUnlockArraysEXT )
		qglUnlockArraysEXT();

	qglStencilMask( 0xFF );
	qglDisable( GL_STENCIL_TEST );

	// re-enable writing to the color buffer
	qglColorMask(rgba[0], rgba[1], rgba[2], rgba[3]);

	qglEnable( GL_TEXTURE_2D );

	backEnd.doneShadows = qtrue;

	tess.numIndexes = 0;
}


/*
=================
RB_ShadowFinish

Darken everything that is is a shadow volume.
We have to delay this until everything has been shadowed,
because otherwise shadows from different body parts would
overlap and double darken.
=================
*/
void RB_ShadowFinish( void ) {

	static const vec3_t verts[4] = {
		{ -100, 100, -10 },
		{  100, 100, -10 },
		{ -100,-100, -10 },
		{  100,-100, -10 }
	};

	if ( !backEnd.doneShadows ) {
		return;
	}

	backEnd.doneShadows = qfalse;

	if ( r_shadows->integer != 2 ) {
		return;
	}
	if ( glConfig.stencilBits < 4 ) {
		return;
	}

	qglEnable( GL_STENCIL_TEST );
	qglStencilFunc( GL_NOTEQUAL, 0, 0x7F );  // check shadow bits 0-6 only

	qglDisable( GL_CLIP_PLANE0 );
	GL_Cull( CT_TWO_SIDED );

	qglDisable( GL_TEXTURE_2D );

	// override projection to avoid portal oblique near plane clipping
	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglOrtho( -100, 100, -100, 100, -100, 100 );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();

	qglColor4f( 0.6f, 0.6f, 0.6f, 1 );

	GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHTEST_DISABLE );

	VBO_UnBind();
	GL_ClientState( 0, CLS_NONE );
	qglVertexPointer( 3, GL_FLOAT, 0, verts );
	qglDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	qglColor4f( 1, 1, 1, 1 );
	qglDisable( GL_STENCIL_TEST );

	// restore projection
	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );

	qglEnable( GL_TEXTURE_2D );
}


/*
=================
RB_ProjectionShadowDeform

=================
*/
void RB_ProjectionShadowDeform( void ) {
	float	*xyz;
	int		i;
	float	h;
	vec3_t	ground;
	vec3_t	light;
	float	groundDist;
	float	d;
	vec3_t	lightDir;

	xyz = ( float * ) tess.xyz;

	ground[0] = backEnd.or.axis[0][2];
	ground[1] = backEnd.or.axis[1][2];
	ground[2] = backEnd.or.axis[2][2];

	groundDist = backEnd.or.origin[2] - backEnd.currentEntity->e.shadowPlane;

#ifdef USE_PMLIGHT
	if ( R_GetDlightMode() == 2 && r_shadows->integer == 2 )
		VectorCopy( backEnd.currentEntity->shadowLightDir, lightDir );
	else
#endif
		VectorCopy( backEnd.currentEntity->lightDir, lightDir );

	d = DotProduct( lightDir, ground );
	// don't let the shadows get too long or go negative
	if ( d < 0.5 ) {
		VectorMA( lightDir, (0.5 - d), ground, lightDir );
		d = DotProduct( lightDir, ground );
	}
	d = 1.0 / d;

	light[0] = lightDir[0] * d;
	light[1] = lightDir[1] * d;
	light[2] = lightDir[2] * d;

	for ( i = 0; i < tess.numVertexes; i++, xyz += 4 ) {
		h = DotProduct( xyz, ground ) + groundDist;

		xyz[0] -= light[0] * h;
		xyz[1] -= light[1] * h;
		xyz[2] -= light[2] * h;
	}
}
