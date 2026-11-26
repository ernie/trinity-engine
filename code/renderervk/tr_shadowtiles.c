// tr_shadowtiles.c -- cg_shadows 4: soft model shadows from light-space tiles

#include "tr_local.h"

#define MAX_GROUP_ENTITIES	12
#define MAX_TILE_FRAGMENTS	1024
#define GROUP_KEY_EPSILON	0.5f
#define ITEM_BOB_CELL		16.0f
#define RECV_CACHE_ENTRIES	128
#define RECV_CACHE_POINTS	512
#define RECV_CACHE_FRAGMENTS	128
#define RECV_CACHE_KEY		7

typedef struct {
	vec3_t			key, mins, maxs, travel;
	float			darkness, dist;
	int				numEntities;
	trRefEntity_t	*entities[MAX_GROUP_ENTITIES];
	qboolean		cacheable;
	vec3_t			clipTravel;		// travel rounded so the cached receivers serve every direction near it
	int				cacheKey[RECV_CACHE_KEY];	// model, x, y, bob cell, quantized travel
} shadowGroup_t;

// world receivers of upright spinning items, whose tile fits stay the same from frame to frame
typedef struct {
	int					key[RECV_CACHE_KEY];
	float				fit[6];		// center, half extents and reach of the tile the receivers were clipped for
	int					lastFrame;
	int					numFragments, numPoints;
	markFragment_t		fragments[RECV_CACHE_FRAGMENTS];
	const msurface_t	*surfaces[RECV_CACHE_FRAGMENTS];
	vec3_t				points[RECV_CACHE_POINTS];
	float				st[RECV_CACHE_POINTS][2];
} shadowRecvCache_t;

static shadowRecvCache_t	recvCache[RECV_CACHE_ENTRIES];

static shadowGroup_t	groups[MAX_REFENTITIES];
static int				groupOrder[MAX_REFENTITIES];
static int				groupLarge[MAX_REFENTITIES];
static int				groupRects[MAX_REFENTITIES][4];
static vec3_t			fragmentPoints[MAX_SHADOW_RECV_VERTS];
static float			fragmentST[MAX_SHADOW_RECV_VERTS][2];
static markFragment_t	fragments[MAX_TILE_FRAGMENTS];
static const msurface_t	*fragmentSurfaces[MAX_TILE_FRAGMENTS];
static qboolean			fragmentDone[MAX_TILE_FRAGMENTS];
static const shadowRecvRun_t	*drawRun;	// the cutout run being drawn, or NULL
static float			casterMvp[16];
static int				casterChannel;

static qboolean R_ShadowCasterBounds( const trRefEntity_t *ent, const md3Header_t *header, vec3_t mins, vec3_t maxs ) {
	const md3Frame_t	*frames, *a, *b;
	int					i;

	if ( ent->e.frame < 0 || ent->e.frame >= header->numFrames || ent->e.oldframe < 0 || ent->e.oldframe >= header->numFrames ) {
		return qfalse;
	}
	frames = (const md3Frame_t *)( (const byte *)header + header->ofsFrames );
	a = frames + ent->e.frame;
	b = frames + ent->e.oldframe;
	for ( i = 0; i < 8; i++ ) {
		vec3_t local, world;
		int k;
		for ( k = 0; k < 3; k++ ) {
			local[k] = ( i & ( 1 << k ) ) ? MAX( a->bounds[1][k], b->bounds[1][k] ) : MIN( a->bounds[0][k], b->bounds[0][k] );
		}
		for ( k = 0; k < 3; k++ ) {
			world[k] = ent->e.origin[k] + local[0] * ent->e.axis[0][k] + local[1] * ent->e.axis[1][k] + local[2] * ent->e.axis[2][k];
		}
		AddPointToBounds( world, mins, maxs );
	}
	return qtrue;
}

