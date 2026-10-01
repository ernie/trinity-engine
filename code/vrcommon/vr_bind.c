#include "vr_bind.h"
#include "vr_float.h"
#include "vr_safe_types.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int VR_ProfileFamily( int profile ) {
	switch ( profile ) {
		case CL_XRP_INDEX:
			return VRF_INDEX;
		case CL_XRP_FRAME:
			return VRF_FRAME;
		case CL_XRP_SIMPLE:
			return VRF_SIMPLE;
		default:
			return VRF_TOUCH;
	}
}

float VR_StickCurve( float value, float deadzone ) {
	float magnitude;
	if ( !VR_FloatFinite( value ) || !VR_FloatFinite( deadzone ) )
		return 0;
	magnitude = fabsf( value );
	if ( deadzone < 0 )
		deadzone = 0;
	if ( deadzone > .95f )
		deadzone = .95f;
	if ( magnitude <= deadzone )
		return 0;
	if ( magnitude > 1 )
		magnitude = 1;
	return (value < 0 ? -1 : 1) * (magnitude - deadzone) / (1 - deadzone);
}

static int VRB_Held( int was, float value, float press, float release ) {
	if ( !VR_FloatFinite( value ) )
		return 0;
	// A release at or below zero would hold the key forever once pressed
	if ( release < .05f )
		release = .05f;
	return was ? value >= release : value > press;
}

int VR_KeyPresent( int profile, vrKey_t key ) {
	int families;
	if ( key <= VRK_OFF_TRIGGER )
		families = VRF_PLAY | VRF_SIMPLE;
	else if ( key <= VRK_OFF_GRIP )
		families = VRF_PLAY;
	else if ( key <= VRK_OFF_GRIPCLICK )
		families = VRF_FRAME;
	else if ( key <= VRK_OFF_THUMBREST )
		families = VRF_TOUCH;
	else if ( key <= VRK_OFF_BUMPER )
		families = VRF_FRAME;
	else if ( key <= VRK_OFF_B )
		families = VRF_INDEX; /* trackpads and a second pair of face buttons */
	else if ( key <= VRK_TURNSTICK_RIGHT )
		families = VRF_PLAY;
	else if ( key <= VRK_Y )
		families = VRF_TOUCH | VRF_FRAME;
	else if ( key == VRK_MENU )
		families = VRF_TOUCH | VRF_FRAME | VRF_SIMPLE;
	else
		families = key < VRK_COUNT ? VRF_FRAME : 0; /* View and the D-pad */
	return (families & VR_ProfileFamily( profile )) != 0;
}

int VR_KeyHand( vrKey_t key, int rightHanded, int switchSticks ) {
	const int weapon = rightHanded ? 1 : 0, off = 1 - weapon;
	const int move = switchSticks ? weapon : off;
	if ( key <= VRK_OFF_B )
		return (key == VRK_WPN_A || key == VRK_WPN_B || (key <= VRK_WPN_TRACKPAD && !(key & 1))) ? weapon : off;
	if ( key == VRK_MOVESTICK || (key >= VRK_MOVESTICK_UP && key <= VRK_MOVESTICK_RIGHT) )
		return move;
	if ( key == VRK_TURNSTICK || (key >= VRK_TURNSTICK_UP && key <= VRK_TURNSTICK_RIGHT) )
		return 1 - move;
	return -1;
}

/* out[0..3] are up, down, left, right, the key enum's order. */
static void VRB_StickKeys( const float raw[2], const vrThresholds_t *t, const unsigned char *previous,
							unsigned char *out ) {
	float x = VR_StickCurve( raw[0], t->deadzone ), y = VR_StickCurve( raw[1], t->deadzone );
	float magnitude = sqrtf( x * x + y * y );
	int was = previous[0] || previous[1] || previous[2] || previous[3];
	if ( magnitude > (was ? t->stickRelease : t->stickPress) ) {
		if ( fabsf( y ) >= fabsf( x ) )
			out[y > 0 ? 0 : 1] = 1;
		else
			out[x > 0 ? 3 : 2] = 1;
	}
}

