#ifndef TRINITY_VK_PASS_H
#define TRINITY_VK_PASS_H

/* Attachment view masks are a pass contract. Mask 1 keeps array
 * shader/descriptor compatibility while only executing layer 0. */
typedef enum {
	RENDER_PASS_MAIN = 0,
	RENDER_PASS_SCREENMAP,
	RENDER_PASS_POST_BLOOM,
	RENDER_PASS_HUD,
	RENDER_PASS_DESKTOP,
	RENDER_PASS_VR_SCREEN,
	RENDER_PASS_MONO_MAIN,
	RENDER_PASS_POST_SCENE_2D,
	RENDER_PASS_MONO_POST_SCENE_2D,
	RENDER_PASS_VR_SCREEN_EYE,
	RENDER_PASS_COUNT
} renderPass_t;

static ID_INLINE unsigned VK_PassViewMask( int multiview, renderPass_t pass ) {
	if ( !multiview || pass == RENDER_PASS_SCREENMAP ||
		pass == RENDER_PASS_HUD || pass == RENDER_PASS_DESKTOP ) return 0;
	if ( pass == RENDER_PASS_MONO_MAIN || pass == RENDER_PASS_MONO_POST_SCENE_2D ) return 1;
	return 3;
}

/* Both post-scene 2D subpasses apply the eye output chain per fragment and have no world depth. */
static ID_INLINE qboolean VK_PassIsPostScene2D( renderPass_t pass ) {
	return pass == RENDER_PASS_POST_SCENE_2D || pass == RENDER_PASS_MONO_POST_SCENE_2D;
}

/* The virtual-screen composition, into the scene image or straight into the eye pass; its eye matrices need no Y flip. */
static ID_INLINE qboolean VK_PassIsVRScreen( renderPass_t pass ) {
	return pass == RENDER_PASS_VR_SCREEN || pass == RENDER_PASS_VR_SCREEN_EYE;
}

/* NVIDIA bug 6413598 drops multiview point fragments. The array-view probe
 * uses triangle topology and omits PointSize. */
static ID_INLINE int VK_FlareProbeTriangles( int multiview, renderPass_t pass ) {
	return VK_PassViewMask( multiview, pass ) != 0;
}

#endif
