/* Pure CPU math for soft model shadows: light-space tiles in a shared atlas. */
#ifndef VK_SHADOWTILE_MATH_H
#define VK_SHADOWTILE_MATH_H
#include <math.h>
#include <string.h>

#define SHADOWTILE_ATLAS_WIDTH		4096
#define SHADOWTILE_ATLAS_HEIGHT		2048
#define SHADOWTILE_LARGE			512
#define SHADOWTILE_SMALL			128
#define SHADOWTILE_MAX_LARGE		16
#define SHADOWTILE_LARGE_EXTENT		48.0f
#define SHADOWTILE_DEPTH_RANGE		1024.0f	/* the longest r_shadowFadeLength, 512, finishes behind casters up to 512 deep */
#define SHADOWTILE_VIEW_DISTANCE	2048.0f
#define SHADOWTILE_STRENGTH			0.55f

typedef struct {
	float axis[3][3];	/* axis[0] is the direction the light travels */
	float center[3];	/* on the near plane */
	float halfW, halfH;
	float nearDepth;
	float casterDepth;	/* the casters' own extent along axis[0] */
	float depthRange;
} shadowTileFit_t;

void ShadowTile_Basis( const float travel[3], float axis[3][3] );
int ShadowTile_Fit( const float mins[3], const float maxs[3], const float travel[3], float depthRange, shadowTileFit_t *out );
/* Column-major, world to Vulkan clip: x and y in [-1,1] across the tile, z in [0,1] from near to near + depthRange. */
void ShadowTile_WorldMatrix( const shadowTileFit_t *fit, float m[16] );
void ShadowTile_CasterMatrix( const shadowTileFit_t *fit, const float origin[3], const float axis[3][3], float mvp[16] );
/* Clip y -1 is the tile's top texel row, matching a Vulkan viewport over rect. */
void ShadowTile_Project( const shadowTileFit_t *fit, const int rect[4], int atlasWidth, int atlasHeight, const float world[3], float out[3] );
float ShadowTile_UvPerUnit( const shadowTileFit_t *fit, const int rect[4], int atlasWidth );
/* Wound so ( p1 - p0 ) x ( p2 - p0 ) points along the travel, which the mark clipper's side planes need. */
void ShadowTile_Footprint( const shadowTileFit_t *fit, float points[4][3] );
/* Large requests past maxLarge fall back to small tiles. */
int ShadowTile_Allocate( const int *large, int count, int atlasWidth, int atlasHeight, int largeSize, int smallSize, int maxLarge, int (*rects)[4] );
float ShadowTile_Darkness( const float ambient[3], const float directed[3], float strength );
/* World bounds of a model spinning about z at origin and bobbing anywhere inside origin's cellHeight-tall cell. */
void ShadowTile_SpinBounds( const float origin[3], const float localMins[3], const float localMaxs[3], float xyScale, float zScale,
	float cellHeight, float mins[3], float maxs[3] );
/* Like ShadowTile_Fit, with axis[1] taken from side made perpendicular to travel. */
int ShadowTile_FitAligned( const float mins[3], const float maxs[3], const float travel[3], const float side[3], float depthRange,
	shadowTileFit_t *out );
float ShadowTile_QuantizePad( float reach, float half );
void ShadowTile_QuantizeTravel( const float travel[3], int key[3], float out[3] );
/* Whether the box swept reach along unit travel lies wholly behind one plane; a plane's inside is normal . p >= dist. */
int ShadowTile_SweptBoxCulled( const float mins[3], const float maxs[3], const float travel[3], float reach,
	const float (*planes)[4], int numPlanes );

#endif