void VR_SampleKeys( const clXRHandInput_t hands[2], int rightHanded, int switchSticks, const vrThresholds_t *t,
					const unsigned char previous[VRK_COUNT], unsigned char out[VRK_COUNT] ) {
	const int weapon = rightHanded ? 1 : 0;
	const int move = VR_KeyHand( VRK_MOVESTICK, rightHanded, switchSticks );
	int h;
	memset( out, 0, VRK_COUNT );
	for ( h = 0; h < 2; h++ ) {
		const clXRHandInput_t *in = &hands[h];
		const int role = h == weapon ? 0 : 1; /* 0 = WPN key, 1 = OFF key */
		const int family = VR_ProfileFamily( in->profile );
		const int stick = h == move ? VRK_MOVESTICK : VRK_TURNSTICK;
		const int dirs = h == move ? VRK_MOVESTICK_UP : VRK_TURNSTICK_UP;
		if ( !in->active )
			continue;
		out[VRK_WPN_TRIGGER + role] =
			VRB_Held( previous[VRK_WPN_TRIGGER + role], in->trigger, t->triggerPress, t->triggerRelease );
		out[VRK_WPN_GRIP + role] = VRB_Held( previous[VRK_WPN_GRIP + role], in->squeeze, t->gripPress, t->gripRelease );
		out[VRK_WPN_GRIPCLICK + role] = (in->buttons & CL_XRI_SQUEEZE_CLICK_BUTTON) != 0;
		out[VRK_WPN_THUMBREST + role] = (in->buttons & CL_XRI_THUMBREST_BUTTON) != 0;
		out[VRK_WPN_BUMPER + role] = (in->buttons & CL_XRI_BUMPER_BUTTON) != 0;
		out[VRK_WPN_TRACKPAD + role] =
			VRB_Held( previous[VRK_WPN_TRACKPAD + role], in->trackpad, t->padPress, t->padRelease );
		out[stick] = (in->buttons & CL_XRI_STICK_BUTTON) != 0;
		VRB_StickKeys( in->stick, t, &previous[dirs], &out[dirs] );
		if ( family == VRF_INDEX ) {
			out[role ? VRK_OFF_A : VRK_WPN_A] = (in->buttons & CL_XRI_PRIMARY_BUTTON) != 0;
			out[role ? VRK_OFF_B : VRK_WPN_B] = (in->buttons & CL_XRI_SECONDARY_BUTTON) != 0;
		} else if ( family == VRF_SIMPLE ) {
			out[VRK_MENU] |= (in->buttons & CL_XRI_MENU_BUTTON) != 0;
		} else if ( h == 1 ) {
			out[VRK_A] = (in->buttons & CL_XRI_PRIMARY_BUTTON) != 0;
			out[VRK_B] = (in->buttons & CL_XRI_SECONDARY_BUTTON) != 0;
			if ( family == VRF_FRAME ) {
				out[VRK_X] = (in->buttons & CL_XRI_X_BUTTON) != 0;
				out[VRK_Y] = (in->buttons & CL_XRI_Y_BUTTON) != 0;
				out[VRK_MENU] |= (in->buttons & CL_XRI_MENU_BUTTON) != 0;
			}
		} else if ( family == VRF_FRAME ) {
			out[VRK_VIEW] = (in->buttons & CL_XRI_VIEW_BUTTON) != 0;
			out[VRK_DPAD_UP] = (in->buttons & CL_XRI_DPAD_UP_BUTTON) != 0;
			out[VRK_DPAD_DOWN] = (in->buttons & CL_XRI_DPAD_DOWN_BUTTON) != 0;
			out[VRK_DPAD_LEFT] = (in->buttons & CL_XRI_DPAD_LEFT_BUTTON) != 0;
			out[VRK_DPAD_RIGHT] = (in->buttons & CL_XRI_DPAD_RIGHT_BUTTON) != 0;
		} else {
			out[VRK_X] = (in->buttons & CL_XRI_PRIMARY_BUTTON) != 0;
			out[VRK_Y] = (in->buttons & CL_XRI_SECONDARY_BUTTON) != 0;
			out[VRK_MENU] |= (in->buttons & CL_XRI_MENU_BUTTON) != 0;
		}
	}
}

void VR_RoleSticks( const clXRHandInput_t hands[2], int rightHanded, int switchSticks, float deadzone, float move[2],
					float turn[2] ) {
	const int m = VR_KeyHand( VRK_MOVESTICK, rightHanded, switchSticks );
	move[0] = VR_StickCurve( hands[m].stick[0], deadzone );
	move[1] = VR_StickCurve( hands[m].stick[1], deadzone );
	turn[0] = VR_StickCurve( hands[1 - m].stick[0], deadzone );
	turn[1] = VR_StickCurve( hands[1 - m].stick[1], deadzone );
}

