#version 450
#extension GL_GOOGLE_include_directive : require
#include "../vk_xr_color.h"
layout(set = 1, binding = 0) uniform sampler2D virtualScreenTexture;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 0) out vec4 out_color;
void main() {
	vec3 color = texture(virtualScreenTexture, frag_tex_coord).rgb;
	out_color = vec4(VKXR_SdrEncoded(color.r), VKXR_SdrEncoded(color.g), VKXR_SdrEncoded(color.b), 1.0);
}
