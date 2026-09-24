#version 450

#extension GL_GOOGLE_include_directive : require
#include "view_transform.glsl"

layout(location = 0) in vec3 in_position;
//layout(location = 1) in vec4 in_color;
//layout(location = 2) in vec2 in_tex_coord0;
//layout(location = 3) in vec2 in_tex_coord1;

//layout(location = 0) out vec4 frag_color;
//layout(location = 1) out vec2 frag_tex_coord0;
//layout(location = 2) out vec2 frag_tex_coord1;

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	gl_Position = transformPosition(in_position);

	//frag_color = in_color;
	//frag_tex_coord0 = in_tex_coord0;
}
