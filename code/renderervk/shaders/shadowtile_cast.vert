#version 450

#extension GL_GOOGLE_include_directive : require
#include "view_transform.glsl"

layout(location = 0) in vec3 in_position;
#ifdef USE_CUTOUT
layout(location = 1) in vec4 in_cutout;

layout(location = 0) out vec2 cutout_st;
layout(location = 1) flat out float cutout_test;
#endif

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	gl_Position = transformPosition(in_position);
#ifdef USE_CUTOUT
	cutout_st = in_cutout.xy;
	cutout_test = in_cutout.z;
#endif
}
