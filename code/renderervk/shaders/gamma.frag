#version 450

layout(set = 0, binding = 0) uniform sampler2D texture0;
layout(set = 1, binding = 0) uniform sampler2D texture1; // emissive highlight layer

layout(location = 0) in vec2 frag_tex_coord;

layout(location = 0) out vec4 out_color;

layout(constant_id = 0) const float gamma = 1.0;
layout(constant_id = 1) const float obScale = 2.0;
layout(constant_id = 2) const float greyscale = 0.0;
//
layout(constant_id = 7) const int ditherMode = 0; // 0 - disabled, 1 - ordered
layout(constant_id = 8) const int depth_r = 255;
layout(constant_id = 9) const int depth_g = 255;
layout(constant_id = 10) const int depth_b = 255;
layout(constant_id = 11) const int hdrMode = 0;          // 0 - SDR, 1 - scRGB linear HDR
layout(constant_id = 12) const float paperWhite = 200.0; // nits SDR-white maps to
layout(constant_id = 13) const float hdrHighlight = 1.0; // multiplier on the >1.0 headroom
layout(constant_id = 14) const float hdrPeak = 1000.0;   // nits display peak; highlights roll off toward this
layout(constant_id = 15) const int hdrEmissive = 0;      // 1 - reconstruct highlights from the emissive layer

layout(push_constant) uniform Push {
	int hdrCalibrate; // 1 = draw the peak-match calibration window
} push;

const vec3 sRGB = { 0.2126, 0.7152, 0.0722 };

vec3 sRGBtoLinear(vec3 c) {
	bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
	vec3 lower = c / 12.92;
	vec3 higher = pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4));
	return mix(higher, lower, cutoff);
}

const int bayerSize = 8;
const float bayerMatrix[bayerSize * bayerSize] = {
	0,  32, 8,  40, 2,  34, 10, 42,
	48, 16, 56, 24, 50, 18, 58, 26,
	12, 44, 4,  36, 14, 46, 6,  38,
	60, 28, 52, 20, 62, 30, 54, 22,
	3,  35, 11, 43, 1,  33, 9,  41,
	51, 19, 59, 27, 49, 17, 57, 25,
	15, 47, 7,  39, 13, 45, 5,  37,
	63, 31, 55, 23, 61, 29, 53, 21
};

float threshold() {
	ivec2 coordDenormalized = ivec2(gl_FragCoord.xy);
	ivec2 bayerCoord = coordDenormalized % bayerSize;
	float bayerSample = bayerMatrix[bayerCoord.x + bayerCoord.y * bayerSize];
	float threshold = (bayerSample + 0.5) / float(bayerSize * bayerSize);
	return threshold;
}

vec3 dither(vec3 color) {
	ivec3 depth = ivec3(depth_r, depth_g, depth_b);
	vec3 cDenormalized = color * depth;
	vec3 cLow = floor(cDenormalized);
	vec3 cFractional = cDenormalized - cLow;
	vec3 cDithered = cLow + step(threshold(), cFractional);
	return cDithered / depth;
}

void main() {
	if ( hdrMode == 1 && push.hdrCalibrate == 1 ) {
		// Peak-match test: a fixed outer rectangle that clips to the panel's true
		// peak, and an inner rectangle at r_hdrPeak. Raise r_hdrPeak until the inner
		// edge vanishes = panel peak. Window ~5% of pixels, 2.4:1:
		//   area 4*wx*wy = 0.05, wx = 2.4*wy -> wy = sqrt(0.05/9.6) = 0.0722
		// Centered at 0.38 to leave room below for the menu controls.
		vec2 d = abs(frag_tex_coord - vec2(0.5, 0.38));
		const float wy = 0.0722;
		const float wx = 0.1733; // wy * 2.4
		if ( d.x < wx && d.y < wy ) {
			float nits = 10000.0;                 // outer: clips to panel peak
			if ( d.x < wx * 0.5 && d.y < wy * 0.5 ) {
				nits = hdrPeak;                   // inner: value being calibrated
			}
			out_color = vec4(vec3(nits / 80.0), 1.0);
			return;
		}
	}

	vec3 base = texture(texture0, frag_tex_coord).rgb;

	if ( greyscale == 1 )
	{
		base = vec3(dot(base, sRGB));
	}
	else if ( greyscale != 0 )
	{
		vec3 luma = vec3(dot(base, sRGB));
		base = mix(base, luma, greyscale);
	}

	if ( hdrMode == 1 )
	{
		vec3 src = base;
		if ( hdrEmissive == 1 ) {
			vec3 emissive = texture(texture1, frag_tex_coord).rgb;
			// Restore a channel only where base is still clipped to the ceiling,
			// so 2D (drawn on top, below the ceiling) occludes the emitter.
			vec3 saturated = step( vec3( 0.999 ), base );
			src = mix( base, max( base, emissive ), saturated );
		}
		vec3 lin = sRGBtoLinear(pow(src, vec3(gamma)) * obScale); // 1.0 == paper-white

		// Hue-preserving highlights: scale all channels by one factor from the
		// brightest, rolling the headroom toward the panel peak.
		float m = max(max(lin.r, lin.g), lin.b);
		if ( m > 1.0 )
		{
			float peak = max(hdrPeak / paperWhite, 1.0);            // ceiling in paper-white units (>= paper-white)
			float boosted = 1.0 + (m - 1.0) * hdrHighlight;         // highlight push
			float rolled = peak * boosted / (peak - 1.0 + boosted); // soft asymptote at peak
			lin *= rolled / m;
		}

		out_color = vec4(lin * (paperWhite / 80.0), 1.0);
		return;
	}

	if ( gamma != 1.0 )
	{
		out_color = vec4(pow(base, vec3(gamma)) * obScale, 1);
	}
	else
	{
		out_color = vec4(base * obScale, 1);
	}

	if ( ditherMode == 1 ) {
		out_color.rgb = dither(out_color.rgb);
	}
}
