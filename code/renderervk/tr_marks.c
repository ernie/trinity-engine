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
// tr_marks.c -- polygon projection on the world polygons

#include "tr_local.h"
//#include "assert.h"

#define MAX_VERTS_ON_POLY		64

#define MARKER_OFFSET			0	// 1

#define VectorCopy2(a,b)		((b)[0]=(a)[0],(b)[1]=(a)[1])

/*
=============
R_ChopPolyBehindPlane

Out must have space for two more vertexes than in
=============
*/
#define	SIDE_FRONT	0
#define	SIDE_BACK	1
#define	SIDE_ON		2
void R_ChopPolyBehindPlane( int numInPoints, vec3_t inPoints[MAX_VERTS_ON_POLY],
								int *numOutPoints, vec3_t outPoints[MAX_VERTS_ON_POLY],
							vec3_t normal, vec_t dist, vec_t epsilon) {
	float		dists[MAX_VERTS_ON_POLY+4];
	int			sides[MAX_VERTS_ON_POLY+4];
	int			counts[3];
	float		dot;
	int			i, j;
	float		*p1, *p2, *clip;
	float		d;

	// don't clip if it might overflow
	if ( numInPoints >= MAX_VERTS_ON_POLY - 2 ) {
		*numOutPoints = 0;
		return;
	}

	counts[0] = counts[1] = counts[2] = 0;
	dists[0] = 0.0;
	sides[0] = 0;

	// determine sides for each point
	for ( i = 0 ; i < numInPoints ; i++ ) {
		dot = DotProduct( inPoints[i], normal );
		dot -= dist;
		dists[i] = dot;
		if ( dot > epsilon ) {
			sides[i] = SIDE_FRONT;
		} else if ( dot < -epsilon ) {
			sides[i] = SIDE_BACK;
		} else {
			sides[i] = SIDE_ON;
		}
		counts[sides[i]]++;
	}
	sides[i] = sides[0];
	dists[i] = dists[0];

	*numOutPoints = 0;

	if ( !counts[0] ) {
		return;
	}
	if ( !counts[1] ) {
		*numOutPoints = numInPoints;
		Com_Memcpy( outPoints, inPoints, numInPoints * sizeof(vec3_t) );
		return;
	}

	for ( i = 0 ; i < numInPoints ; i++ ) {
		p1 = inPoints[i];
		clip = outPoints[ *numOutPoints ];
		
		if ( sides[i] == SIDE_ON ) {
			VectorCopy( p1, clip );
			(*numOutPoints)++;
			continue;
		}
	
		if ( sides[i] == SIDE_FRONT ) {
			VectorCopy( p1, clip );
			(*numOutPoints)++;
			clip = outPoints[ *numOutPoints ];
		}

		if ( sides[i+1] == SIDE_ON || sides[i+1] == sides[i] ) {
			continue;
		}
			
		// generate a split point
		p2 = inPoints[ (i+1) % numInPoints ];

		d = dists[i] - dists[i+1];
		if ( d == 0 ) {
			dot = 0;
		} else {
			dot = dists[i] / d;
		}

		// clip xyz

		for (j=0 ; j<3 ; j++) {
			clip[j] = p1[j] + dot * ( p2[j] - p1[j] );
		}

		(*numOutPoints)++;
	}
}

/*
=================
R_SurfaceSkipped

=================
*/
static qboolean R_SurfaceSkipped( const msurface_t *surf, int flags ) {
	const shader_t *sh = surf->shader;

	if ( ( sh->surfaceFlags & ( SURF_NOIMPACT | SURF_NOMARKS ) ) || ( sh->contentFlags & CONTENTS_FOG ) ) {
		return qtrue;
	}
	if ( flags & BOXSURF_RECEIVERS ) {
		return sh->sort > SS_OPAQUE || !sh->stages[0];
	}
	return qfalse;
}

