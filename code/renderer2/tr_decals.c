/*
 * tr_decals.c -- enhanced blood decals: radial projection of a decal onto all
 * nearby world surfaces, stored in a ring buffer and drawn via the SF_POLY path.
 */
#include "tr_local.h"

#define DECAL_SURFACE_OFFSET	0.1f	// tiny lift off the surface to limit z-fighting

static decal_t		r_decals[MAX_DECAL_FRAGMENTS];
static polyVert_t	r_decalVerts[MAX_DECAL_FRAGMENTS * MAX_DECAL_VERTS_PER_FRAG];
static int			r_decalHead;		// next fragment slot (ring)

void RE_ClearDecals( void ) {
	int i;

	Com_Memset( r_decals, 0, sizeof( r_decals ) );
	r_decalHead = 0;

	// bind each fragment slot to its fixed vertex window
	for ( i = 0; i < MAX_DECAL_FRAGMENTS; i++ ) {
		r_decals[i].srf.surfaceType = SF_POLY;
		r_decals[i].srf.verts = &r_decalVerts[i * MAX_DECAL_VERTS_PER_FRAG];
	}
}

// fog lookup mirrors RE_AddPolyToScene
static int R_DecalFogIndex( const polyVert_t *verts, int numVerts ) {
	vec3_t	bounds[2];
	int		i, fogIndex;
	const fog_t *fog;

	if ( tr.world == NULL || tr.world->numfogs <= 1 ) {
		return 0;
	}
	VectorCopy( verts[0].xyz, bounds[0] );
	VectorCopy( verts[0].xyz, bounds[1] );
	for ( i = 1; i < numVerts; i++ ) {
		AddPointToBounds( verts[i].xyz, bounds[0], bounds[1] );
	}
	for ( fogIndex = 1; fogIndex < tr.world->numfogs; fogIndex++ ) {
		fog = &tr.world->fogs[fogIndex];
		if ( bounds[1][0] >= fog->bounds[0][0] && bounds[1][1] >= fog->bounds[0][1]
		  && bounds[1][2] >= fog->bounds[0][2] && bounds[0][0] <= fog->bounds[1][0]
		  && bounds[0][1] <= fog->bounds[1][1] && bounds[0][2] <= fog->bounds[1][2] ) {
			return fogIndex;
		}
	}
	return 0;
}

// Build the decal texture frame from a surface normal: axis[0]=normal,
// axis[1]/axis[2] span the surface plane, rotated by orientation.
static void R_DecalAxisFromNormal( const vec3_t normal, float orientation, vec3_t axis[3] ) {
	VectorNormalize2( normal, axis[0] );
	PerpendicularVector( axis[1], axis[0] );
	RotatePointAroundVector( axis[2], axis[0], axis[1], orientation );
	CrossProduct( axis[0], axis[2], axis[1] );
}

