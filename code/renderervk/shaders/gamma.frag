#version 450
#extension GL_GOOGLE_include_directive : require
#include "../vk_xr_color.h"
#include "postprocess_source.glsl"

#ifdef USE_BLOOM_COMPOSITE
layout(set = 0, binding = 0) uniform sceneSampler texture0;
layout(set = 0, binding = 1) uniform sceneSampler texture1; // emissive highlight layer
layout(set = 0, binding = 2) uniform sceneSampler bloom0;
layout(set = 0, binding = 3) uniform sceneSampler bloom1;
layout(set = 0, binding = 4) uniform sceneSampler bloom2;
layout(set = 0, binding = 5) uniform sceneSampler bloom3;
layout(constant_id = 4) const float bloomFactor = 0.5;
layout(constant_id = 15) const int bloomEnabled = 1;
layout(constant_id = 16) const int bloomFine = 0; // adds the half-resolution level
#else
layout(set = 0, binding = 0) uniform sceneSampler texture0;
layout(set = 1, binding = 0) uniform sceneSampler texture1; // emissive highlight layer
#endif

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
layout(constant_id = 11) const int hdrMode = 0;          // 0 - SDR, 1 - scRGB linear HDR, 3 - scRGB from the encoded eye image
layout(constant_id = 13) const int screenSource = 0; // 1: encoded composition, 2: linear menu capture
layout(constant_id = 12) const int linearSDROutput = 0; // SRGB headset attachment

layout(push_constant) uniform Push {
	int hdrCalibrate;   // 1 = draw the peak-match calibration window
	float paperWhite;   // nits SDR-white maps to
	float hdrPeak;      // nits display peak; highlights roll off toward this
	float hdrHighlight; // multiplier on the >1.0 headroom
	float hdrSaturation; // 0 = brightest emitters go white; 1 = keep full color
	float hdrSaturationFull; // emitter intensity at which the bleed reaches its max
	float hdrSoftKnee;  // width of the smooth highlight onset above paper-white
	int bloomActive;    // the blur ran this frame
} push;

const vec3 sRGB = { 0.2126, 0.7152, 0.0722 };

vec3 sRGBtoLinear(vec3 c) {
	bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
	vec3 lower = c / 12.92;
	vec3 higher = pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4));
	return mix(higher, lower, cutoff);
}