static const char *contextNames[VRC_COUNT] = {"global", "menu",	 "adjust", "scrub",	 "scoreboard",
											   "vote",	 "wheel", "follow", "gameplay"};

const char *VR_ContextName( vrContext_t context ) {
	return context >= 0 && context < VRC_COUNT ? contextNames[context] : "";
}

/* Console input; matches key names, which are case-insensitive. */
static int VRB_NameEquals( const char *a, const char *b ) {
	while ( *a && tolower( (unsigned char)*a ) == tolower( (unsigned char)*b ) ) {
		a++;
		b++;
	}
	return tolower( (unsigned char)*a ) == tolower( (unsigned char)*b );
}

int VR_ContextFromName( const char *name, int *alt ) {
	char layer[16];
	const char *plus = name ? strchr( name, '+' ) : NULL;
	size_t length = name ? ( plus ? (size_t)( plus - name ) : strlen( name ) ) : 0;
	int i;
	if ( !name || length == 0 || length >= sizeof( layer ) || ( plus && ( !alt || !VRB_NameEquals( plus, "+alt" ) ) ) )
		return -1;
	memcpy( layer, name, length );
	layer[length] = 0;
	for ( i = 0; i < VRC_COUNT; i++ )
		if ( VRB_NameEquals( layer, contextNames[i] ) ) {
			if ( alt )
				*alt = plus != NULL;
			return i;
		}
	return -1;
}

int VR_ContextExclusive( vrContext_t context ) {
	return context == VRC_MENU || context == VRC_ADJUST || context == VRC_SCRUB;
}

int VR_StackHas( const vrStack_t *stack, vrContext_t context ) {
	int i;
	for ( i = 0; i < stack->count; i++ )
		if ( stack->layers[i] == context )
			return 1;
	return 0;
}

void VR_ResolveStack( const vrSignals_t *s, vrStack_t *out ) {
	const int flags[] = {s->textEntry || s->menu || s->offline, s->adjust, s->scrub, s->scoreboard, s->vote, s->wheel};
	const vrContext_t layers[] = {VRC_MENU, VRC_ADJUST, VRC_SCRUB, VRC_SCOREBOARD, VRC_VOTE, VRC_WHEEL};
	unsigned i;
	out->count = 0;
	out->layers[out->count++] = VRC_GLOBAL;
	for ( i = 0; i < sizeof( layers ) / sizeof( layers[0] ); i++ )
		if ( flags[i] )
			out->layers[out->count++] = layers[i];
	if ( s->tv || s->demo || s->following )
		out->layers[out->count++] = VRC_FOLLOW;
	out->layers[out->count++] = VRC_GAMEPLAY;
	out->base = s->offline			? VRB_OFFLINE
				: s->tv				? VRB_TV
				: s->demo			? VRB_DEMO
				: s->following		? VRB_FOLLOWING
				: s->intermission	? VRB_INTERMISSION
				: s->dead			? VRB_DEAD
				: s->spectating		? VRB_SPECTATING
									: VRB_PLAYING;
}

static int VRB_Pass( const vrStack_t *stack, int alt, vrKey_t key, vrLookup_t lookup, void *user,
					 const char **binding ) {
	int i;
	for ( i = 0; i < stack->count; i++ ) {
		const char *b = lookup( stack->layers[i], alt, key, user );
		if ( b && b[0] ) {
			*binding = b;
			return stack->layers[i];
		}
		if ( VR_ContextExclusive( stack->layers[i] ) )
			return -1;
	}
	return -1;
}

int VR_ResolveKey( const vrStack_t *stack, int alt, vrKey_t key, vrLookup_t lookup, void *user, const char **binding,
				   int *altSet ) {
	int layer = -1;
	*binding = NULL;
	/* Alt + a button is its own button: an Alt binding anywhere active beats every plain one */
	if ( alt )
		layer = VRB_Pass( stack, 1, key, lookup, user, binding );
	if ( altSet )
		*altSet = layer >= 0;
	return layer >= 0 ? layer : VRB_Pass( stack, 0, key, lookup, user, binding );
}