static void R_ShadowGroupEntity( trRefEntity_t *ent, int *numGroups ) {
	const model_t	*model;
	const float		*key, *dir;
	shadowGroup_t	*g;
	vec3_t			mins, maxs, lightDir;
	int				i, j;

	if ( ent->e.reType != RT_MODEL || ( ent->e.renderfx & ( RF_NOSHADOW | RF_DEPTHHACK | RF_FIRST_PERSON ) ) ) {
		return;
	}
	model = R_GetModelByHandle( ent->e.hModel );
	if ( model->type != MOD_MESH || !model->md3[0] ) {
		return;
	}
	ClearBounds( mins, maxs );
	if ( !R_ShadowCasterBounds( ent, model->md3[0], mins, maxs ) ) {
		return;
	}
	R_SetupEntityLighting( &tr.refdef, ent );

	key = ( ent->e.renderfx & RF_LIGHTING_ORIGIN ) ? ent->e.lightingOrigin : ent->e.origin;
	for ( i = 0; i < *numGroups; i++ ) {
		g = &groups[i];
		if ( fabsf( g->key[0] - key[0] ) < GROUP_KEY_EPSILON && fabsf( g->key[1] - key[1] ) < GROUP_KEY_EPSILON
				&& fabsf( g->key[2] - key[2] ) < GROUP_KEY_EPSILON ) {
			break;
		}
	}
	if ( i == *numGroups ) {
		if ( *numGroups == ARRAY_LEN( groups ) ) {
			return;
		}
		g = &groups[( *numGroups )++];
		VectorCopy( key, g->key );
		ClearBounds( g->mins, g->maxs );
		g->numEntities = 0;
		dir = ent->lightDir;
#ifdef USE_PMLIGHT
		// per-pixel dynamic lights stay out of lightDir, so their pull on the shadow comes from shadowLightDir
		if ( r_dlightMode->integer == 2 ) {
			dir = ent->shadowLightDir;
		}
#endif
		for ( j = 0; j < 3; j++ ) {
			lightDir[j] = dir[0] * ent->e.axis[0][j] + dir[1] * ent->e.axis[1][j] + dir[2] * ent->e.axis[2][j];
		}
		VectorNormalize( lightDir );
		VectorNegate( lightDir, g->travel );
		g->darkness = ShadowTile_Darkness( ent->ambientLight, ent->directedLight, SHADOWTILE_STRENGTH );
	}
	if ( g->numEntities == MAX_GROUP_ENTITIES ) {
		return;
	}
	g->entities[g->numEntities++] = ent;
	AddPointToBounds( mins, g->mins, g->maxs );
	AddPointToBounds( maxs, g->mins, g->maxs );
}

static void R_SpinCacheGroup( shadowGroup_t *g ) {
	const trRefEntity_t	*ent = g->entities[0];
	const md3Header_t	*header = R_GetModelByHandle( ent->e.hModel )->md3[0];
	const md3Frame_t	*frames = (const md3Frame_t *)( (const byte *)header + header->ofsFrames );
	const md3Frame_t	*a = frames + ent->e.frame, *b = frames + ent->e.oldframe;
	float				zScale = VectorLength( ent->e.axis[2] );
	vec3_t				localMins, localMaxs;
	int					quantized[3], k;

	g->cacheable = qfalse;
	if ( g->numEntities != 1 || zScale <= 0.0f || fabsf( ent->e.axis[2][2] ) < 0.999f * zScale ) {
		return;
	}
	for ( k = 0; k < 3; k++ ) {
		localMins[k] = MIN( a->bounds[0][k], b->bounds[0][k] );
		localMaxs[k] = MAX( a->bounds[1][k], b->bounds[1][k] );
	}
	ShadowTile_SpinBounds( ent->e.origin, localMins, localMaxs, VectorLength( ent->e.axis[0] ), zScale, ITEM_BOB_CELL,
		g->mins, g->maxs );
	ShadowTile_QuantizeTravel( g->travel, quantized, g->clipTravel );
	g->cacheKey[0] = ent->e.hModel;
	g->cacheKey[1] = (int)floorf( ent->e.origin[0] + 0.5f );
	g->cacheKey[2] = (int)floorf( ent->e.origin[1] + 0.5f );
	g->cacheKey[3] = (int)floorf( ent->e.origin[2] / ITEM_BOB_CELL );
	g->cacheKey[4] = quantized[0];
	g->cacheKey[5] = quantized[1];
	g->cacheKey[6] = quantized[2];
	g->cacheable = qtrue;
}

