/* Pure math for the desktop mirror: one eye's crop and its stabilization. */
#ifndef VK_MIRROR_MATH_H
#define VK_MIRROR_MATH_H

/* Spare eye image kept on each side: a pan at this speed's lag over the smoothing time, at least the jitter floor. */
#define MIRROR_PAN_SPEED		50.0f
#define MIRROR_MARGIN_FLOOR		3.0f

/* aspect is the window's height over its width; seconds is the smoothing time. Returns the widest span across, in
 * whole tens of degrees, that leaves the margin on every side. */
int VKM_StabilizedFov( const float tangents[4], float aspect, float seconds );

/* The degrees across a one-eye mirror crops for a setting of fov, 0 the whole eye: while stabilizing (seconds above
 * 0), no wider than VKM_StabilizedFov. Returns 0 for the whole eye. */
float VKM_MirrorFov( float fov, const float tangents[4], float aspect, float seconds );

/* The half span in tangents, across and up, of a crop fov degrees across a width by height window. It stops at the
 * image's nearest edge, and stays wide enough that the viewport magnifying it fits maxWidth by maxHeight. Returns the
 * degrees across it covers, 0 for no crop. */
float VKM_Span( float fov, const float tangents[4], int width, int height, int maxWidth, int maxHeight, float *halfW,
				float *halfH );

/* Holds center where a crop of halfW by halfH tangents stays inside the image, or in the middle of an image too small
 * for it. */
void VKM_HoldCenter( const float tangents[4], float halfW, float halfH, float center[2] );

/* Turns smoothed toward current so the angle between them shrinks by e^(-dt/seconds), the same at any frame rate.
 * Both are unit directions. */
void VKM_Follow( float smoothed[3], const float current[3], float dt, float seconds );

/* Where smoothed falls in the eye image, as tangents right and up from straight ahead, held where a crop of
 * halfW by halfH tangents stays inside the image; smoothed moves to match a held center. axis is the eye's right,
 * up and forward. */
void VKM_CropCenter( const float axis[3][3], float smoothed[3], const float tangents[4], float halfW, float halfH,
					 float center[2] );

#endif