// Clip one triangle to the decal's square footprint, on the triangle's own
// plane, and store the result as a ring decal fragment. axis[0] is the
// surface's outward unit normal; tri are the 3 positions.
static void R_DecalClipTriangle( const vec3_t origin, float size, float reach, vec3_t axis[3],
		float texScale, qhandle_t hShader, const byte color[4], int lifeTime, vec3_t tri[3] ) {
	vec3_t		poly[2][MAX_VERTS_ON_POLY];
	vec3_t		neg;
	int			numPoly, ping, k;
	float		d, baseS, baseT;
	decal_t		*dec;
	polyVert_t	*v;

	// front-facing only: the decal center must lie in front of the triangle's
	// plane (small tolerance so blood sitting right on a surface still takes).
	// This stops blood bleeding onto back faces, e.g. the underside of a ledge.
	d = DotProduct( origin, axis[0] ) - DotProduct( tri[0], axis[0] );
	if ( d < -0.1f || d > reach ) {
		return;
	}

	// seed the clip with the triangle, lifted slightly off the surface
	for ( k = 0; k < 3; k++ ) {
		VectorMA( tri[k], DECAL_SURFACE_OFFSET, axis[0], poly[0][k] );
	}
	numPoly = 3;
	ping = 0;

	baseS = DotProduct( origin, axis[1] );
	baseT = DotProduct( origin, axis[2] );

	// clip to the square footprint: |dot(P,axis1) - baseS| <= size (and axis2)
	R_ChopPolyBehindPlane( numPoly, poly[ping], &numPoly, poly[!ping], axis[1], baseS - size, 0.0f );
	ping ^= 1; if ( numPoly < 3 ) return;

	VectorNegate( axis[1], neg );
	R_ChopPolyBehindPlane( numPoly, poly[ping], &numPoly, poly[!ping], neg, -( baseS + size ), 0.0f );
	ping ^= 1; if ( numPoly < 3 ) return;

	R_ChopPolyBehindPlane( numPoly, poly[ping], &numPoly, poly[!ping], axis[2], baseT - size, 0.0f );
	ping ^= 1; if ( numPoly < 3 ) return;

	VectorNegate( axis[2], neg );
	R_ChopPolyBehindPlane( numPoly, poly[ping], &numPoly, poly[!ping], neg, -( baseT + size ), 0.0f );
	ping ^= 1; if ( numPoly < 3 ) return;

	if ( numPoly > MAX_DECAL_VERTS_PER_FRAG ) {
		numPoly = MAX_DECAL_VERTS_PER_FRAG;
	}

	dec = &r_decals[r_decalHead];
	r_decalHead = ( r_decalHead + 1 ) & ( MAX_DECAL_FRAGMENTS - 1 );

	v = dec->srf.verts;	// fixed window bound in RE_ClearDecals
	for ( k = 0; k < numPoly; k++ ) {
		VectorCopy( poly[ping][k], v[k].xyz );
		v[k].st[0] = 0.5f + ( DotProduct( poly[ping][k], axis[1] ) - baseS ) * texScale;
		v[k].st[1] = 0.5f + ( DotProduct( poly[ping][k], axis[2] ) - baseT ) * texScale;
		v[k].modulate.rgba[0] = color[0];
		v[k].modulate.rgba[1] = color[1];
		v[k].modulate.rgba[2] = color[2];
		v[k].modulate.rgba[3] = color[3];
	}

	dec->srf.surfaceType = SF_POLY;
	dec->srf.hShader = hShader;
	dec->srf.numVerts = numPoly;
	dec->srf.fogIndex = R_DecalFogIndex( v, numPoly );
	dec->spawnTime = tr.refdef.time;
	dec->lifeTime = lifeTime;
	dec->baseColor[0] = color[0];
	dec->baseColor[1] = color[1];
	dec->baseColor[2] = color[2];
	dec->baseColor[3] = color[3];
}

// cross-product normal of a triangle; qfalse if degenerate
static qboolean R_DecalTriNormal( vec3_t tri[3], vec3_t normal ) {
	vec3_t v1, v2;
	VectorSubtract( tri[0], tri[1], v1 );
	VectorSubtract( tri[2], tri[1], v2 );
	CrossProduct( v1, v2, normal );
	return ( VectorNormalize( normal ) > 1e-4f ) ? qtrue : qfalse;
}

