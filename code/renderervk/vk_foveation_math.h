/* Pure CPU layer-local shading-rate policy. */
#ifndef VK_FOVEATION_MATH_H
#define VK_FOVEATION_MATH_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
typedef struct {
	uint32_t width, height, samples;
} vkFovRate_t;
typedef struct {
	uint32_t x, y, width, height;
} vkFovRect_t;
typedef struct {
	float center[2][2], tangent[2][4];
	int eyeTracked, valid;
	int64_t heldTime;
} vkFovCenters_t;
typedef struct {
	uint32_t width, height, texelWidth, texelHeight; /* Per-eye image extent. */
	vkFovRect_t eye[2];
	float center[2][2], tangent[2][4];
	float display[2][4]; /* Tangents of the unzoomed eye field; centers stay in the zoomed one's NDC. */
	int strength, eyeTracked;
	uint32_t samples;
} vkFovMap_t;
int VK_FovMode( int requested, int attachmentSupported, int gazeSupported );
uint8_t VK_FovLegalRate( const vkFovRate_t *, uint32_t count, uint32_t width, uint32_t height,
						 uint32_t samples );
/* Quaternions use runtime x/y/z/w, all orientations share one reference space.
 * Gaze direction is head local; time is predicted display time in nanoseconds.
 * Updates fixed/gaze centers, preserving a valid gaze across a one-second blink.
 * Caller sets effective strength zero for virtual screens (full rate text). */
void VK_FovCenters( vkFovCenters_t *, const float head[4], const float eye[2][4], const float fov[2][4],
					int mode, int virtualScreen, int scoped, const float gaze[3], int gazeValid,
					int64_t time );
/* dst holds two contiguous layers, each ceil(eye extent / texel extent).
 * Each eye rectangle uses layer-local coordinates; partial/outside tiles stay full-rate.
 * Returns zero for invalid geometry or insufficient destination capacity. */
int VK_FovWrite( uint8_t *dst, size_t capacity, const vkFovMap_t *, const vkFovRate_t *, uint32_t count );
/* Fragment density maps: two R8G8 layers, one per eye, over the per-eye scene extent. */
typedef struct {
	uint32_t width, height, texelWidth, texelHeight;
	uint32_t tileWidth, tileHeight; /* Tiler bin; zero when unknown. */
} vkFdmGeometry_t;
uint32_t VK_FdmMapWidth( const vkFdmGeometry_t * );
uint32_t VK_FdmMapHeight( const vkFdmGeometry_t * );
size_t VK_FdmBytes( const vkFdmGeometry_t * );
/* Each eye's optical axis in pixels, moved to the middle of its bin when the bin is known. */
void VK_FdmReference( const vkFdmGeometry_t *, const float tangent[2][4], int32_t ref[2][2] );
/* The map drawn around the reference points, whole bins at a level; strength 0 is all full density. */
int VK_FdmWriteFixed( uint8_t *dst, size_t capacity, const vkFdmGeometry_t *, const float tangent[2][4],
					  const int32_t ref[2][2], int strength, int eyeTracked );
/* The map drawn around each eye's NDC center, for passes that cannot offset it. */
int VK_FdmWriteGaze( uint8_t *dst, size_t capacity, const vkFdmGeometry_t *, const float tangent[2][4],
					 const float center[2][2], int strength, int eyeTracked );
/* The scope's view: full density in every cell that reaches the centered ellipse (pixel radii), coarsest
 * over the mask outside it; a cell is a bin when known, else a texel. No usable ellipse leaves it all full. */
int VK_FdmWriteScope( uint8_t *dst, size_t capacity, const vkFdmGeometry_t *, float radiusX, float radiusY );
/* GL-style NDC bounds (min x, min y, max x, max y) of the rectangle in (x0, y0, x1, y1) through a column-major
 * matrix producing Vulkan clip space (+Y down); 0 when a corner is behind the eye. */
int VK_FdmPlaneBounds( const float m[16], const float in[4], float rect[4] );
/* Full density over every texel a GL-style NDC rectangle (min x, min y, max x, max y, +Y up) touches in one layer,
 * where a map slid by offset pixels is read. */
void VK_FdmFullDensityNdc( uint8_t *map, const vkFdmGeometry_t *, int layer, const float rect[4],
						   const int32_t offset[2] );
/* Pixels each eye's fixed map slides to reach its center, in whole granularity steps. */
void VK_FdmOffsets( const vkFdmGeometry_t *, const float center[2][2], const int32_t ref[2][2],
					uint32_t granularityWidth, uint32_t granularityHeight, int strength, int32_t offset[2][2] );
/* Fragment edge in pixels the map asks for at a pixel: a power of two up to 16. */
int VK_FdmBlockAt( const uint8_t *map, const vkFdmGeometry_t *, int layer, float x, float y,
				   const int32_t offset[2] );
/* The same at a GL-style NDC position, +Y up, as the renderer's eye projections produce. */
int VK_FdmBlockAtNdc( const uint8_t *map, const vkFdmGeometry_t *, int layer, float x, float y,
					  const int32_t offset[2] );
/* Turnip's bin for a two-view pass on an Adreno 750, from each GMEM attachment's bytes a pixel. */
int VK_FdmTurnipTile( uint32_t width, uint32_t height, const uint32_t *cpp, uint32_t count, uint32_t maxWidth,
					  uint32_t maxHeight, uint32_t *tileWidth, uint32_t *tileHeight );
/* Largest bin Turnip allows once the depth image is made for density map offsets. */
void VK_FdmTurnipOffsetLimit( uint32_t width, uint32_t height, uint32_t samples, uint32_t *maxWidth,
							  uint32_t *maxHeight );
#endif