static void R_AddTileCasters( shadowTileFrame_t *f, shadowTile_t *t, const shadowGroup_t *g ) {
	int i, s;

	t->firstCaster = f->numCasters;
	for ( i = 0; i < g->numEntities; i++ ) {
		trRefEntity_t		*ent = g->entities[i];
		const md3Header_t	*header = R_GetModelByHandle( ent->e.hModel )->md3[0];
		md3Surface_t		*surface = (md3Surface_t *)( (byte *)header + header->ofsSurfaces );

		for ( s = 0; s < header->numSurfaces; s++ ) {
			if ( f->numCasters < MAX_SHADOW_CASTERS && R_MD3SurfaceShader( ent, surface )->sort == SS_OPAQUE ) {
				f->casters[f->numCasters].ent = ent;
				f->casters[f->numCasters].surface = (surfaceType_t *)surface;
				f->numCasters++;
			}
			surface = (md3Surface_t *)( (byte *)surface + surface->ofsEnd );
		}
	}
	t->numCasters = f->numCasters - t->firstCaster;
}

static image_t *R_ReceiverCutout( const msurface_t *surf, int *alphaTest ) {
	const shaderStage_t *stage = surf->shader->stages[0];

	*alphaTest = stage->stateBits & GLS_ATEST_BITS;
	return *alphaTest ? stage->bundle[0].image[0] : NULL;
}

static void R_AddTileFragment( shadowTileFrame_t *f, const shadowTile_t *t, int fragment ) {
	const markFragment_t	*mf = &fragments[fragment];
	int						base = f->numVerts, k;

	if ( mf->numPoints < 3 || f->numVerts + mf->numPoints > MAX_SHADOW_RECV_VERTS
			|| f->numIndexes + ( mf->numPoints - 2 ) * 3 > MAX_SHADOW_RECV_INDEXES ) {
		return;
	}
	for ( k = 0; k < mf->numPoints; k++ ) {
		shadowRecvVert_t *v = &f->verts[f->numVerts++];
		VectorCopy( fragmentPoints[mf->firstPoint + k], v->xyz );
		ShadowTile_Project( &t->fit, t->rect, SHADOWTILE_ATLAS_WIDTH, SHADOWTILE_ATLAS_HEIGHT, v->xyz, v->uvz );
		v->st[0] = fragmentST[mf->firstPoint + k][0];
		v->st[1] = fragmentST[mf->firstPoint + k][1];
		v->fogNum = fragmentSurfaces[fragment]->fogIndex;
	}
	for ( k = 2; k < mf->numPoints; k++ ) {
		f->indexes[f->numIndexes++] = base;
		f->indexes[f->numIndexes++] = base + k - 1;
		f->indexes[f->numIndexes++] = base + k;
	}
}

// receivers past the fade length draw nothing, so the clip ends there
static float R_FitReach( const shadowTileFit_t *fit ) {
	return MIN( SHADOWTILE_DEPTH_RANGE, fit->casterDepth + r_shadowFadeLength->value );
}

static int R_ClipTileFragments( const shadowTileFrame_t *f, const shadowTileFit_t *clip, float reach, const vec3_t points[4],
		qhandle_t bmodel, const trRefEntity_t *mover, int maxPoints ) {
	maxPoints = MIN( maxPoints, MAX_SHADOW_RECV_VERTS - f->numVerts );
	if ( maxPoints <= 0 ) {
		return 0;
	}
	return R_ShadowReceiverFragments( points, clip->axis[0], reach, bmodel,
		mover ? mover->e.origin : NULL, mover ? (const vec3_t *)mover->e.axis : NULL,
		maxPoints, fragmentPoints[0], MAX_TILE_FRAGMENTS, fragments, fragmentSurfaces, fragmentST );
}

static int R_FragmentPointCount( int numFragments ) {
	int i, n = 0;

	for ( i = 0; i < numFragments; i++ ) {
		n = MAX( n, fragments[i].firstPoint + fragments[i].numPoints );
	}
	return n;
}

/*
=================
R_ClearShadowReceiverCache

Entries point into the loaded world's surfaces.
=================
*/
void R_ClearShadowReceiverCache( void ) {
	int i;

	for ( i = 0; i < RECV_CACHE_ENTRIES; i++ ) {
		recvCache[i].lastFrame = -1;
	}
}

static shadowRecvCache_t *R_RecvCacheFind( const int key[RECV_CACHE_KEY] ) {
	int i;

	for ( i = 0; i < RECV_CACHE_ENTRIES; i++ ) {
		if ( recvCache[i].lastFrame >= 0 && !memcmp( recvCache[i].key, key, sizeof( recvCache[i].key ) ) ) {
			recvCache[i].lastFrame = tr.frameCount;
			return &recvCache[i];
		}
	}
	return NULL;
}

