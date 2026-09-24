#version 450

// Floor grid in tracking-space meters.
layout(location = 1) in vec2 frag_tex_coord0;

layout(location = 0) out vec4 out_color;

void main() {
	vec2 P = (frag_tex_coord0 - vec2(0.5)) * 30.0;

	vec2 f = fract(P);
	vec2 d = min(f, 1.0 - f);
	vec2 w = max(fwidth(P) * 1.5, vec2(1e-6));
	vec2 a = 1.0 - smoothstep(vec2(0.0), w, d);
	float lineAlpha = max(a.x, a.y);
	float dotRadius = 0.02;
	float dist = length(d);
	float dotAlpha = 1.0 - smoothstep(0.0, dotRadius, dist);

	vec4 background = vec4(0.0);
	vec4 gridGrey = vec4(vec3(0.075), 1.0);
	vec4 dotWhite = vec4(vec3(0.5), 1.0);

	vec4 col1 = mix(background, gridGrey, lineAlpha);
	vec4 col2 = mix(background, dotWhite, dotAlpha);

	float distToCamera = length(vec3(P.x, 0.0, P.y));

	out_color = max(col1, col2);
	out_color.gb *= 1.0 - (distToCamera / 5.0);
	out_color.a *= 1.0 - (distToCamera / 15.0);
}
