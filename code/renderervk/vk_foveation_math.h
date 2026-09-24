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
#endif
