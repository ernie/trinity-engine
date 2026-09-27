#version 450
#extension GL_GOOGLE_include_directive : require
#include "../vk_xr_color.h"
layout(set = 1, binding = 0) uniform sampler2D virtualScreenTexture;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 0) out vec4 out_color;
// SPAN must match VK_SCREEN_REFLECT_SPAN in vk.c; the mesh covers only that band
const float SPAN = 0.25;
const float STRENGTH = 0.25;
const float BLUR_LOD = 3.0;
// the fade reaches zero inside the band, so its far edge never leaves a step of a few 8-bit levels
const float FADE_END = 0.9;
void main() {
	float depth = (1.0 - frag_tex_coord.y) / SPAN;
	vec3 color = textureLod(virtualScreenTexture, frag_tex_coord, BLUR_LOD).rgb;
	out_color = vec4(VKXR_SdrEncoded(color.r), VKXR_SdrEncoded(color.g), VKXR_SdrEncoded(color.b),
		STRENGTH * (1.0 - smoothstep(0.0, FADE_END, depth)));
}
