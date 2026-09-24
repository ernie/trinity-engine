// Scene attachments are arrays only in XR. Material and HUD textures stay 2D.
#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif
#if defined(MULTIVIEW) || defined(ARRAY_SOURCE)
#define sceneSampler sampler2DArray
#ifdef ARRAY_SOURCE
layout(constant_id = 14) const int sourceLayer = 0;
#define SCENE_LAYER sourceLayer
#else
#define SCENE_LAYER gl_ViewIndex
#endif
vec4 sceneSample(sampler2DArray source, vec2 uv) {
    return texture(source, vec3(uv, float(SCENE_LAYER)));
}
#else
#define sceneSampler sampler2D
vec4 sceneSample(sampler2D source, vec2 uv) {
    return texture(source, uv);
}
#endif