// an item growing in as it respawns keeps its key while its tile shrinks or grows
static qboolean R_RecvCacheFits( const shadowRecvCache_t *c, const shadowTileFit_t *clip, float reach ) {
	const float now[6] = { clip->center[0], clip->center[1], clip->center[2], clip->halfW, clip->halfH, reach };
	int			k;

	for ( k = 0; k < 6; k++ ) {
		if ( fabsf( c->fit[k] - now[k] ) > 0.01f ) {
			return qfalse;
		}
	}
	return qtrue;
}

// a clip that came close to its point budget may have dropped polygons, so only roomy ones are kept
static void R_RecvCacheStore( shadowRecvCache_t *c, const int key[RECV_CACHE_KEY], const shadowTileFit_t *clip, float reach,
		int numFragments, int maxPoints ) {
	int numPoints = R_FragmentPointCount( numFragments ), i;

	if ( numFragments > RECV_CACHE_FRAGMENTS || numPoints > maxPoints - MAX_VERTS_ON_POLY ) {
		return;
	}
	if ( !c ) {
		c = &recvCache[0];
		for ( i = 1; i < RECV_CACHE_ENTRIES && c->lastFrame >= 0; i++ ) {
			if ( recvCache[i].lastFrame < c->lastFrame ) {
				c = &recvCache[i];
			}
		}
	}
	Com_Memcpy( c->key, key, sizeof( c->key ) );
	c->fit[0] = clip->center[0];
	c->fit[1] = clip->center[1];
	c->fit[2] = clip->center[2];
	c->fit[3] = clip->halfW;
	c->fit[4] = clip->halfH;
	c->fit[5] = reach;
	c->lastFrame = tr.frameCount;
	c->numFragments = numFragments;
	c->numPoints = numPoints;
	Com_Memcpy( c->fragments, fragments, numFragments * sizeof( fragments[0] ) );
	Com_Memcpy( c->surfaces, fragmentSurfaces, numFragments * sizeof( fragmentSurfaces[0] ) );
	Com_Memcpy( c->points, fragmentPoints, numPoints * sizeof( fragmentPoints[0] ) );
	Com_Memcpy( c->st, fragmentST, numPoints * sizeof( fragmentST[0] ) );
}

static int R_RecvCacheLoad( const shadowRecvCache_t *c ) {
	Com_Memcpy( fragments, c->fragments, c->numFragments * sizeof( fragments[0] ) );
	Com_Memcpy( fragmentSurfaces, c->surfaces, c->numFragments * sizeof( fragmentSurfaces[0] ) );
	Com_Memcpy( fragmentPoints, c->points, c->numPoints * sizeof( fragmentPoints[0] ) );
	Com_Memcpy( fragmentST, c->st, c->numPoints * sizeof( fragmentST[0] ) );
	return c->numFragments;
}

static void R_AddTileFragments( shadowTileFrame_t *f, const shadowTile_t *t, int numFragments ) {
	int i, j;

	// one run per receiver texture that cuts holes, drawn with that texture bound
	Com_Memset( fragmentDone, 0, numFragments * sizeof( fragmentDone[0] ) );
	for ( i = 0; i < numFragments && f->numRuns < MAX_SHADOW_RECV_RUNS; i++ ) {
		shadowRecvRun_t	*run = &f->runs[f->numRuns];
		int				alphaTest;

		if ( fragmentDone[i] ) {
			continue;
		}
		run->cutout = R_ReceiverCutout( fragmentSurfaces[i], &run->alphaTest );
		run->firstIndex = f->numIndexes;
		for ( j = i; j < numFragments; j++ ) {
			if ( !fragmentDone[j] && R_ReceiverCutout( fragmentSurfaces[j], &alphaTest ) == run->cutout && alphaTest == run->alphaTest ) {
				fragmentDone[j] = qtrue;
				R_AddTileFragment( f, t, j );
			}
		}
		run->numIndexes = f->numIndexes - run->firstIndex;
		if ( run->numIndexes ) {
			f->numRuns++;
		}
	}
}

