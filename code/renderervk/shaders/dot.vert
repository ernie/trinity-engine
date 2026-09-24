#version 450

#extension GL_GOOGLE_include_directive : require
#include "view_transform.glsl"

layout(set = 0, binding = 0) buffer SSBO {
	#ifdef MULTIVIEW
	int sampled[2];
#else
	int sampled;
#endif
};

layout(location = 0) in vec3 in_position;

out gl_PerVertex {
	vec4 gl_Position;
#ifndef MULTIVIEW
	float gl_PointSize;
#endif
};

void main() {
	#ifndef MULTIVIEW
	sampled = 0;
#endif
	gl_Position = transformPosition(in_position);
#ifndef MULTIVIEW
	gl_PointSize = 1.0;
#endif
}
