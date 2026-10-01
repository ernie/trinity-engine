#ifndef VR_BIND_H
#define VR_BIND_H
/* VR binding logic with no engine dependencies: key identities, input sampling, contexts, hold ownership,
 * display names and default layouts. The client owns storage and runs the bound commands. */
#include "vr_input_types.h"

#define VR_BINDING_MAX 256

/* Order matches the engine's K_VR_* key codes (client/keycodes.h). */
typedef enum {
	VRK_WPN_TRIGGER, VRK_OFF_TRIGGER, VRK_WPN_GRIP, VRK_OFF_GRIP, VRK_WPN_GRIPCLICK, VRK_OFF_GRIPCLICK,
	VRK_WPN_THUMBREST, VRK_OFF_THUMBREST, VRK_WPN_BUMPER, VRK_OFF_BUMPER, VRK_WPN_TRACKPAD, VRK_OFF_TRACKPAD,
	VRK_WPN_A, VRK_WPN_B, VRK_OFF_A, VRK_OFF_B,
	VRK_MOVESTICK, VRK_TURNSTICK,
	VRK_MOVESTICK_UP, VRK_MOVESTICK_DOWN, VRK_MOVESTICK_LEFT, VRK_MOVESTICK_RIGHT,
	VRK_TURNSTICK_UP, VRK_TURNSTICK_DOWN, VRK_TURNSTICK_LEFT, VRK_TURNSTICK_RIGHT,
	VRK_A, VRK_B, VRK_X, VRK_Y, VRK_MENU, VRK_VIEW,
	VRK_DPAD_UP, VRK_DPAD_DOWN, VRK_DPAD_LEFT, VRK_DPAD_RIGHT,
	VRK_COUNT
} vrKey_t;

typedef enum {
	VRC_GLOBAL, VRC_MENU, VRC_ADJUST, VRC_SCRUB, VRC_SCOREBOARD, VRC_VOTE, VRC_WHEEL, VRC_FOLLOW, VRC_GAMEPLAY,
	VRC_COUNT
} vrContext_t;

typedef enum {
	VRB_OFFLINE, VRB_PLAYING, VRB_DEAD, VRB_SPECTATING, VRB_FOLLOWING, VRB_INTERMISSION, VRB_DEMO, VRB_TV
} vrBase_t;

typedef struct {
	int textEntry, menu, adjust, scrub, scoreboard, vote, wheel;
	int offline, tv, demo, following, intermission, dead, spectating;
} vrSignals_t;

typedef struct {
	int count;
	vrContext_t layers[VRC_COUNT];
	vrBase_t base;
} vrStack_t;

typedef struct {
	float triggerPress, triggerRelease, gripPress, gripRelease, padPress, padRelease;
	float stickPress, stickRelease, deadzone;
} vrThresholds_t;

typedef const char *(*vrLookup_t)( vrContext_t context, int alt, vrKey_t key, void *user );

typedef enum { VRE_PRESS, VRE_RELEASE } vrBindEventType_t;
typedef struct {
	vrBindEventType_t type;
	vrKey_t key;
	vrContext_t layer;
	char binding[VR_BINDING_MAX];
} vrBindEvent_t;

typedef struct {
	unsigned char down[VRK_COUNT];
	signed char owner[VRK_COUNT];
	char binding[VRK_COUNT][VR_BINDING_MAX];
	int repeatAt[VRK_COUNT];
	int base; /* vrBase_t of the last update, -1 before the first */
	int mapping; /* handedness and stick switch the keys were named under, -1 before the first */
	unsigned char gesture[VRK_COUNT]; /* bindings-menu capture: keys pressed since the gesture began */
	int gestureFirst;				  /* the gesture's first key, -1 while none is pressed */
} vrHolds_t;

/* Controller families share button labels and default layouts. */
enum { VRF_TOUCH = 1, VRF_INDEX = 2, VRF_FRAME = 4, VRF_SIMPLE = 8, VRF_PLAY = VRF_TOUCH | VRF_INDEX | VRF_FRAME };