static void R_AddTileReceivers( shadowTileFrame_t *f, shadowTile_t *t, const shadowGroup_t *g ) {
	shadowRecvCache_t	*cached = g->cacheable ? R_RecvCacheFind( g->cacheKey ) : NULL;
	shadowTileFit_t		clip = t->fit;
	vec3_t				points[4], prismMins, prismMaxs, far;
	float				reach = R_FitReach( &t->fit ), pad;
	int					i, numFragments;

	if ( g->cacheable ) {
		ShadowTile_Fit( g->mins, g->maxs, g->clipTravel, SHADOWTILE_DEPTH_RANGE, &clip );
		pad = ShadowTile_QuantizePad( R_FitReach( &clip ), clip.halfW );
		reach = R_FitReach( &clip ) + pad;
		clip.halfW += pad;
		clip.halfH += pad;
	}
	ShadowTile_Footprint( &clip, points );
	ClearBounds( prismMins, prismMaxs );
	for ( i = 0; i < 4; i++ ) {
		AddPointToBounds( points[i], prismMins, prismMaxs );
		VectorMA( points[i], reach, clip.axis[0], far );
		AddPointToBounds( far, prismMins, prismMaxs );
	}

	t->firstVert = f->numVerts;
	t->firstIndex = f->numIndexes;
	t->firstRun = f->numRuns;
	if ( cached && R_RecvCacheFits( cached, &clip, reach ) ) {
		numFragments = R_RecvCacheLoad( cached );
	} else if ( g->cacheable ) {
		numFragments = R_ClipTileFragments( f, &clip, reach, (const vec3_t *)points, 0, NULL, RECV_CACHE_POINTS );
		R_RecvCacheStore( cached, g->cacheKey, &clip, reach, numFragments, MIN( RECV_CACHE_POINTS, MAX_SHADOW_RECV_VERTS - f->numVerts ) );
	} else {
		numFragments = R_ClipTileFragments( f, &clip, reach, (const vec3_t *)points, 0, NULL, MAX_SHADOW_RECV_VERTS );
	}
	R_AddTileFragments( f, t, numFragments );

	for ( i = 0; i < tr.refdef.num_entities; i++ ) {
		const trRefEntity_t	*ent = &tr.refdef.entities[i];
		const model_t		*model;
		vec3_t				mins, maxs, corner;
		int					c, k;

		if ( ent->e.reType != RT_MODEL ) {
			continue;
		}
		model = R_GetModelByHandle( ent->e.hModel );
		if ( model->type != MOD_BRUSH || !model->bmodel ) {
			continue;
		}
		ClearBounds( mins, maxs );
		for ( c = 0; c < 8; c++ ) {
			for ( k = 0; k < 3; k++ ) {
				corner[k] = ent->e.origin[k];
			}
			for ( k = 0; k < 3; k++ ) {
				float d = model->bmodel->bounds[( c >> k ) & 1][k];
				VectorMA( corner, d, ent->e.axis[k], corner );
			}
			AddPointToBounds( corner, mins, maxs );
		}
		if ( BoundsIntersect( mins, maxs, prismMins, prismMaxs ) ) {
			R_AddTileFragments( f, t, R_ClipTileFragments( f, &clip, reach, (const vec3_t *)points, ent->e.hModel, ent, MAX_SHADOW_RECV_VERTS ) );
		}
	}
	t->numVerts = f->numVerts - t->firstVert;
	t->numIndexes = f->numIndexes - t->firstIndex;
	t->numRuns = f->numRuns - t->firstRun;
}

static qboolean R_ShadowGroupInView( const shadowGroup_t *g, const float (*planes)[4] ) {
	shadowTileFit_t fit;

	return ShadowTile_Fit( g->mins, g->maxs, g->travel, SHADOWTILE_DEPTH_RANGE, &fit )
		&& !ShadowTile_SweptBoxCulled( g->mins, g->maxs, g->travel, R_FitReach( &fit ), planes, 4 );
}