void RE_ProjectDecal( const vec3_t origin, float size, float reach, float orientation,
		qhandle_t hShader, const float rgba[4], int lifeTime ) {
	vec3_t			mins, maxs;
	surfaceType_t	*surfaces[64];
	vec3_t			zeroDir = { 0, 0, 0 };
	vec3_t			axis[3], tri[3], normal;
	byte			color[4];
	float			texScale;
	int				numSurfaces, i, k, m, n;

	if ( tr.world == NULL || size <= 0 || hShader == 0 ) {
		return;
	}

	color[0] = (byte)( rgba[0] * 255 );
	color[1] = (byte)( rgba[1] * 255 );
	color[2] = (byte)( rgba[2] * 255 );
	color[3] = (byte)( rgba[3] * 255 );
	texScale = 0.5f / size;

	for ( i = 0; i < 3; i++ ) {
		mins[i] = origin[i] - reach;
		maxs[i] = origin[i] + reach;
	}

	tr.viewCount++;
	numSurfaces = 0;
	R_BoxSurfaces_r( tr.world->nodes, mins, maxs, surfaces, 64, &numSurfaces, zeroDir, qtrue );

	for ( i = 0; i < numSurfaces; i++ ) {

		if ( *surfaces[i] == SF_FACE ) {
			srfBspSurface_t	*face = (srfBspSurface_t *)surfaces[i];
			glIndex_t		*indexes = face->indexes;

			// flat face: build the frame from the face's outward normal; back
			// faces (e.g. a ledge underside) are rejected by the front-facing
			// test in R_DecalClipTriangle.
			VectorCopy( face->cullPlane.normal, normal );
			R_DecalAxisFromNormal( normal, orientation, axis );
			for ( k = 0; k < face->numIndexes; k += 3 ) {
				for ( m = 0; m < 3; m++ ) {
					VectorCopy( face->verts[ indexes[k + m] ].xyz, tri[m] );
				}
				R_DecalClipTriangle( origin, size, reach, axis, texScale, hShader, color, lifeTime, tri );
			}
		}
		else if ( *surfaces[i] == SF_GRID ) {
			srfBspSurface_t	*grid = (srfBspSurface_t *)surfaces[i];
			vec3_t	sumNormal = { 0, 0, 0 };
			vec3_t	sharedNormal, cen;
			int		t;

			// pass 1: average the normals of segments near the decal center so
			// the whole patch shares ONE texture frame (segments then connect)
			for ( m = 0; m < grid->height - 1; m++ ) {
				for ( n = 0; n < grid->width - 1; n++ ) {
					srfVert_t *dv = grid->verts + m * grid->width + n;
					for ( t = 0; t < 2; t++ ) {
						if ( t == 0 ) {
							VectorCopy( dv[0].xyz, tri[0] );
							VectorCopy( dv[grid->width].xyz, tri[1] );
							VectorCopy( dv[1].xyz, tri[2] );
						} else {
							VectorCopy( dv[1].xyz, tri[0] );
							VectorCopy( dv[grid->width].xyz, tri[1] );
							VectorCopy( dv[grid->width + 1].xyz, tri[2] );
						}
						if ( !R_DecalTriNormal( tri, normal ) ) {
							continue;
						}
						VectorAdd( tri[0], tri[1], cen );
						VectorAdd( cen, tri[2], cen );
						VectorScale( cen, 1.0f / 3.0f, cen );
						if ( Distance( cen, origin ) > reach ) {
							continue;	// only segments the decal sphere overlaps
						}
						// only front-facing segments contribute (no back-of-curve bleed)
						if ( DotProduct( origin, normal ) - DotProduct( tri[0], normal ) < 0.0f ) {
							continue;
						}
						VectorAdd( sumNormal, normal, sumNormal );
					}
				}
			}
			if ( VectorNormalize2( sumNormal, sharedNormal ) < 1e-3f ) {
				continue;	// no segment near the center (or normals cancel)
			}
			R_DecalAxisFromNormal( sharedNormal, orientation, axis );

			// pass 2: clip every segment with the shared frame
			for ( m = 0; m < grid->height - 1; m++ ) {
				for ( n = 0; n < grid->width - 1; n++ ) {
					srfVert_t *dv = grid->verts + m * grid->width + n;
					for ( t = 0; t < 2; t++ ) {
						if ( t == 0 ) {
							VectorCopy( dv[0].xyz, tri[0] );
							VectorCopy( dv[grid->width].xyz, tri[1] );
							VectorCopy( dv[1].xyz, tri[2] );
						} else {
							VectorCopy( dv[1].xyz, tri[0] );
							VectorCopy( dv[grid->width].xyz, tri[1] );
							VectorCopy( dv[grid->width + 1].xyz, tri[2] );
						}
						// skip back-of-curve segments (facing away from the shared frame)
						if ( !R_DecalTriNormal( tri, normal ) || DotProduct( normal, sharedNormal ) <= 0.0f ) {
							continue;
						}
						R_DecalClipTriangle( origin, size, reach, axis, texScale, hShader, color, lifeTime, tri );
					}
				}
			}
		}
		else if ( *surfaces[i] == SF_TRIANGLES && r_marksOnTriangleMeshes->integer ) {
			srfBspSurface_t	*cts = (srfBspSurface_t *)surfaces[i];
			vec3_t	sumNormal = { 0, 0, 0 };
			vec3_t	sharedNormal, cen;

			// pass 1: shared frame from near-center segment normals
			for ( k = 0; k < cts->numIndexes; k += 3 ) {
				for ( m = 0; m < 3; m++ ) {
					VectorCopy( cts->verts[ cts->indexes[k + m] ].xyz, tri[m] );
				}
				if ( !R_DecalTriNormal( tri, normal ) ) {
					continue;
				}
				VectorAdd( tri[0], tri[1], cen );
				VectorAdd( cen, tri[2], cen );
				VectorScale( cen, 1.0f / 3.0f, cen );
				if ( Distance( cen, origin ) > reach ) {
					continue;
				}
				// only front-facing segments contribute (no back-of-curve bleed)
				if ( DotProduct( origin, normal ) - DotProduct( tri[0], normal ) < 0.0f ) {
					continue;
				}
				VectorAdd( sumNormal, normal, sumNormal );
			}
			if ( VectorNormalize2( sumNormal, sharedNormal ) < 1e-3f ) {
				continue;
			}
			R_DecalAxisFromNormal( sharedNormal, orientation, axis );

			// pass 2: clip every triangle with the shared frame
			for ( k = 0; k < cts->numIndexes; k += 3 ) {
				for ( m = 0; m < 3; m++ ) {
					VectorCopy( cts->verts[ cts->indexes[k + m] ].xyz, tri[m] );
				}
				if ( !R_DecalTriNormal( tri, normal ) || DotProduct( normal, sharedNormal ) <= 0.0f ) {
					continue;	// skip back-facing segments
				}
				R_DecalClipTriangle( origin, size, reach, axis, texScale, hShader, color, lifeTime, tri );
			}
		}
	}
}

void R_AddDecalSurfaces( void ) {
	int			i, j;
	decal_t		*d;
	shader_t	*sh;

	tr.currentEntityNum = REFENTITYNUM_WORLD;
	tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;

	for ( i = 0; i < MAX_DECAL_FRAGMENTS; i++ ) {
		d = &r_decals[i];
		if ( d->srf.numVerts < 3 || d->srf.hShader == 0 ) {
			continue;
		}

		// fade by age (CPU vertex alpha); retire when fully aged
		if ( d->lifeTime > 0 ) {
			int age = tr.refdef.time - d->spawnTime;
			int fade;

			if ( age < 0 || age >= d->lifeTime ) {
				d->srf.numVerts = 0;	// expired (or clock reset) -> free the slot
				continue;
			}
			fade = 255 - ( 255 * age / d->lifeTime );
			for ( j = 0; j < d->srf.numVerts; j++ ) {
				d->srf.verts[j].modulate.rgba[3] = (byte)( ( d->baseColor[3] * fade ) / 255 );
			}
		}

		sh = R_GetShaderByHandle( d->srf.hShader );
		R_AddDrawSurf( (surfaceType_t *)&d->srf, sh, d->srf.fogIndex, 0, 0, 0 );
	}
}