int VR_ProfileFamily( int profile );
float VR_StickCurve( float value, float deadzone );
void VR_SampleKeys( const clXRHandInput_t hands[2], int rightHanded, int switchSticks, const vrThresholds_t *t,
					const unsigned char previous[VRK_COUNT], unsigned char out[VRK_COUNT] );
void VR_RoleSticks( const clXRHandInput_t hands[2], int rightHanded, int switchSticks, float deadzone, float move[2],
					float turn[2] );
/* Physical hand (0 left, 1 right) a hand-role or stick-role key reads; -1 for one-hand keys. */
int VR_KeyHand( vrKey_t key, int rightHanded, int switchSticks );

void VR_ResolveStack( const vrSignals_t *s, vrStack_t *out );
int VR_ContextExclusive( vrContext_t context );
int VR_StackHas( const vrStack_t *stack, vrContext_t context );
/* Alt sets first while alt is held, then plain sets; each pass stops at an exclusive layer. Returns the layer or -1;
 * *altSet (may be NULL) says which set answered. */
int VR_ResolveKey( const vrStack_t *stack, int alt, vrKey_t key, vrLookup_t lookup, void *user, const char **binding,
				   int *altSet );
/* Whether a stick (VRK_MOVESTICK or VRK_TURNSTICK) is spoken for while alt is held: one of its directions has an Alt
 * binding in an active layer. Its deflection then stays out of moving and turning. */
int VR_StickTaken( const vrStack_t *stack, int alt, vrKey_t stick, vrLookup_t lookup, void *user );

void VR_HoldsInit( vrHolds_t *h );
int VR_UpdateHolds( vrHolds_t *h, const unsigned char now[VRK_COUNT], const vrStack_t *stack, vrLookup_t lookup,
					void *user, int timeMs, vrBindEvent_t *events, int maxEvents );
/* Releases every hold and latches every key until it is released. */
int VR_ReleaseAll( vrHolds_t *h, vrBindEvent_t *events, int maxEvents );
/* A new handedness or stick switch renames the keys under held buttons, so it releases and latches like ReleaseAll. */
int VR_HoldsSetMapping( vrHolds_t *h, int mapping, vrBindEvent_t *events, int maxEvents );
/* Whether an owned hold's binding runs command (one ';'-separated part, compared exactly). */
int VR_HoldsBound( const vrHolds_t *h, const char *command );
/* Bindings-menu capture: once every key of a gesture is let go, its key, or -1 meanwhile. The first key pressed wins
 * unless a deeper stage of the same control joined it (grip click over grip, stick click over that stick's direction). */
int VR_CaptureKey( vrHolds_t *h, const unsigned char now[VRK_COUNT] );

/* Whether a controller of the profile's family has the input behind key. */
int VR_KeyPresent( int profile, vrKey_t key );
const char *VR_KeyDisplayName( vrKey_t key, int profile, int rightHanded, int switchSticks, char *buf, int size );
/* Every default of the profile's family, the plain set then the Alt set. */
void VR_ForEachDefault( int profile,
						void ( *fn )( vrContext_t context, int alt, vrKey_t key, const char *binding, void *user ),
						void *user );
/* The index-th key the profile's defaults bind to command in context's plain or Alt set, or -1. */
int VR_DefaultKey( int profile, vrContext_t context, int alt, const char *command, int index );
/* The key that has to be Escape because no button this controller has is bound to it in global: its default Menu
 * button. -1 while one is. */
int VR_EscapeFallback( int profile, vrLookup_t lookup, void *user );
const char *VR_ContextName( vrContext_t context );
/* "<layer>" or "<layer>+alt", without regard to case. *alt says which set; a NULL alt refuses the suffix.
 * -1 when unknown. */
int VR_ContextFromName( const char *name, int *alt );
/* Maps cg_followMode to the VR follow mode; free-fly (2) exists only in TV playback. */
int VR_FollowModeFor( int followMode, int tvPlayback );
#endif