/*
=================
R_ShadowTiles_BuildScene
=================
*/
void R_ShadowTiles_BuildScene( const viewParms_t *parms ) {
	shadowTileFrame_t		*f;
	viewParms_t				view;
	float					planes[4][4];
	int						numGroups = 0, i, j;

	if ( r_shadows->integer != 4 || ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) || !tr.world ) {
		return;
	}
	if ( !backEndData->shadowTiles ) {
		backEndData->shadowTiles = ri.Malloc( sizeof( *backEndData->shadowTiles ) );
		backEndData->shadowTiles->frame = -1;
	}
	f = backEndData->shadowTiles;
	if ( f->frame == tr.frameCount ) {
		return;
	}
	f->frame = tr.frameCount;
	f->numTiles = f->numCasters = f->numVerts = f->numIndexes = f->numRuns = 0;

	view = *parms;
	R_SetupProjection( &view, r_zproj->value, qtrue );
	for ( i = 0; i < 4; i++ ) {
		VectorCopy( view.frustum[i].normal, planes[i] );
		planes[i][3] = view.frustum[i].dist;
	}

	for ( i = 0; i < tr.refdef.num_entities; i++ ) {
		R_ShadowGroupEntity( &tr.refdef.entities[i], &numGroups );
	}

	for ( i = 0, j = 0; i < numGroups; i++ ) {
		vec3_t center;
		VectorAdd( groups[i].mins, groups[i].maxs, center );
		VectorScale( center, 0.5f, center );
		groups[i].dist = Distance( center, tr.refdef.vieworg );
		if ( groups[i].dist <= SHADOWTILE_VIEW_DISTANCE && groups[i].darkness > 0.0f
				&& R_ShadowGroupInView( &groups[i], (const float (*)[4])planes ) ) {
			groupOrder[j++] = i;
		}
	}
	numGroups = j;
	for ( i = 1; i < numGroups; i++ ) {
		int o = groupOrder[i];
		for ( j = i; j > 0 && groups[groupOrder[j - 1]].dist > groups[o].dist; j-- ) {
			groupOrder[j] = groupOrder[j - 1];
		}
		groupOrder[j] = o;
	}
	for ( i = 0; i < numGroups; i++ ) {
		shadowGroup_t *g = &groups[groupOrder[i]];
		groupLarge[i] = MAX( MAX( g->maxs[0] - g->mins[0], g->maxs[1] - g->mins[1] ), g->maxs[2] - g->mins[2] ) >= SHADOWTILE_LARGE_EXTENT;
		g->cacheable = qfalse;
		if ( !groupLarge[i] ) {
			R_SpinCacheGroup( g );
		}
	}
	ShadowTile_Allocate( groupLarge, numGroups, SHADOWTILE_ATLAS_WIDTH, SHADOWTILE_ATLAS_HEIGHT, SHADOWTILE_LARGE, SHADOWTILE_SMALL,
		SHADOWTILE_MAX_LARGE, groupRects );

	for ( i = 0; i < numGroups && f->numTiles < MAX_SHADOW_TILES; i++ ) {
		const shadowGroup_t	*g = &groups[groupOrder[i]];
		shadowTile_t		*t;

		if ( !groupRects[i][2] ) {
			continue;
		}
		t = &f->tiles[f->numTiles];
		if ( g->cacheable ) {
			// its side axes follow the clip's, so the padded receivers cover it however the travel rounds
			shadowTileFit_t clip;
			ShadowTile_Fit( g->mins, g->maxs, g->clipTravel, SHADOWTILE_DEPTH_RANGE, &clip );
			if ( !ShadowTile_FitAligned( g->mins, g->maxs, g->travel, clip.axis[1], SHADOWTILE_DEPTH_RANGE, &t->fit ) ) {
				continue;
			}
		} else if ( !ShadowTile_Fit( g->mins, g->maxs, g->travel, SHADOWTILE_DEPTH_RANGE, &t->fit ) ) {
			continue;
		}
		Com_Memcpy( t->rect, groupRects[i], sizeof( t->rect ) );
		t->large = groupRects[i][2] == SHADOWTILE_LARGE ? qtrue : qfalse;
		t->darkness = g->darkness;
		t->uvPerUnit = ShadowTile_UvPerUnit( &t->fit, t->rect, SHADOWTILE_ATLAS_WIDTH );
		R_AddTileCasters( f, t, g );
		R_AddTileReceivers( f, t, g );
		f->numTiles++;
	}
}

/*
=================
R_AddShadowReceiverSurfaces
=================
*/
void R_AddShadowReceiverSurfaces( void ) {
	shadowTileFrame_t	*f = backEndData->shadowTiles;
	int					i;

	if ( r_shadows->integer != 4 || !f || f->frame != tr.frameCount || ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		return;
	}
	tr.currentEntityNum = REFENTITYNUM_WORLD;
	tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;
	for ( i = 0; i < f->numTiles; i++ ) {
		if ( !f->tiles[i].numIndexes ) {
			continue;
		}
		f->surfaces[i].surfaceType = SF_SHADOW_RECEIVER;
		f->surfaces[i].tile = i;
		R_AddDrawSurf( (void *)&f->surfaces[i], tr.shadowTileShader, 0, 0 );
	}
}

