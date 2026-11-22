// Values shared with vk_shadowtile_math.h.
const float SHADOWTILE_DEPTH_RANGE = 1024.0;
const vec2 SHADOWTILE_LARGE_UV = vec2(512.0 / 4096.0, 512.0 / 2048.0);
const vec2 SHADOWTILE_SMALL_UV = vec2(128.0 / 4096.0, 128.0 / 2048.0);

// test is the surface's alpha test, GLS_ATEST_* shifted down to 1, 2 or 3
bool shadowtile_cut_away(float alpha, float test) {
	int t = int(test + 0.5);
	return t == 1 ? alpha == 0.0 : t == 2 ? alpha >= 0.5 : alpha < 0.5;
}
