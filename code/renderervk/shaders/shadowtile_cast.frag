#version 450

#ifdef USE_CUTOUT
#extension GL_GOOGLE_include_directive : require
#include "shadowtile.glsl"

layout(set = 1, binding = 0) uniform sampler2D cutout;

layout(location = 0) in vec2 cutout_st;
layout(location = 1) flat in float cutout_test;
#endif

layout(location = 0) out vec4 out_depth;

void main() {
#ifdef USE_CUTOUT
	if (shadowtile_cut_away(texture(cutout, cutout_st).a, cutout_test)) {
		discard;
	}
#endif
	out_depth = vec4(gl_FragCoord.z);
}