static void RB_SetCutoutVertex( const shadowRecvVert_t *v, int n ) {
	if ( drawRun ) {
		tess.normal[n][0] = v->st[0];
		tess.normal[n][1] = v->st[1];
		tess.normal[n][2] = (float)( drawRun->alphaTest >> 12 );
		tess.normal[n][3] = 0.0f;
	}
}

static void RB_DrawTileReceiverDepth( const shadowTileFrame_t *f, const shadowTile_t *t ) {
	int r, i, k;

	casterChannel = 1;
	ShadowTile_WorldMatrix( &t->fit, casterMvp );
	backEnd.currentEntity = &tr.worldEntity;
	for ( r = t->firstRun; r < t->firstRun + t->numRuns; r++ ) {
		const shadowRecvRun_t *run = &f->runs[r];

		drawRun = run->cutout ? run : NULL;
		RB_BeginSurface( tr.shadowTileShader, 0 );
		for ( i = 0; i < run->numIndexes; i += 3 ) {
			RB_CHECKOVERFLOW( 3, 3 );
			for ( k = 0; k < 3; k++ ) {
				const shadowRecvVert_t *v = &f->verts[f->indexes[run->firstIndex + i + k]];

				VectorCopy( v->xyz, tess.xyz[tess.numVertexes] );
				RB_SetCutoutVertex( v, tess.numVertexes );
				tess.indexes[tess.numIndexes++] = tess.numVertexes++;
			}
		}
		RB_EndSurface();
	}
	drawRun = NULL;
}

/*
=================
RB_ShadowTiles
=================
*/
void RB_ShadowTiles( void ) {
	const shadowTileFrame_t	*f = backEndData->shadowTiles;
	const trRefEntity_t		*open;
	int						i, c;

	if ( !f || f->frame != tr.frameCount || !f->numTiles ) {
		return;
	}

	vk_create_shadow_atlas();
	vk_begin_shadowtile_render_pass();
	backEnd.shadowTilePass = qtrue;
	for ( i = 0; i < f->numTiles; i++ ) {
		const shadowTile_t *t = &f->tiles[i];

		vk_set_tile_viewport( t->rect );
		casterChannel = 0;
		// a model's surfaces share its transform, so each model goes out as one draw
		open = NULL;
		for ( c = t->firstCaster; c < t->firstCaster + t->numCasters; c++ ) {
			const shadowCaster_t *caster = &f->casters[c];

			if ( caster->ent != open ) {
				if ( open ) {
					RB_EndSurface();
				}
				open = caster->ent;
				backEnd.currentEntity = caster->ent;
				ShadowTile_CasterMatrix( &t->fit, caster->ent->e.origin, (const float (*)[3])caster->ent->e.axis, casterMvp );
				RB_BeginSurface( tr.shadowTileShader, 0 );
			}
			RB_SurfaceMeshPositions( (const md3Surface_t *)caster->surface );
		}
		if ( open ) {
			RB_EndSurface();
		}
		RB_DrawTileReceiverDepth( f, t );
	}
	backEnd.shadowTilePass = qfalse;

	vk_end_shadowtile_render_pass();
	backEnd.shadowTilesReady = qtrue;
}

/*
=================
RB_ShadowTileCasterEnd
=================
*/
void RB_ShadowTileCasterEnd( void ) {
	if ( drawRun ) {
		vk_update_descriptor( VK_DESC_TEXTURE0, drawRun->cutout->descriptor );
		vk_bind_pipeline( vk.shadowtile_cut_depth_pipeline );
	} else {
		vk_bind_pipeline( vk.shadowtile_cast_pipeline[casterChannel] );
	}
	vk_update_mvp( casterMvp );
	vk_bind_index();
	vk_bind_geometry( drawRun ? TESS_XYZ | TESS_NNN : TESS_XYZ );
	vk_draw_geometry( DEPTH_RANGE_NORMAL, qtrue );
	tess.numIndexes = tess.numVertexes = 0;
}

