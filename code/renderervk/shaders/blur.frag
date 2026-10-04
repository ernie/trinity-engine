#version 450
#extension GL_GOOGLE_include_directive : require
#include "postprocess_source.glsl"

// 3-tap gaussian blur
// exploiting linear filtering with -1.2 0 +1.2 texture offsets and 5 6 5 weighting
// to emulate 5-tap blur

layout(set = 0, binding = 0) uniform sceneSampler texture0;

layout(location = 0) in vec2 tex_coord0;

layout(location = 0) out vec4 out_color;

layout(constant_id = 0) const float texoffset_x = 0.0;
layout(constant_id = 1) const float texoffset_y = 0.0;

#ifdef USE_EXTRACT
layout(constant_id = 3) const float threshold = 0.6;
layout(constant_id = 5) const int extract_mode = 0;
layout(constant_id = 6) const int base_modulate = 0;
layout(constant_id = 7) const float knee = 0.1;

// The bright pass: mode 0 max(r,g,b), 1 (r+g+b)/3, 2 luma, ramped in across threshold +- knee.
// A step flips a texel in and out of the bloom as it crosses the threshold, which shimmers under head motion.
vec3 extract( vec3 base )
{
	const vec3 luma = vec3( 0.2126, 0.7152, 0.0722 );
	float v = dot( luma, base );
	float level;
	float gate;

	if ( extract_mode == 1 )
		level = ( base.r + base.g + base.b ) * 0.33333333;
	else if ( extract_mode == 2 )
		level = v;
	else
		level = max( base.r, max( base.g, base.b ) );
	gate = knee > 0.0 ? smoothstep( threshold - knee, threshold + knee, level ) : step( threshold, level );
	if ( base_modulate == 1 )
		return base * base * gate;
	if ( base_modulate != 0 )
		return base * v * gate;
	return base * gate;
}
#endif

vec3 tap( vec2 uv )
{
	return sceneSample( texture0, uv ).rgb;
}

void main()
{
	vec2 tex_coord1 = tex_coord0;
	vec2 tex_coord2 = tex_coord0;

	tex_coord1.x += texoffset_x;
	tex_coord1.y += texoffset_y;

	tex_coord2.x -= texoffset_x;
	tex_coord2.y -= texoffset_y;

	vec3 base = tap( tex_coord0 ) * (6.0 / 16.0)
		+ tap( tex_coord1 ) * (5.0 / 16.0)
		+ tap( tex_coord2 ) * (5.0 / 16.0);

#ifdef USE_EXTRACT
	// the filtered neighborhood decides what blooms, so a lone bright texel on a textured surface does not
	base = extract( base );
#endif
	out_color = vec4( base, 1.0 );
}
