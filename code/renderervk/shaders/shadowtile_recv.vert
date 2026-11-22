#version 450

#extension GL_GOOGLE_include_directive : require
#include "view_transform.glsl"
#include "shadowtile.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_atlas_uv;
layout(location = 3) in vec2 in_depth_scale;
layout(location = 4) in vec2 in_tile_min;
#ifdef USE_CUTOUT
layout(location = 5) in vec4 in_cutout;
#endif

layout(location = 0) out vec2 atlas_uv;
layout(location = 1) out float receiver_depth;
layout(location = 2) flat out float uv_per_unit;
layout(location = 3) flat out vec4 tile;
layout(location = 4) out float darkness;
layout(location = 7) flat out float fade_length;
layout(location = 8) out vec3 world_pos;
#ifdef USE_CUTOUT
layout(location = 5) out vec2 cutout_st;
layout(location = 6) flat out float cutout_test;
#endif

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	gl_Position = transformPosition(in_position);
	atlas_uv = in_atlas_uv;
	receiver_depth = in_depth_scale.x;
	uv_per_unit = in_depth_scale.y;
	tile = vec4(in_tile_min, in_tile_min + (in_color.g > 0.5 ? SHADOWTILE_LARGE_UV : SHADOWTILE_SMALL_UV));
	darkness = in_color.r;
	fade_length = in_color.b * SHADOWTILE_DEPTH_RANGE;
	world_pos = in_position;
#ifdef USE_CUTOUT
	cutout_st = in_cutout.xy;
	cutout_test = in_cutout.z;
#endif
}