// fog hides a shadow as much as it hides the surface under it
static byte RB_ReceiverDarkness( const shadowTile_t *t, const shadowRecvVert_t *v ) {
	float keep = 1.0f, st[2];

	if ( v->fogNum ) {
		RB_CalcFogTexCoordsFor( v->fogNum, v->xyz, 3, 1, st );
		keep = 1.0f - R_FogFactor( st[0], st[1] );
	}
	return (byte)( t->darkness * keep * 255.0f + 0.5f );
}

/*
=================
RB_SurfaceShadowReceiver
=================
*/
void RB_SurfaceShadowReceiver( srfShadowReceiver_t *surf ) {
	const shadowTileFrame_t	*f = backEndData->shadowTiles;
	const shadowTile_t		*t = &f->tiles[surf->tile];
	const float				minU = (float)t->rect[0] / SHADOWTILE_ATLAS_WIDTH, minV = (float)t->rect[1] / SHADOWTILE_ATLAS_HEIGHT;
	const byte				fadeLength = (byte)( r_shadowFadeLength->value / SHADOWTILE_DEPTH_RANGE * 255.0f + 0.5f );
	int						r, i, k;

#ifdef USE_VBO
	VBO_Flush();
#endif

	for ( r = t->firstRun; r < t->firstRun + t->numRuns; r++ ) {
		const shadowRecvRun_t *run = &f->runs[r];

		if ( run->cutout ) {
			if ( tess.numIndexes ) {
				RB_EndSurface();
				RB_BeginSurface( tr.shadowTileShader, 0 );
			}
			drawRun = run;
		}
		for ( i = 0; i < run->numIndexes; i += 3 ) {
			RB_CHECKOVERFLOW( 3, 3 );
			for ( k = 0; k < 3; k++ ) {
				const shadowRecvVert_t	*v = &f->verts[f->indexes[run->firstIndex + i + k]];
				const int				n = tess.numVertexes;

				VectorCopy( v->xyz, tess.xyz[n] );
				tess.svars.texcoords[0][n][0] = v->uvz[0];
				tess.svars.texcoords[0][n][1] = v->uvz[1];
				tess.svars.texcoords[1][n][0] = v->uvz[2];
				tess.svars.texcoords[1][n][1] = t->uvPerUnit;
				tess.svars.texcoords[2][n][0] = minU;
				tess.svars.texcoords[2][n][1] = minV;
				tess.svars.colors[0][n].rgba[0] = RB_ReceiverDarkness( t, v );
				tess.svars.colors[0][n].rgba[1] = t->large ? 255 : 0;
				tess.svars.colors[0][n].rgba[2] = fadeLength;
				tess.svars.colors[0][n].rgba[3] = 255;
				RB_SetCutoutVertex( v, n );
				tess.indexes[tess.numIndexes++] = n;
				tess.numVertexes++;
			}
		}
		if ( run->cutout ) {
			RB_EndSurface();
			RB_BeginSurface( tr.shadowTileShader, 0 );
			drawRun = NULL;
		}
	}
}

/*
=================
RB_ShadowTileReceiveEnd
=================
*/
void RB_ShadowTileReceiveEnd( void ) {
	if ( !backEnd.shadowTilesReady ) {
		tess.numIndexes = tess.numVertexes = 0;
		return;
	}
	tess.svars.texcoordPtr[0] = tess.svars.texcoords[0];
	tess.svars.texcoordPtr[1] = tess.svars.texcoords[1];
	tess.svars.texcoordPtr[2] = tess.svars.texcoords[2];
	vk_update_descriptor( VK_DESC_TEXTURE0, vk.shadowAtlas.descriptor );
	if ( drawRun ) {
		vk_update_descriptor( VK_DESC_TEXTURE1, drawRun->cutout->descriptor );
		vk_bind_pipeline( vk.shadowtile_recv_cut_pipeline[RB_ReverseWinding() ? 1 : 0] );
	} else {
		vk_bind_pipeline( vk.shadowtile_recv_pipeline[RB_ReverseWinding() ? 1 : 0] );
	}
	vk_update_mvp( NULL );
	vk_bind_index();
	vk_bind_geometry( TESS_XYZ | TESS_RGBA0 | TESS_ST0 | TESS_ST1 | TESS_ST2 | ( drawRun ? TESS_NNN : 0 ) );
	vk_draw_geometry( DEPTH_RANGE_NORMAL, qtrue );
	tess.numIndexes = tess.numVertexes = 0;
}
