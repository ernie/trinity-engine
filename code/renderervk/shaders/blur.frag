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

// The bright pass: mode 0 (r|g|b) >= threshold, 1 (r+g+b)/3 >= threshold, 2 luma >= threshold
vec3 extract( vec3 base )
{
	const vec3 luma = vec3( 0.2126, 0.7152, 0.0722 );
	float v = dot( luma, base );
	bool bright;

	if ( extract_mode == 1 )
		bright = ( base.r + base.g + base.b ) * 0.33333333 >= threshold;
	else if ( extract_mode == 2 )
		bright = v >= threshold;
	else
		bright = base.r >= threshold || base.g >= threshold || base.b >= threshold;
	if ( !bright )
		return vec3( 0.0 );
	if ( base_modulate == 1 )
		return base * base;
	if ( base_modulate != 0 )
		return base * v;
	return base;
}

// Thresholding each filtered tap, not each texel, keeps the first pass at three fetches
vec3 tap( vec2 uv )
{
	return extract( sceneSample( texture0, uv ).rgb );
}
#else
vec3 tap( vec2 uv )
{
	return sceneSample( texture0, uv ).rgb;
}
#endif

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

	out_color = vec4( base, 1.0 );
}
