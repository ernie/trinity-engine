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

layout(push_constant) uniform Push {
	int hdrCalibrate;   // 1 = draw the peak-match calibration window
	float paperWhite;   // nits SDR-white maps to
	float hdrPeak;      // nits display peak; highlights roll off toward this
	float hdrHighlight; // multiplier on the >1.0 headroom
	float hdrSaturation; // 0 = brightest emitters go white; 1 = keep full color
	float hdrSaturationFull; // emitter intensity at which the bleed reaches its max
} push;

const vec3 sRGB = { 0.2126, 0.7152, 0.0722 };

vec3 sRGBtoLinear(vec3 c) {
	bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
	vec3 lower = c / 12.92;
	vec3 higher = pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4));
	return mix(higher, lower, cutoff);
}

// === BEGIN shared HDR core — keep byte-identical with
// trinity-vr code/renderervk/shaders/desktopmirror.frag (this is the canonical copy) ===
// Reconstructs scRGB-linear HDR output from the SDR color buffer and the emissive
// highlight layer. 1.0 == paper-white; result is scaled to scRGB (paper-white / 80).
vec3 hdrReconstruct( vec3 base, vec3 emissive,
                     float gamma, float obScale,
                     float paperWhite, float hdrPeak, float hdrHighlight,
                     float hdrSaturation, float hdrSaturationFull )
{
	// Restore only where an additive emitter exceeded SDR white AND the color
	// buffer is still clipped there. The emissive term excludes bloom, which
	// inflates base in halos but never writes the emissive layer (no fringing);
	// the base term restores 2D occlusion, since 2D composited on top darkens
	// base below the ceiling even where the emissive layer kept the emitter.
	float em = max( max( emissive.r, emissive.g ), emissive.b );
	float clip = smoothstep( 1.0, 1.1, em )
	           * smoothstep( 0.990, 1.0, max( max( base.r, base.g ), base.b ) );
	vec3 src = mix( base, max( base, emissive ), clip );

	vec3 lin = sRGBtoLinear( pow( src, vec3( gamma ) ) * obScale );

	// Highlights (m>1.0): bleed over-white emitters toward white, then roll the
	// headroom off toward the panel peak.
	float m = max( max( lin.r, lin.g ), lin.b );
	if ( m > 1.0 )
	{
		float peak = max( hdrPeak / paperWhite, 1.0 );
		float luma = dot( lin, vec3( 0.2126, 0.7152, 0.0722 ) );
		float deficit = ( m - luma ) / m;
		// Gate on em (float emissive intensity) so only real emitters bleed, not
		// clipped saturated surfaces (em ~ 0, same m). Bleed = stronger of deficit
		// (blue crosses early) and intensity t (any hot core whitens), capped by
		// hdrSaturation.
		float t = smoothstep( 0.0, hdrSaturationFull, em );
		float toWhite = ( 1.0 - hdrSaturation ) * t * max( deficit, t );
		lin = mix( lin, vec3( m ), toWhite );
		float boosted = 1.0 + (m - 1.0) * hdrHighlight;
		float rolled = peak * boosted / (peak - 1.0 + boosted); // soft asymptote at peak
		lin *= rolled / m;
	}

	return lin * (paperWhite / 80.0);
}
// === END shared HDR core ===

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
				nits = push.hdrPeak;              // inner: value being calibrated
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
		vec3 emissive = texture(texture1, frag_tex_coord).rgb;
		out_color = vec4( hdrReconstruct( base, emissive, gamma, obScale,
			push.paperWhite, push.hdrPeak, push.hdrHighlight, push.hdrSaturation,
			push.hdrSaturationFull ), 1.0 );
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
