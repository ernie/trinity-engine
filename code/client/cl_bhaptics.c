#include "cl_bhaptics.h"
#include "../vrcommon/vr_float.h"
#include <string.h>
#include <math.h>
typedef void (*clBHProc_t)(void);
#if defined(_WIN32)
#include <windows.h>
static void *load( void ) {
	return (void *)LoadLibraryA( "haptic_library.dll" );
}
static clBHProc_t symbol( void *p, const char *n ) {
	return (clBHProc_t)GetProcAddress( (HMODULE)p, n );
}
static void unload( void *p ) {
	FreeLibrary( (HMODULE)p );
}
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
static void *load( void ) {
	return dlopen( "libhaptic_library.so", RTLD_NOW | RTLD_LOCAL );
}
static clBHProc_t symbol( void *p, const char *n ) {
	return (clBHProc_t)dlsym( p, n );
}
static void unload( void *p ) {
	dlclose( p );
}
#else
static void *load( void ) {
	return NULL;
}
static clBHProc_t symbol( void *p, const char *n ) {
	(void)p;
	(void)n;
	return NULL;
}
static void unload( void *p ) {
	(void)p;
}
#endif
int CL_BHaptics_Open( clBHaptics_t *ctx ) {
	void (*init)(const char *, const char *);

	if ( !ctx )
		return 0;
	if ( ctx->initialized )
		return 1;
	memset( ctx, 0, sizeof( *ctx ) );
	ctx->library = load();
	if ( !ctx->library )
		return 0;
	init = (void (*)(const char *, const char *))symbol( ctx->library, "Initialise" );
	ctx->destroy = (void (*)(void))symbol( ctx->library, "Destroy" );
	ctx->stop = (void (*)(void))symbol( ctx->library, "TurnOff" );
	ctx->submit =
		(void (*)(const char *, int, clBHPoint_t *, size_t, int))symbol( ctx->library, "SubmitPathArray" );
	if ( !init || !ctx->destroy || !ctx->stop || !ctx->submit ) {
		unload( ctx->library );
		memset( ctx, 0, sizeof( *ctx ) );
		return 0;
	}
	init( "trinity", "Trinity" );
	ctx->initialized = 1;
	return 1;
}
void CL_BHaptics_Stop( clBHaptics_t *ctx ) {
	if ( ctx && ctx->initialized && ctx->playing ) {
		ctx->stop();
		ctx->playing = 0;
	}
}
void CL_BHaptics_Close( clBHaptics_t *ctx ) {
	if ( !ctx )
		return;
	CL_BHaptics_Stop( ctx );
	if ( ctx->initialized )
		ctx->destroy();
	if ( ctx->library )
		unload( ctx->library );
	memset( ctx, 0, sizeof( *ctx ) );
}
static float clamp( float v ) {
	return v < 0 ? 0 : v > 1 ? 1 : v;
}
static void pulse( clBHaptics_t *ctx, const char *key, int position, int intensity, int duration ) {
	clBHPoint_t p = {.5f, .5f, intensity, 3};
	ctx->submit( key, position, &p, 1, duration );
	ctx->playing = 1;
}
static void impact( clBHaptics_t *ctx, const char *key, int visor, float yaw, float height, int intensity,
					int duration ) {
	clBHPoint_t p[5];
	float radians = yaw * .0174532925199433f, x = clamp( .5f + sinf( radians ) * .5f ),
			y = clamp( .5f + height * .5f ), spread = visor ? .08f : .12f;
	int i, edge = (int)(intensity * .6f);
	if ( edge < 1 && intensity > 0 )
		edge = 1;
	for ( i = 0; i < 5; i++ ) {
		p[i].x = x;
		p[i].y = y;
		p[i].intensity = i ? edge : intensity;
		p[i].motorCount = visor ? 4 : 6;
	}
	p[1].x = clamp( x - spread );
	p[2].x = clamp( x + spread );
	p[3].y = clamp( y + spread );
	p[4].y = clamp( y - spread );
	ctx->submit( key, visor ? 4 : (yaw > 90 && yaw < 270 ? 202 : 201), p, 5, duration );
	ctx->playing = 1;
}
void CL_BHaptics_Event( clBHaptics_t *ctx, const char *event, int position, int intensity, float yaw,
						float height, float scale, int right, int menuLeft ) {
	const char *separator;
	float scaled;
	int value;
	if ( !ctx || !ctx->initialized || !event || !VR_FloatFinite( yaw ) || !VR_FloatFinite( height ) ||
		!VR_FloatFinite( scale ) || scale <= 0 )
		return;
	scaled = intensity * scale;
	if ( !VR_FloatFinite( scaled ) )
		return;
	value = (int)(scaled < 0 ? 0 : scaled > 100 ? 100 : scaled);
	if ( !value )
		return;
	yaw = fmodf( yaw, 360 );
	if ( yaw < 0 )
		yaw += 360;
	separator = strchr( event, ':' );
	if ( separator )
		event = separator + 1;
	if ( !strncmp( event, "pickup_", 7 ) ) {
		impact( ctx, "bhaptics_pickup", 0, 0, .3f, value, 130 );
		return;
	}
	if ( !strcmp( event, "weapon_switch" ) ) {
		pulse( ctx, "bhaptics_weapon_switch", right ? 7 : 6, value, 110 );
		return;
	}
	if ( !strcmp( event, "menu_move" ) ) {
		pulse( ctx, "bhaptics_menu_move", menuLeft ? 6 : 7, value / 2, 60 );
		return;
	}
	if ( !strcmp( event, "jump_start" ) ) {
		impact( ctx, "bhaptics_jump_start", 0, yaw, height, value, 140 );
		return;
	}
	if ( !strcmp( event, "jump_landing" ) ) {
		impact( ctx, "bhaptics_jump_land", 0, yaw, height, value, 200 );
		return;
	}
	if ( !strcmp( event, "spark" ) || !strcmp( event, "shield_break" ) ) {
		impact( ctx, "bhaptics_shield", 0, yaw, height, value, 220 );
		impact( ctx, "bhaptics_visor_impact", 1, yaw, height, value, 160 );
		return;
	}
	if ( !strcmp( event, "bullet" ) || !strcmp( event, "shotgun" ) || !strcmp( event, "fireball" ) ) {
		impact( ctx, "bhaptics_impact", 0, yaw, height, value, 210 );
		impact( ctx, "bhaptics_visor_impact", 1, yaw, height, value, 170 );
		return;
	}
	if ( strstr( event, "_fire" ) ) {
		if ( position == 1 )
			right = 1;
		else if ( position == 2 )
			right = 0;
		pulse( ctx, right ? "bhaptics_fire_right" : "bhaptics_fire_left", right ? 7 : 6, value, 140 );
		pulse( ctx, right ? "bhaptics_sleeve_fire_right" : "bhaptics_sleeve_fire_left", right ? 11 : 10, value,
				140 );
		return;
	}
	impact( ctx, "bhaptics_generic", 0, yaw, height, value, 120 );
}