int VR_StickTaken( const vrStack_t *stack, int alt, vrKey_t stick, vrLookup_t lookup, void *user ) {
	const int first = stick == VRK_MOVESTICK ? VRK_MOVESTICK_UP : VRK_TURNSTICK_UP;
	const char *binding;
	int i;
	/* the weapon wheel picks with a stick's deflection, so it keeps the stick while it is up */
	if ( !alt || VR_StackHas( stack, VRC_WHEEL ) )
		return 0;
	for ( i = 0; i < 4; i++ )
		if ( VRB_Pass( stack, 1, (vrKey_t)( first + i ), lookup, user, &binding ) >= 0 )
			return 1;
	return 0;
}

void VR_HoldsInit( vrHolds_t *h ) {
	memset( h, 0, sizeof( *h ) );
	memset( h->owner, -1, sizeof( h->owner ) );
	h->base = h->mapping = h->gestureFirst = -1;
}

static int VRB_Emit( vrBindEvent_t *events, int n, int maxEvents, vrBindEventType_t type, vrKey_t key, int layer,
					  const char *binding ) {
	if ( n >= maxEvents )
		return n;
	events[n].type = type;
	events[n].key = key;
	events[n].layer = (vrContext_t)layer;
	snprintf( events[n].binding, sizeof( events[n].binding ), "%s", binding );
	return n + 1;
}

int VR_ReleaseAll( vrHolds_t *h, vrBindEvent_t *events, int maxEvents ) {
	int k, n = 0;
	for ( k = 0; k < VRK_COUNT; k++ ) {
		if ( h->owner[k] >= 0 )
			n = VRB_Emit( events, n, maxEvents, VRE_RELEASE, (vrKey_t)k, h->owner[k], h->binding[k] );
		h->owner[k] = -1;
		/* Treated as down, so a button still held when input returns waits for its release. */
		h->down[k] = 1;
		h->gesture[k] = 0;
	}
	h->gestureFirst = -1;
	return n;
}

int VR_HoldsSetMapping( vrHolds_t *h, int mapping, vrBindEvent_t *events, int maxEvents ) {
	const int previous = h->mapping;
	h->mapping = mapping;
	return previous >= 0 && previous != mapping ? VR_ReleaseAll( h, events, maxEvents ) : 0;
}

static int VRB_Repeats( const char *binding ) {
	return !strncmp( binding, "+menunav", 8 );
}

/* Whether binding runs command as one of its ';'-separated parts. */
static int VRB_Runs( const char *binding, const char *command ) {
	const size_t length = strlen( command );
	const char *p = binding;
	while ( *p ) {
		while ( *p == ' ' || *p == ';' )
			p++;
		if ( !strncmp( p, command, length ) && ( p[length] == 0 || p[length] == ';' || p[length] == ' ' ) )
			return 1;
		while ( *p && *p != ';' )
			p++;
	}
	return 0;
}

int VR_HoldsBound( const vrHolds_t *h, const char *command ) {
	int k;
	for ( k = 0; k < VRK_COUNT; k++ )
		if ( h->owner[k] >= 0 && VRB_Runs( h->binding[k], command ) )
			return 1;
	return 0;
}

int VR_UpdateHolds( vrHolds_t *h, const unsigned char now[VRK_COUNT], const vrStack_t *stack, vrLookup_t lookup,
					void *user, int timeMs, vrBindEvent_t *events, int maxEvents ) {
	int k, n = 0, stage;
	if ( h->base != (int)stack->base ) {
		if ( h->base >= 0 )
			for ( k = 0; k < VRK_COUNT; k++ ) {
				if ( h->owner[k] >= 0 )
					n = VRB_Emit( events, n, maxEvents, VRE_RELEASE, (vrKey_t)k, h->owner[k], h->binding[k] );
				h->owner[k] = -1;
			}
		h->base = stack->base;
	}
	for ( stage = 0; stage < 2; stage++ ) {
		/* Alt buttons go first, so a button pressed on the same frame as one sees Alt held */
		const int alt = VR_HoldsBound( h, "+alt" );
		for ( k = 0; k < VRK_COUNT; k++ ) {
			if ( now[k] && !h->down[k] ) {
				const char *binding;
				int layer = VR_ResolveKey( stack, alt, (vrKey_t)k, lookup, user, &binding, NULL );
				if ( ( layer >= 0 && VRB_Runs( binding, "+alt" ) ) != ( stage == 0 ) )
					continue;
				if ( layer >= 0 ) {
					h->owner[k] = (signed char)layer;
					snprintf( h->binding[k], sizeof( h->binding[k] ), "%s", binding );
					h->repeatAt[k] = timeMs + 400;
					n = VRB_Emit( events, n, maxEvents, VRE_PRESS, (vrKey_t)k, layer, binding );
				}
				h->down[k] = 1;
			} else if ( !now[k] && h->down[k] ) {
				if ( ( h->owner[k] >= 0 && VRB_Runs( h->binding[k], "+alt" ) ) != ( stage == 0 ) )
					continue;
				if ( h->owner[k] >= 0 )
					n = VRB_Emit( events, n, maxEvents, VRE_RELEASE, (vrKey_t)k, h->owner[k], h->binding[k] );
				h->owner[k] = -1;
				h->down[k] = 0;
			} else if ( stage && now[k] && h->owner[k] >= 0 && VRB_Repeats( h->binding[k] ) && timeMs >= h->repeatAt[k] ) {
				h->repeatAt[k] = timeMs + 140;
				n = VRB_Emit( events, n, maxEvents, VRE_PRESS, (vrKey_t)k, h->owner[k], h->binding[k] );
			}
		}
	}
	return n;
}