/*
=================
R_FaceBehindAPlane

A face with no point in front of some plane loses every triangle to that plane's chop
=================
*/
static qboolean R_FaceBehindAPlane( const srfSurfaceFace_t *face, int numPlanes, vec3_t *normals, const float *dists ) {
	int p, i;

	for ( p = 0; p < numPlanes; p++ ) {
		for ( i = 0; i < face->numPoints; i++ ) {
			if ( DotProduct( face->points[i], normals[p] ) - dists[p] > 0.5f ) {
				break;
			}
		}
		if ( i == face->numPoints ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
R_BoundsBehindAPlane

=================
*/
static qboolean R_BoundsBehindAPlane( const vec3_t mins, const vec3_t maxs, int numPlanes, vec3_t *normals, const float *dists ) {
	int p, k;

	for ( p = 0; p < numPlanes; p++ ) {
		float front = 0.0f;
		for ( k = 0; k < 3; k++ ) {
			front += normals[p][k] * ( normals[p][k] > 0.0f ? maxs[k] : mins[k] );
		}
		if ( front - dists[p] <= 0.5f ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
R_SurfaceBehindAPlane

=================
*/
static qboolean R_SurfaceBehindAPlane( const msurface_t *surf, int numPlanes, vec3_t *normals, const float *dists ) {
	switch ( *surf->data ) {
	case SF_FACE:
		return R_FaceBehindAPlane( (const srfSurfaceFace_t *)surf->data, numPlanes, normals, dists );
	case SF_GRID:
		return R_BoundsBehindAPlane( ( (const srfGridMesh_t *)surf->data )->meshBounds[0],
			( (const srfGridMesh_t *)surf->data )->meshBounds[1], numPlanes, normals, dists );
	case SF_TRIANGLES:
		return R_BoundsBehindAPlane( ( (const srfTriangles_t *)surf->data )->bounds[0],
			( (const srfTriangles_t *)surf->data )->bounds[1], numPlanes, normals, dists );
	default:
		return qfalse;
	}
}

/*
=================
R_BoxSurfaces_r

=================
*/
void R_BoxSurfaces_r(mnode_t *node, vec3_t mins, vec3_t maxs, const msurface_t **list, int listsize, int *listlength, vec3_t dir, int flags,
		int numPlanes, vec3_t *normals, const float *dists) {

	int			s, c;
	msurface_t	*surf, **mark;

	// do the tail recursion in a loop
	while ( node->contents == CONTENTS_NODE ) {
		s = BoxOnPlaneSide( mins, maxs, node->plane );
		if (s == 1) {
			node = node->children[0];
		} else if (s == 2) {
			node = node->children[1];
		} else {
			R_BoxSurfaces_r(node->children[0], mins, maxs, list, listsize, listlength, dir, flags, numPlanes, normals, dists);
			node = node->children[1];
		}
	}

	// add the individual surfaces
	mark = node->firstmarksurface;
	c = node->nummarksurfaces;
	while (c--) {
		//
		if (*listlength >= listsize) break;
		//
		surf = *mark;
		if ( R_SurfaceSkipped( surf, flags ) || R_SurfaceBehindAPlane( surf, numPlanes, normals, dists ) ) {
			surf->viewCount = tr.viewCount;
		}
		// extra check for surfaces to avoid list overflows
		else if (*(surf->data) == SF_FACE) {
			// the face plane should go through the box
			s = BoxOnPlaneSide( mins, maxs, &(( srfSurfaceFace_t * ) surf->data)->plane );
			if (s == 1 || s == 2) {
				surf->viewCount = tr.viewCount;
			} else if (!(flags & BOXSURF_RADIAL) && DotProduct((( srfSurfaceFace_t * ) surf->data)->plane.normal, dir) > -0.5) {
			// don't add faces that make sharp angles with the projection direction
				surf->viewCount = tr.viewCount;
			}
		}
		else if (*(surfaceType_t *) (surf->data) != SF_GRID &&
			 *(surfaceType_t *) (surf->data) != SF_TRIANGLES)
			surf->viewCount = tr.viewCount;
		// check the viewCount because the surface may have
		// already been added if it spans multiple leafs
		if (surf->viewCount != tr.viewCount) {
			surf->viewCount = tr.viewCount;
			list[*listlength] = surf;
			(*listlength)++;
		}
		mark++;
	}
}

/*
=================
R_BmodelSurfaces

=================
*/
void R_BmodelSurfaces( const bmodel_t *bmodel, vec3_t mins, vec3_t maxs, const msurface_t **list, int listsize, int *listlength, const vec3_t dir, int flags,
		int numPlanes, vec3_t *normals, const float *dists ) {
	const msurface_t	*surf;
	const srfGridMesh_t	*grid;
	const srfTriangles_t	*tris;
	cplane_t			*plane;
	int					i, s;

	for ( i = 0; i < bmodel->numSurfaces && *listlength < listsize; i++ ) {
		surf = bmodel->firstSurface + i;
		if ( R_SurfaceSkipped( surf, flags ) || R_SurfaceBehindAPlane( surf, numPlanes, normals, dists ) ) {
			continue;
		}
		if ( *surf->data == SF_FACE ) {
			plane = &( ( srfSurfaceFace_t * ) surf->data )->plane;
			s = BoxOnPlaneSide( mins, maxs, plane );
			if ( s == 1 || s == 2 || ( !( flags & BOXSURF_RADIAL ) && DotProduct( plane->normal, dir ) > -0.5 ) ) {
				continue;
			}
		} else if ( *surf->data == SF_GRID ) {
			grid = ( const srfGridMesh_t * ) surf->data;
			if ( !BoundsIntersect( mins, maxs, grid->meshBounds[0], grid->meshBounds[1] ) ) {
				continue;
			}
		} else if ( *surf->data == SF_TRIANGLES ) {
			tris = ( const srfTriangles_t * ) surf->data;
			if ( !BoundsIntersect( mins, maxs, tris->bounds[0], tris->bounds[1] ) ) {
				continue;
			}
		} else {
			continue;
		}
		list[(*listlength)++] = surf;
	}
}

/*
=================
R_AddMarkFragments

=================
*/
static void R_AddMarkFragments(int numClipPoints, vec3_t clipPoints[2][MAX_VERTS_ON_POLY],
				   int numPlanes, vec3_t *normals, float *dists,
				   int maxPoints, vec3_t pointBuffer,
				   int maxFragments, markFragment_t *fragmentBuffer,
				   int *returnedPoints, int *returnedFragments,
				   vec3_t mins, vec3_t maxs) {
	int pingPong, i;
	markFragment_t	*mf;

	// chop the surface by all the bounding planes of the to be projected polygon
	pingPong = 0;

	for ( i = 0 ; i < numPlanes ; i++ ) {

		R_ChopPolyBehindPlane( numClipPoints, clipPoints[pingPong],
						   &numClipPoints, clipPoints[!pingPong],
							normals[i], dists[i], 0.5 );
		pingPong ^= 1;
		if ( numClipPoints == 0 ) {
			break;
		}
	}
	// completely clipped away?
	if ( numClipPoints == 0 ) {
		return;
	}

	// add this fragment to the returned list
	if ( numClipPoints + (*returnedPoints) > maxPoints ) {
		return;	// not enough space for this polygon
	}
	/*
	// all the clip points should be within the bounding box
	for ( i = 0 ; i < numClipPoints ; i++ ) {
		int j;
		for ( j = 0 ; j < 3 ; j++ ) {
			if (clipPoints[pingPong][i][j] < mins[j] - 0.5) break;
			if (clipPoints[pingPong][i][j] > maxs[j] + 0.5) break;
		}
		if (j < 3) break;
	}
	if (i < numClipPoints) return;
	*/

	mf = fragmentBuffer + (*returnedFragments);
	mf->firstPoint = (*returnedPoints);
	mf->numPoints = numClipPoints;
	Com_Memcpy( pointBuffer + (*returnedPoints) * 3, clipPoints[pingPong], numClipPoints * sizeof(vec3_t) );

	(*returnedPoints) += numClipPoints;
	(*returnedFragments)++;
}

#define MAX_CLIP_SURFACES	256

typedef struct {
	float		behind, ahead;		// window along the projection around points[0]
	float		faceFacing;			// faces are kept while normal . projection is at most this
	float		gridFacing[2];		// the same for each grid quad's two triangles
	int			maxSurfaces;
	qboolean	triangleSoups;
	int			boxFlags;
	const msurface_t	**fragmentSurfaces;	// optional: each fragment's source surface
	float				(*pointST)[2];		// optional: each point's texture coordinates on that surface
} markClip_t;

/*
=================
R_NoteFragmentSource

=================
*/
static void R_NoteFragmentSource( const markClip_t *clip, const msurface_t *surf, vec3_t tri[3], float st[3][2],
		int fragment, int numFragments, const markFragment_t *fragmentBuffer, const float *pointBuffer ) {
	const markFragment_t	*mf;
	vec3_t					e0, e1, d;
	float					d00, d01, d11, denom, b1, b2;
	int						k;

	if ( !clip->fragmentSurfaces || fragment == numFragments ) {
		return;
	}
	clip->fragmentSurfaces[fragment] = surf;
	mf = fragmentBuffer + fragment;
	VectorSubtract( tri[1], tri[0], e0 );
	VectorSubtract( tri[2], tri[0], e1 );
	d00 = DotProduct( e0, e0 );
	d01 = DotProduct( e0, e1 );
	d11 = DotProduct( e1, e1 );
	denom = d00 * d11 - d01 * d01;
	if ( denom == 0.0f ) {
		denom = 1.0f;
	}
	for ( k = 0; k < mf->numPoints; k++ ) {
		float *out = clip->pointST[mf->firstPoint + k];
		float d20, d21;

		VectorSubtract( pointBuffer + ( mf->firstPoint + k ) * 3, tri[0], d );
		d20 = DotProduct( d, e0 );
		d21 = DotProduct( d, e1 );
		b1 = ( d11 * d20 - d01 * d21 ) / denom;
		b2 = ( d00 * d21 - d01 * d20 ) / denom;
		out[0] = st[0][0] + b1 * ( st[1][0] - st[0][0] ) + b2 * ( st[2][0] - st[0][0] );
		out[1] = st[0][1] + b1 * ( st[1][1] - st[0][1] ) + b2 * ( st[2][1] - st[0][1] );
	}
}

/*
=================
R_MarkFragmentsOn

=================
*/
static int R_MarkFragmentsOn( const bmodel_t *bmodel, int numPoints, const vec3_t *points, const vec3_t projection,
				   const markClip_t *clip,
				   int maxPoints, vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer ) {
	int				numsurfaces, numPlanes;
	int				i, j, k, m, n, before;
	const msurface_t	*surfaces[MAX_CLIP_SURFACES];
	vec3_t			tri[3];
	float			triST[3][2];
	vec3_t			mins, maxs;
	int				returnedFragments;
	int				returnedPoints;
	vec3_t			normals[MAX_VERTS_ON_POLY+2];
	float			dists[MAX_VERTS_ON_POLY+2];
	vec3_t			clipPoints[2][MAX_VERTS_ON_POLY];
	int				numClipPoints;
	float			*v;
	srfGridMesh_t	*cv;
	drawVert_t		*dv;
	vec3_t			normal;
	vec3_t			projectionDir;
	vec3_t			v1, v2;
	int				*indexes;

	if (numPoints <= 0) {
		return 0;
	}

	//increment view count for double check prevention
	tr.viewCount++;

	//
	VectorNormalize2( projection, projectionDir );
	// find all the brushes that are to be considered
	ClearBounds( mins, maxs );
	for ( i = 0 ; i < numPoints ; i++ ) {
		vec3_t	temp;

		AddPointToBounds( points[i], mins, maxs );
		VectorAdd( points[i], projection, temp );
		AddPointToBounds( temp, mins, maxs );
		// make sure we get all the leafs (also the one(s) in front of the hit surface)
		VectorMA( points[i], -20, projectionDir, temp );
		AddPointToBounds( temp, mins, maxs );
	}

	if (numPoints > MAX_VERTS_ON_POLY) numPoints = MAX_VERTS_ON_POLY;
	// create the bounding planes for the to be projected polygon
	for ( i = 0 ; i < numPoints ; i++ ) {
		VectorSubtract(points[(i+1)%numPoints], points[i], v1);
		VectorAdd(points[i], projection, v2);
		VectorSubtract(points[i], v2, v2);
		CrossProduct(v1, v2, normals[i]);
		VectorNormalizeFast(normals[i]);
		dists[i] = DotProduct(normals[i], points[i]);
	}
	// add near and far clipping planes for projection
	VectorCopy(projectionDir, normals[numPoints]);
	dists[numPoints] = DotProduct(normals[numPoints], points[0]) - clip->behind;
	VectorCopy(projectionDir, normals[numPoints+1]);
	VectorInverse(normals[numPoints+1]);
	dists[numPoints+1] = DotProduct(normals[numPoints+1], points[0]) - clip->ahead;
	numPlanes = numPoints + 2;

	numsurfaces = 0;
	if ( bmodel ) {
		R_BmodelSurfaces( bmodel, mins, maxs, surfaces, clip->maxSurfaces, &numsurfaces, projectionDir, clip->boxFlags,
			numPlanes, normals, dists );
	} else {
		R_BoxSurfaces_r(tr.world->nodes, mins, maxs, surfaces, clip->maxSurfaces, &numsurfaces, projectionDir, clip->boxFlags,
			numPlanes, normals, dists);
	}
	//assert(numsurfaces <= 64);
	//assert(numsurfaces != 64);

	returnedPoints = 0;
	returnedFragments = 0;

	for ( i = 0 ; i < numsurfaces ; i++ ) {

		if (*surfaces[i]->data == SF_GRID) {

			cv = (srfGridMesh_t *) surfaces[i]->data;
			for ( m = 0 ; m < cv->height - 1 ; m++ ) {
				for ( n = 0 ; n < cv->width - 1 ; n++ ) {
					// We triangulate the grid and chop all triangles within
					// the bounding planes of the to be projected polygon.
					// LOD is not taken into account, not such a big deal though.
					//
					// It's probably much nicer to chop the grid itself and deal
					// with this grid as a normal SF_GRID surface so LOD will
					// be applied. However the LOD of that chopped grid must
					// be synced with the LOD of the original curve.
					// One way to do this; the chopped grid shares vertices with
					// the original curve. When LOD is applied to the original
					// curve the unused vertices are flagged. Now the chopped curve
					// should skip the flagged vertices. This still leaves the
					// problems with the vertices at the chopped grid edges.
					//
					// To avoid issues when LOD applied to "hollow curves" (like
					// the ones around many jump pads) we now just add a 2 unit
					// offset to the triangle vertices.
					// The offset is added in the vertex normal vector direction
					// so all triangles will still fit together.
					// The 2 unit offset should avoid pretty much all LOD problems.

					numClipPoints = 3;

					dv = cv->verts + m * cv->width + n;

					VectorCopy(dv[0].xyz, clipPoints[0][0]);
					VectorMA(clipPoints[0][0], MARKER_OFFSET, dv[0].normal, clipPoints[0][0]);
					VectorCopy(dv[cv->width].xyz, clipPoints[0][1]);
					VectorMA(clipPoints[0][1], MARKER_OFFSET, dv[cv->width].normal, clipPoints[0][1]);
					VectorCopy(dv[1].xyz, clipPoints[0][2]);
					VectorMA(clipPoints[0][2], MARKER_OFFSET, dv[1].normal, clipPoints[0][2]);
					// check the normal of this triangle
					VectorSubtract(clipPoints[0][0], clipPoints[0][1], v1);
					VectorSubtract(clipPoints[0][2], clipPoints[0][1], v2);
					CrossProduct(v1, v2, normal);
					VectorNormalizeFast(normal);
					if (DotProduct(normal, projectionDir) < clip->gridFacing[0]) {
						for ( j = 0; j < 3; j++ ) {
							VectorCopy( clipPoints[0][j], tri[j] );
						}
						VectorCopy2( dv[0].st, triST[0] );
						VectorCopy2( dv[cv->width].st, triST[1] );
						VectorCopy2( dv[1].st, triST[2] );
						before = returnedFragments;
						// add the fragments of this triangle
						R_AddMarkFragments(numClipPoints, clipPoints,
										   numPlanes, normals, dists,
										   maxPoints, pointBuffer,
										   maxFragments, fragmentBuffer,
										   &returnedPoints, &returnedFragments, mins, maxs);
						R_NoteFragmentSource( clip, surfaces[i], tri, triST, before, returnedFragments, fragmentBuffer, pointBuffer );

						if ( returnedFragments == maxFragments ) {
							return returnedFragments;	// not enough space for more fragments
						}
					}

					VectorCopy(dv[1].xyz, clipPoints[0][0]);
					VectorMA(clipPoints[0][0], MARKER_OFFSET, dv[1].normal, clipPoints[0][0]);
					VectorCopy(dv[cv->width].xyz, clipPoints[0][1]);
					VectorMA(clipPoints[0][1], MARKER_OFFSET, dv[cv->width].normal, clipPoints[0][1]);
					VectorCopy(dv[cv->width+1].xyz, clipPoints[0][2]);
					VectorMA(clipPoints[0][2], MARKER_OFFSET, dv[cv->width+1].normal, clipPoints[0][2]);
					// check the normal of this triangle
					VectorSubtract(clipPoints[0][0], clipPoints[0][1], v1);
					VectorSubtract(clipPoints[0][2], clipPoints[0][1], v2);
					CrossProduct(v1, v2, normal);
					VectorNormalizeFast(normal);
					if (DotProduct(normal, projectionDir) < clip->gridFacing[1]) {
						for ( j = 0; j < 3; j++ ) {
							VectorCopy( clipPoints[0][j], tri[j] );
						}
						VectorCopy2( dv[1].st, triST[0] );
						VectorCopy2( dv[cv->width].st, triST[1] );
						VectorCopy2( dv[cv->width+1].st, triST[2] );
						before = returnedFragments;
						// add the fragments of this triangle
						R_AddMarkFragments(numClipPoints, clipPoints,
										   numPlanes, normals, dists,
										   maxPoints, pointBuffer,
										   maxFragments, fragmentBuffer,
										   &returnedPoints, &returnedFragments, mins, maxs);
						R_NoteFragmentSource( clip, surfaces[i], tri, triST, before, returnedFragments, fragmentBuffer, pointBuffer );

						if ( returnedFragments == maxFragments ) {
							return returnedFragments;	// not enough space for more fragments
						}
					}
				}
			}
		}
		else if (*surfaces[i]->data == SF_FACE) {

			srfSurfaceFace_t *surf = ( srfSurfaceFace_t * ) surfaces[i]->data;

			// check the normal of this face
			if (DotProduct(surf->plane.normal, projectionDir) > clip->faceFacing) {
				continue;
			}

			indexes = (int *)( (byte *)surf + surf->ofsIndices );
			for ( k = 0 ; k < surf->numIndices ; k += 3 ) {
				for ( j = 0 ; j < 3 ; j++ ) {
					v = &surf->points[0][0] + VERTEXSIZE * indexes[k+j];
					VectorMA( v, MARKER_OFFSET, surf->plane.normal, clipPoints[0][j] );
					VectorCopy( clipPoints[0][j], tri[j] );
					triST[j][0] = v[3];
					triST[j][1] = v[4];
				}

				before = returnedFragments;
				// add the fragments of this face
				R_AddMarkFragments( 3 , clipPoints,
								   numPlanes, normals, dists,
								   maxPoints, pointBuffer,
								   maxFragments, fragmentBuffer,
								   &returnedPoints, &returnedFragments, mins, maxs);
				R_NoteFragmentSource( clip, surfaces[i], tri, triST, before, returnedFragments, fragmentBuffer, pointBuffer );
				if ( returnedFragments == maxFragments ) {
					return returnedFragments;	// not enough space for more fragments
				}
			}
		}
		else if(*surfaces[i]->data == SF_TRIANGLES && clip->triangleSoups) {

			srfTriangles_t *surf = (srfTriangles_t *) surfaces[i]->data;

			for (k = 0; k < surf->numIndexes; k += 3)
			{
				for(j = 0; j < 3; j++)
				{
					v = surf->verts[surf->indexes[k + j]].xyz;
					VectorMA(v, MARKER_OFFSET, surf->verts[surf->indexes[k + j]].normal, clipPoints[0][j]);
					VectorCopy( clipPoints[0][j], tri[j] );
					VectorCopy2( surf->verts[surf->indexes[k + j]].st, triST[j] );
				}

				before = returnedFragments;
				// add the fragments of this face
				R_AddMarkFragments(3, clipPoints,
								   numPlanes, normals, dists,
								   maxPoints, pointBuffer,
								   maxFragments, fragmentBuffer, &returnedPoints, &returnedFragments, mins, maxs);
				R_NoteFragmentSource( clip, surfaces[i], tri, triST, before, returnedFragments, fragmentBuffer, pointBuffer );
				if(returnedFragments == maxFragments)
				{
					return returnedFragments;	// not enough space for more fragments
				}
			}
		}
	}
	return returnedFragments;
}

/*
=================
R_MarkClip

=================
*/
static void R_MarkClip( markClip_t *clip ) {
	clip->behind = 32.0f;
	clip->ahead = 20.0f;
	clip->faceFacing = -0.5f;
	clip->gridFacing[0] = -0.1f;
	clip->gridFacing[1] = -0.05f;
	clip->maxSurfaces = 64;
	clip->triangleSoups = r_marksOnTriangleMeshes->integer ? qtrue : qfalse;
	clip->boxFlags = 0;
	clip->fragmentSurfaces = NULL;
	clip->pointST = NULL;
}

/*
=================
R_MarkFragments

=================
*/
int R_MarkFragments( int numPoints, const vec3_t *points, const vec3_t projection,
				   int maxPoints, vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer ) {
	markClip_t clip;

	R_MarkClip( &clip );
	return R_MarkFragmentsOn( NULL, numPoints, points, projection, &clip, maxPoints, pointBuffer, maxFragments, fragmentBuffer );
}

/*
=================
R_MarkFragments2

=================
*/
int R_MarkFragments2( int numPoints, const vec3_t *points, const vec3_t projection,
				   int maxPoints, vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer,
				   qhandle_t model, const vec3_t modelOrigin, const vec3_t modelAxis[3] ) {
	const model_t	*mod;
	vec3_t			localPoints[MAX_VERTS_ON_POLY];
	vec3_t			localProjection;
	vec3_t			delta;
	int				i, j;
	markClip_t		clip;

	R_MarkClip( &clip );
	mod = R_GetSubmodelByHandle( model );
	if ( !mod || numPoints <= 0 || !modelOrigin || !modelAxis ) {
		return 0;
	}

	if ( numPoints > MAX_VERTS_ON_POLY ) {
		numPoints = MAX_VERTS_ON_POLY;
	}
	for ( i = 0; i < numPoints; i++ ) {
		VectorSubtract( points[i], modelOrigin, delta );
		for ( j = 0; j < 3; j++ ) {
			localPoints[i][j] = DotProduct( delta, modelAxis[j] );
		}
	}
	for ( j = 0; j < 3; j++ ) {
		localProjection[j] = DotProduct( projection, modelAxis[j] );
	}

	return R_MarkFragmentsOn( mod->bmodel, numPoints, (const vec3_t *)localPoints, localProjection, &clip,
		maxPoints, pointBuffer, maxFragments, fragmentBuffer );
}

/*
=================
R_ShadowReceiverFragments

=================
*/
int R_ShadowReceiverFragments( const vec3_t points[4], const vec3_t travel, float length,
		qhandle_t bmodel, const vec3_t bmodelOrigin, const vec3_t bmodelAxis[3],
		int maxPoints, vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer,
		const msurface_t **fragmentSurfaces, float (*pointST)[2] ) {
	markClip_t		clip;
	const model_t	*mod;
	vec3_t			localPoints[4], projection, delta;
	int				i, j, numFragments, numPoints;

	clip.behind = 1.0f;
	clip.ahead = length;
	clip.faceFacing = 0.0f;
	clip.gridFacing[0] = clip.gridFacing[1] = 0.0f;
	clip.maxSurfaces = MAX_CLIP_SURFACES;
	clip.triangleSoups = qfalse;
	clip.boxFlags = BOXSURF_RADIAL | BOXSURF_RECEIVERS;
	clip.fragmentSurfaces = fragmentSurfaces;
	clip.pointST = pointST;

	if ( !bmodel ) {
		VectorScale( travel, length, projection );
		return R_MarkFragmentsOn( NULL, 4, points, projection, &clip, maxPoints, pointBuffer, maxFragments, fragmentBuffer );
	}

	mod = R_GetSubmodelByHandle( bmodel );
	if ( !mod || !mod->bmodel ) {
		return 0;
	}
	for ( i = 0; i < 4; i++ ) {
		VectorSubtract( points[i], bmodelOrigin, delta );
		for ( j = 0; j < 3; j++ ) {
			localPoints[i][j] = DotProduct( delta, bmodelAxis[j] );
		}
	}
	for ( j = 0; j < 3; j++ ) {
		projection[j] = length * DotProduct( travel, bmodelAxis[j] );
	}
	numFragments = R_MarkFragmentsOn( mod->bmodel, 4, (const vec3_t *)localPoints, projection, &clip,
		maxPoints, pointBuffer, maxFragments, fragmentBuffer );

	numPoints = 0;
	for ( i = 0; i < numFragments; i++ ) {
		if ( fragmentBuffer[i].firstPoint + fragmentBuffer[i].numPoints > numPoints ) {
			numPoints = fragmentBuffer[i].firstPoint + fragmentBuffer[i].numPoints;
		}
	}
	for ( i = 0; i < numPoints; i++ ) {
		float *p = pointBuffer + i * 3;
		vec3_t local;
		VectorCopy( p, local );
		for ( j = 0; j < 3; j++ ) {
			p[j] = bmodelOrigin[j] + local[0] * bmodelAxis[0][j] + local[1] * bmodelAxis[1][j] + local[2] * bmodelAxis[2][j];
		}
	}
	return numFragments;
}

