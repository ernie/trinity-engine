#version 450

#extension GL_GOOGLE_include_directive : require
#include "shadowtile.glsl"

layout(set = 1, binding = 0) uniform sampler2D atlas;
#ifdef USE_CUTOUT
layout(set = 2, binding = 0) uniform sampler2D cutout;
#endif

layout(location = 0) in vec2 atlas_uv;
layout(location = 1) in float receiver_depth;
layout(location = 2) flat in float uv_per_unit;
layout(location = 3) flat in vec4 tile;
layout(location = 4) in float darkness;
layout(location = 7) flat in float fade_length;
layout(location = 8) in vec3 world_pos;
#ifdef USE_CUTOUT
layout(location = 5) in vec2 cutout_st;
layout(location = 6) flat in float cutout_test;
#endif

layout(location = 0) out vec4 out_color;

const float RECEIVER_BIAS = 2.0;
// penumbra width per unit of distance between caster and receiver: the light's apparent size
const float PENUMBRA_SLOPE = 0.04;

const vec2 POISSON[16] = vec2[](
	vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
	vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
	vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
	vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

// blends the four nearest depth tests so a moving caster's edge slides between texels
float caster_coverage(vec2 uv, float receiver, vec2 texel) {
	vec2 p = uv / texel - 0.5;
	vec2 base = floor(p), f = p - base;
	vec4 lit = step(vec4(receiver - RECEIVER_BIAS), textureGather(atlas, (base + 1.0) * texel, 0) * SHADOWTILE_DEPTH_RANGE);
	// gather order: (0,1) (1,1) (1,0) (0,0)
	vec4 s = vec4(1.0) - lit;
	return mix(mix(s.w, s.z, f.x), mix(s.x, s.y, f.x), f.y);
}

void main() {
#ifdef USE_CUTOUT
	if (shadowtile_cut_away(texture(cutout, cutout_st).a, cutout_test)) {
		discard;
	}
#endif
	vec2 texel = vec2(1.0) / vec2(textureSize(atlas, 0));
	vec2 lo = tile.xy + 0.5 * texel, hi = tile.zw - 0.5 * texel;
	float receiver = receiver_depth * SHADOWTILE_DEPTH_RANGE;
	// a grazing receiver's depth changes across one atlas texel
	vec2 du = dFdx(atlas_uv), dv = dFdy(atlas_uv);
	float dzx = dFdx(receiver), dzy = dFdy(receiver);
	float det = du.x * dv.y - du.y * dv.x;
	vec2 slope = det != 0.0 ? vec2(dzx * dv.y - dzy * du.y, dzy * du.x - dzx * dv.x) / det : vec2(0.0);
	float texelDepth = abs(slope.x) * texel.x + abs(slope.y) * texel.y;
	// cached item receivers reach a little past their tile
	if (any(lessThan(atlas_uv, tile.xy)) || any(greaterThan(atlas_uv, tile.zw))) {
		out_color = vec4(1.0);
		return;
	}
	float first = texture(atlas, clamp(atlas_uv, lo, hi)).g * SHADOWTILE_DEPTH_RANGE;

	// rotating the taps per quarter-unit world cell gives noise both eyes share and the world holds still
	uvec3 cell = uvec3(ivec3(floor(world_pos * 4.0)));
	uint h = cell.x * 73856093u ^ cell.y * 19349663u ^ cell.z * 83492791u;
	h = h * 747796405u + 2891336453u;
	h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
	h ^= h >> 22u;
	float angle = 6.2831853 * float(h) / 4294967296.0;
	mat2 turn = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
	// uv_per_unit is along u; the atlas is wider than tall, so the same world offset spans more of v
	vec2 stretch = vec2(1.0, texel.y / texel.x);

	// the widest penumbra any blocker could give this receiver bounds where blockers can be
	float search = max(receiver * PENUMBRA_SLOPE * uv_per_unit, texel.x);
	float blockers = 0.0, count = 0.0;
	for (int i = 0; i < 16; i++) {
		float d = texture(atlas, clamp(atlas_uv + turn * POISSON[i] * search * stretch, lo, hi)).r * SHADOWTILE_DEPTH_RANGE;
		if (d < receiver - RECEIVER_BIAS) {
			blockers += d;
			count += 1.0;
		}
	}
	if (count == 0.0) {
		out_color = vec4(1.0);
		return;
	}
	// a first surface beyond the blockers hides this receiver from them
	if (receiver > first + RECEIVER_BIAS + texelDepth && first > blockers / count) {
		out_color = vec4(1.0);
		return;
	}

	float distance = receiver - blockers / count;
	float radius = max(distance * PENUMBRA_SLOPE * uv_per_unit, texel.x);
	float shadow = 0.0;
	for (int i = 0; i < 16; i++) {
		shadow += caster_coverage(clamp(atlas_uv + turn * POISSON[i] * radius * stretch, lo, hi), receiver, texel);
	}
	// umbra-like: near full strength by the caster, gone at the fade length
	float along = clamp(distance / fade_length, 0.0, 1.0);
	float fade = 1.0 - along * along;
	out_color = vec4(vec3(1.0 - darkness * fade * shadow / 16.0), 1.0);
}