int VR_CaptureKey( vrHolds_t *h, const unsigned char now[VRK_COUNT] ) {
	int k, held = 0, key;
	for ( k = 0; k < VRK_COUNT; k++ ) {
		if ( now[k] && !h->down[k] ) {
			h->gesture[k] = 1;
			if ( h->gestureFirst < 0 )
				h->gestureFirst = k;
		}
		h->down[k] = now[k];
		held |= now[k] && h->gesture[k];
	}
	if ( held || h->gestureFirst < 0 )
		return -1;
	key = h->gestureFirst;
	if ( (key == VRK_WPN_GRIP || key == VRK_OFF_GRIP) && h->gesture[key + VRK_WPN_GRIPCLICK - VRK_WPN_GRIP] )
		key += VRK_WPN_GRIPCLICK - VRK_WPN_GRIP;
	else if ( key >= VRK_MOVESTICK_UP && key <= VRK_MOVESTICK_RIGHT && h->gesture[VRK_MOVESTICK] )
		key = VRK_MOVESTICK;
	else if ( key >= VRK_TURNSTICK_UP && key <= VRK_TURNSTICK_RIGHT && h->gesture[VRK_TURNSTICK] )
		key = VRK_TURNSTICK;
	memset( h->gesture, 0, sizeof( h->gesture ) );
	h->gestureFirst = -1;
	return key;
}

/* Console-pad style, one letter per word with the hand first (LT, RGC, LTR); directions are the character sheet's
 * arrows the on-screen keyboard draws (135 up, 134 down, 136 left, 141 right); one-hand buttons keep their label. */
const char *VR_KeyDisplayName( vrKey_t key, int profile, int rightHanded, int switchSticks, char *buf, int size ) {
	static const char *roleLabels[] = {"T", "G", "GC", "TR", "B", "TP"};
	static const char *dirLabels[] = {"\x87", "\x86", "\x88", "\x8d"};
	static const char *oneHand[] = {"A", "B", "X", "Y", "Menu", "View", "\x87", "\x86", "\x88", "\x8d"};
	const int hand = VR_KeyHand( key, rightHanded, switchSticks );
	const char side = hand == 1 ? 'R' : 'L';
	(void)profile;
	if ( key <= VRK_OFF_TRACKPAD )
		snprintf( buf, size, "%c%s", side, roleLabels[key / 2] );
	else if ( key <= VRK_OFF_B )
		snprintf( buf, size, "%c%s", side, key == VRK_WPN_A || key == VRK_OFF_A ? "A" : "B" );
	else if ( key == VRK_MOVESTICK || key == VRK_TURNSTICK )
		snprintf( buf, size, "%cS", side );
	else if ( key <= VRK_TURNSTICK_RIGHT )
		snprintf( buf, size, "%cS%s", side, dirLabels[(key - VRK_MOVESTICK_UP) % 4] );
	else if ( key < VRK_COUNT )
		snprintf( buf, size, "%s", oneHand[key - VRK_A] );
	else
		snprintf( buf, size, "?" );
	return buf;
}

typedef struct {
	int families;
	vrContext_t context;
	vrKey_t key;
	const char *binding;
} vrDefault_t;