// Reconstructs scRGB-linear HDR output from the SDR color buffer and the emissive
// highlight layer. 1.0 == paper-white; result is scaled to scRGB (paper-white / 80).
vec3 hdrReconstruct( vec3 base, vec3 emissive, float coverage, bool encoded,
                     float gamma, float obScale,
                     float paperWhite, float hdrPeak, float hdrHighlight,
                     float hdrSaturation, float hdrSaturationFull, float hdrSoftKnee )
{
	// Restore only where an additive emitter exceeded SDR white AND the color
	// buffer is still clipped there. The emissive term excludes bloom, which
	// inflates base in halos but never writes the emissive layer (no fringing);
	// the base term restores 2D occlusion, since 2D composited on top darkens
	// base below the ceiling even where the emissive layer kept the emitter.
	// Coverage (what post-scene draws took from the scene) turns it off under them.
	float em = max( max( emissive.r, emissive.g ), emissive.b );
	float clip = smoothstep( 1.0, 1.1, em )
	           * smoothstep( 0.990, 1.0, max( max( base.r, base.g ), base.b ) ) * ( 1.0 - coverage );
	// An encoded base already carries gamma and overbright; the emissive layer never does.
	vec3 lin = encoded ? sRGBtoLinear( mix( base, max( base, pow( emissive, vec3( gamma ) ) * obScale ), clip ) )
	                   : sRGBtoLinear( pow( mix( base, max( base, emissive ), clip ), vec3( gamma ) ) * obScale );

	// Highlights (m>1.0): bleed over-white emitters toward white, then roll the
	// headroom off toward the panel peak.
	float m = max( max( lin.r, lin.g ), lin.b );
	if ( m > 1.0 )
	{
		float peak = max( hdrPeak / paperWhite, 1.0 );
		// Ramp the highlight treatment in across a soft shoulder above 1.0 so its slope
		// is continuous at the onset; a hard switch at m==1 reads as a fixed-intensity
		// Mach band (the "kick into overbright"). Full strength past the shoulder.
		float onset = hdrSoftKnee > 0.0 ? smoothstep( 1.0, 1.0 + hdrSoftKnee, m ) : 1.0;
		float luma = dot( lin, vec3( 0.2126, 0.7152, 0.0722 ) );
		float deficit = ( m - luma ) / m;
		// Gate on em (float emissive intensity) so only real emitters bleed, not
		// clipped saturated surfaces (em ~ 0, same m). Bleed = stronger of deficit
		// (blue crosses early) and intensity t (any hot core whitens), capped by
		// hdrSaturation.
		float t = smoothstep( 0.0, hdrSaturationFull, em );
		float toWhite = ( 1.0 - hdrSaturation ) * t * max( deficit, t ) * onset;
		lin = mix( lin, vec3( m ), toWhite );
		float boosted = 1.0 + (m - 1.0) * hdrHighlight;
		float rolled = peak * boosted / (peak - 1.0 + boosted); // soft asymptote at peak
		lin *= mix( 1.0, rolled / m, onset );
	}

	return lin * (paperWhite / 80.0);
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
	if ( ( hdrMode == 1 || hdrMode == 3 ) && push.hdrCalibrate == 1 ) {
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

	if ( hdrMode == 3 ) {
		vec4 eye = sceneSample(texture0, frag_tex_coord);
		out_color = vec4( hdrReconstruct( eye.rgb, sceneSample(texture1, frag_tex_coord).rgb, eye.a, true, gamma,
			obScale, push.paperWhite, push.hdrPeak, push.hdrHighlight, push.hdrSaturation,
			push.hdrSaturationFull, push.hdrSoftKnee ), 1.0 );
		return;
	}

	vec4 scene = sceneSample(texture0, frag_tex_coord);
	vec3 base = scene.rgb;
	float coverage = 0.0;
#ifdef USE_BLOOM_COMPOSITE
	if ( bloomEnabled != 0 && push.bloomActive != 0 ) {
		vec3 bloom = sceneSample(bloom1, frag_tex_coord).rgb
			+ sceneSample(bloom2, frag_tex_coord).rgb + sceneSample(bloom3, frag_tex_coord).rgb;
		if ( bloomFine != 0 )
			bloom += sceneSample(bloom0, frag_tex_coord).rgb;
		// bloom lands under the post-scene draws: the scene's alpha is what they let through
		base = min( base + bloom * bloomFactor * scene.a, vec3(1.0) );
	}
	coverage = 1.0 - scene.a;
#endif
	// Screen content already went through gamma/overbright before capture.
	if (screenSource != 0) {
		vec3 linear = screenSource == 2 ? base : sRGBtoLinear(base);
		if (hdrMode == 1) out_color = vec4(linear * (push.paperWhite / 80.0), 1.0);
		else if (linearSDROutput == 1) out_color = vec4(linear, 1.0);
		else if (screenSource == 2) out_color = vec4(VKXR_SdrEncoded(base.r),VKXR_SdrEncoded(base.g),VKXR_SdrEncoded(base.b),1.0);
		else out_color = vec4(base, 1.0);
		return;
	}

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
		vec3 emissive = sceneSample(texture1, frag_tex_coord).rgb;
		out_color = vec4( hdrReconstruct( base, emissive, coverage, false, gamma, obScale,
			push.paperWhite, push.hdrPeak, push.hdrHighlight, push.hdrSaturation,
			push.hdrSaturationFull, push.hdrSoftKnee ), 1.0 );
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
	if ( linearSDROutput == 1 ) {
		out_color.rgb=vec3(VKXR_SdrLinear(out_color.r),VKXR_SdrLinear(out_color.g),VKXR_SdrLinear(out_color.b));
	}
#ifdef USE_BLOOM_COMPOSITE
	// The HDR mirror reads post-scene coverage from the eye image's alpha.
	out_color.a = coverage;
#endif
}