#define T VRF_TOUCH
#define I VRF_INDEX
#define F VRF_FRAME
#define S VRF_SIMPLE
#define P VRF_PLAY
#define ALL ( VRF_PLAY | VRF_SIMPLE )
#define NAV( fam, ctx ) \
	{fam, ctx, VRK_MOVESTICK_UP, "+menunav up"}, {fam, ctx, VRK_MOVESTICK_DOWN, "+menunav down"}, \
	{fam, ctx, VRK_MOVESTICK_LEFT, "+menunav left"}, {fam, ctx, VRK_MOVESTICK_RIGHT, "+menunav right"}, \
	{fam, ctx, VRK_TURNSTICK_UP, "+menunav up"}, {fam, ctx, VRK_TURNSTICK_DOWN, "+menunav down"}, \
	{fam, ctx, VRK_TURNSTICK_LEFT, "+menunav left"}, {fam, ctx, VRK_TURNSTICK_RIGHT, "+menunav right"}, \
	{F, ctx, VRK_DPAD_UP, "+menunav up"}, {F, ctx, VRK_DPAD_DOWN, "+menunav down"}, \
	{F, ctx, VRK_DPAD_LEFT, "+menunav left"}, {F, ctx, VRK_DPAD_RIGHT, "+menunav right"}

static const vrDefault_t defaultBindings[] = {
	/* gameplay */
	{P, VRC_GAMEPLAY, VRK_WPN_TRIGGER, "+attack"},
	{P, VRC_GAMEPLAY, VRK_OFF_TRIGGER, "+moveup"},
	{P, VRC_GAMEPLAY, VRK_WPN_GRIP, "+weapon_select"},
	{P, VRC_GAMEPLAY, VRK_OFF_GRIP, "+weapon_stabilise"},
	{P, VRC_GAMEPLAY, VRK_MOVESTICK, "+scores"},
	{P, VRC_GAMEPLAY, VRK_TURNSTICK, "+voiprecord"},
	{P, VRC_GAMEPLAY, VRK_TURNSTICK_LEFT, "turnleft"},
	{P, VRC_GAMEPLAY, VRK_TURNSTICK_RIGHT, "turnright"},
	{T, VRC_GAMEPLAY, VRK_B, "+moveup"},
	{T, VRC_GAMEPLAY, VRK_A, "+movedown"},
	{T, VRC_GAMEPLAY, VRK_X, "+button2"},
	{T, VRC_GAMEPLAY, VRK_Y, "+button3"},
	{I, VRC_GAMEPLAY, VRK_WPN_B, "+moveup"},
	{I, VRC_GAMEPLAY, VRK_WPN_A, "+movedown"},
	{I, VRC_GAMEPLAY, VRK_OFF_A, "+button2"},
	{F, VRC_GAMEPLAY, VRK_X, "+moveup"},
	{F, VRC_GAMEPLAY, VRK_A, "+movedown"},
	{F, VRC_GAMEPLAY, VRK_Y, "+button2"},
	{F, VRC_GAMEPLAY, VRK_B, "+button3"},
	/* global */
	{T, VRC_GLOBAL, VRK_WPN_THUMBREST, "+alt"},
	{T, VRC_GLOBAL, VRK_OFF_THUMBREST, "+alt"},
	{F, VRC_GLOBAL, VRK_WPN_BUMPER, "+alt"},
	{F, VRC_GLOBAL, VRK_OFF_BUMPER, "+alt"},
	{T | F | S, VRC_GLOBAL, VRK_MENU, "+key ESCAPE"},
	{I, VRC_GLOBAL, VRK_OFF_B, "+key ESCAPE"},
	{F, VRC_GLOBAL, VRK_VIEW, "toggleconsole"},
	/* menu, console and text entry */
	{ALL, VRC_MENU, VRK_WPN_TRIGGER, "+vr_click"},
	{ALL, VRC_MENU, VRK_OFF_TRIGGER, "+vr_click"},
	NAV( P, VRC_MENU ),
	{T | F, VRC_MENU, VRK_A, "+key SPACE"},
	{I, VRC_MENU, VRK_WPN_A, "+key SPACE"},
	/* weapon adjust */
	{T | F, VRC_ADJUST, VRK_A, "weapon_adjust"},
	{T | F, VRC_ADJUST, VRK_B, "+adjust_reset"},
	{I, VRC_ADJUST, VRK_WPN_A, "weapon_adjust"},
	{I, VRC_ADJUST, VRK_WPN_B, "+adjust_reset"},
	/* TV scrub, scoreboard, vote */
	{P, VRC_SCRUB, VRK_OFF_GRIP, "tv_scrub_cancel"},
	{P, VRC_SCOREBOARD, VRK_WPN_TRIGGER, "+vr_click"},
	{P, VRC_SCOREBOARD, VRK_OFF_TRIGGER, "+vr_click"},
	{T | F, VRC_VOTE, VRK_A, "+vote_yes"},
	{T | F, VRC_VOTE, VRK_B, "+vote_no"},
	{I, VRC_VOTE, VRK_WPN_A, "+vote_yes"},
	{I, VRC_VOTE, VRK_WPN_B, "+vote_no"},
	/* following, demos and TV playback */
	{P, VRC_FOLLOW, VRK_WPN_TRIGGER, "follownext"},
	{P, VRC_FOLLOW, VRK_OFF_TRIGGER, "followprev"},
	{T | F, VRC_FOLLOW, VRK_A, "follow"},
	{I, VRC_FOLLOW, VRK_WPN_A, "follow"},
	{T | F, VRC_FOLLOW, VRK_X, "followcam"},
	{I, VRC_FOLLOW, VRK_OFF_A, "followcam"},
	{T | F, VRC_FOLLOW, VRK_B, "followrecenter"},
	{I, VRC_FOLLOW, VRK_WPN_B, "followrecenter"},
	{P, VRC_FOLLOW, VRK_TURNSTICK, "demopause"},
	{P, VRC_FOLLOW, VRK_WPN_GRIP, "+tv_scrub"},
};
/* The Alt set: what the same keys do while +alt is held. */
static const vrDefault_t defaultAltBindings[] = {
	{P, VRC_GAMEPLAY, VRK_TURNSTICK, "voiptarget"},
};
static const struct {
	const vrDefault_t *rows;
	size_t count;
} defaultSets[2] = {{defaultBindings, sizeof( defaultBindings ) / sizeof( defaultBindings[0] )},
					{defaultAltBindings, sizeof( defaultAltBindings ) / sizeof( defaultAltBindings[0] )}};
#undef T
#undef I
#undef F
#undef S
#undef P
#undef ALL
#undef NAV

void VR_ForEachDefault( int profile,
						void ( *fn )( vrContext_t context, int alt, vrKey_t key, const char *binding, void *user ),
						void *user ) {
	const int family = VR_ProfileFamily( profile );
	unsigned i;
	int alt;
	for ( alt = 0; alt < 2; alt++ )
		for ( i = 0; i < defaultSets[alt].count; i++ ) {
			const vrDefault_t *d = &defaultSets[alt].rows[i];
			if ( d->families & family )
				fn( d->context, alt, d->key, d->binding, user );
		}
}

/* Bindings compare as the engine's own lookups do, ignoring case. */
static int VRB_SameCommand( const char *a, const char *b ) {
	while ( *a && tolower( (unsigned char)*a ) == tolower( (unsigned char)*b ) ) {
		a++;
		b++;
	}
	return tolower( (unsigned char)*a ) == tolower( (unsigned char)*b );
}

int VR_DefaultKey( int profile, vrContext_t context, int alt, const char *command, int index ) {
	const int family = VR_ProfileFamily( profile );
	unsigned i;
	for ( i = 0; i < defaultSets[alt ? 1 : 0].count; i++ ) {
		const vrDefault_t *d = &defaultSets[alt ? 1 : 0].rows[i];
		if ( (d->families & family) && d->context == context && VRB_SameCommand( d->binding, command ) && !index-- )
			return d->key;
	}
	return -1;
}

int VR_EscapeFallback( int profile, vrLookup_t lookup, void *user ) {
	int k;
	for ( k = 0; k < VRK_COUNT; k++ ) {
		const char *binding = lookup( VRC_GLOBAL, 0, (vrKey_t)k, user );
		if ( binding && VR_KeyPresent( profile, (vrKey_t)k ) && VRB_SameCommand( binding, "+key ESCAPE" ) )
			return -1;
	}
	return VR_DefaultKey( profile, VRC_GLOBAL, 0, "+key ESCAPE", 0 );
}

int VR_FollowModeFor( int followMode, int tvPlayback ) {
	if ( followMode == 1 )
		return VRFM_THIRDPERSON_1;
	if ( followMode == 2 && tvPlayback )
		return VRFM_THIRDPERSON_2;
	return VRFM_FIRSTPERSON;
}
