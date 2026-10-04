// cl_keyboard.c -- on-screen virtual keyboard for VR

#include "client.h"
#include "cl_vr_input.h"
#include "cl_vkb_layout.h"
#include "../vrcommon/vr_clientinfo.h"
#include "../vrcommon/vr_shared.h"

extern vr_clientinfo_t vr;

#define VKB_FONT_POINT		24		// fonts/fontImage_24.dat, a size Team Arena's menus never register
#define VKB_LABEL_PX		16
#define VKB_SMALL_PX		10
#define VKB_ICON_PX			19		// the icons' ink spans 9/16 of the cell, which puts it at the font's capital height
#define VKB_CAP_CORNER		6		// virtual pixels for the cap image's quarter-size corner slice
#define VKB_PANEL_CORNER	8
#define VKB_GLOW_CORNER		12
#define VKB_GLOW_SPREAD		8
#define VKB_PRESS_DROP		2
#define VKB_ICON_CELLS		8

#define KEY_REPEAT_DELAY	500		// milliseconds
#define KEY_REPEAT_RATE		80		// milliseconds, ~12.5 repeats/sec

// Hand indices for repeat state ownership
#define VKB_HAND_PRIMARY	0
#define VKB_HAND_OFFHAND	1

// icons.tga cells, left to right
#define ICON_BACKSPACE	0
#define ICON_ENTER		1
#define ICON_TAB		2
#define ICON_LEFT		3
#define ICON_RIGHT		4
#define ICON_UP			5
#define ICON_DOWN		6

typedef struct {
	qboolean	active;
	vkbMods_t	mods;
	sfxHandle_t	clickSound;
	const vkbKey_t *lastHover[2];	// per hand, for the hover tick
	// Last-key-wins: only one hand repeats at a time
	const vkbKey_t *repeatKey;
	int			repeatChar;			// resolved at press; 0 for keys that send a key event
	int			repeatAction;		// resolved at press
	int			repeatPressTime;	// cls.realtime
	int			repeatLastTime;		// cls.realtime
	qboolean	repeatStarted;		// past initial delay?
	int			repeatHand;			// VKB_HAND_PRIMARY or VKB_HAND_OFFHAND
} vKeyboardState_t;

static vKeyboardState_t vkb;

typedef struct {
	qboolean	loaded;			// registered against the current renderer
	fontInfo_t	font;			// glyphScale stays 0 when the pak has no font
	qhandle_t	cap, panel, glow, icons;	// 0 when the pak lacks the image
} vkbAssets_t;

static vkbAssets_t assets;

// Dark slate keys under light text; the cap image is near white, so these tints are the keys' colors
static const vec4_t capPlain = { 0.24f, 0.25f, 0.29f, 1 };
static const vec4_t capSpecial = { 0.18f, 0.19f, 0.22f, 1 };
static const vec4_t capActive = { 0.30f, 0.17f, 0.17f, 1 };
static const vec4_t ember = { 1.0f, 0.19f, 0.16f, 0.55f };			// the right hand's pointer red
static const vec4_t azure = { 0.30f, 0.50f, 1.0f, 0.55f };			// the left hand's pointer blue
static const vec4_t violet = { 0.72f, 0.30f, 1.0f, 0.55f };			// both pointers on one key
static const vec4_t emberUnderline = { 1.0f, 0.19f, 0.16f, 0.8f };
static const vec4_t textPlain = { 0.93f, 0.93f, 0.95f, 1 };
static const vec4_t textSpecial = { 0.70f, 0.72f, 0.78f, 1 };
static const vec4_t panelFlat = { 0.1f, 0.1f, 0.12f, 0.95f };

static void VKeyboard_FireAction( int ch, int action );
static void VKeyboard_ProcessKeyPress( const vkbKey_t *key, int handIndex );

/*
=================
Assets
=================
*/

/* Registered on the first draw after a renderer start, so a vid_restart never leaves stale handles behind. */
static void VKeyboard_LoadAssets( void ) {
	Com_Memset( &assets, 0, sizeof( assets ) );
	// FS_FileExists sees only loose files; the paks carry the font, so ask the search path for its length
	if ( FS_FOpenFileRead( va( "fonts/fontImage_%i.dat", VKB_FONT_POINT ), NULL, qfalse ) > 0 )
		re.RegisterFont( "fonts/rajdhani", VKB_FONT_POINT, &assets.font );
	assets.cap = re.RegisterShaderNoMip( "gfx/vkb/cap" );
	assets.panel = re.RegisterShaderNoMip( "gfx/vkb/panel" );
	assets.glow = re.RegisterShaderNoMip( "gfx/vkb/glow" );
	assets.icons = re.RegisterShaderNoMip( "gfx/vkb/icons" );
	assets.loaded = qtrue;
}

void VKeyboard_RendererStarted( void ) {
	assets.loaded = qfalse;
}

/*
=================
Drawing helpers
=================
*/

/* Nine quads from one image whose corner slice is a quarter of its size. */
static void VKB_Draw9( qhandle_t shader, float x, float y, float w, float h, float corner, const float *color ) {
	const float xs[4] = { x, x + corner, x + w - corner, x + w };
	const float ys[4] = { y, y + corner, y + h - corner, y + h };
	const float uv[4] = { 0, 0.25f, 0.75f, 1 };
	int row, col;
	re.SetColor( color );
	for ( row = 0; row < 3; row++ ) {
		for ( col = 0; col < 3; col++ ) {
			float qx = xs[col], qy = ys[row], qw = xs[col + 1] - xs[col], qh = ys[row + 1] - ys[row];
			SCR_AdjustFrom640( &qx, &qy, &qw, &qh );
			re.DrawStretchPic( qx, qy, qw, qh, uv[col], uv[row], uv[col + 1], uv[row + 1], shader );
		}
	}
	re.SetColor( NULL );
}

/* Team Arena's painter: a text scale of 1 is a 48 point em, times the font's own glyphScale */
static float VKB_FontScale( float pixelHeight ) {
	return pixelHeight / 48.0f * assets.font.glyphScale;
}

static float VKB_TextWidth( const char *s, float scale ) {
	float width = 0;
	for ( ; *s; s++ )
		width += assets.font.glyphs[(unsigned char)*s].xSkip * scale;
	return width;
}

/* The cap height sits on the center for every string, so a label keeps its baseline when Shift or Caps swaps it. */
static void VKB_DrawText( const char *s, float cx, float cy, float pixelHeight, const float *color ) {
	const float scale = VKB_FontScale( pixelHeight );
	const float baseline = cy + assets.font.glyphs['H'].top * scale / 2;
	float x = cx - VKB_TextWidth( s, scale ) / 2;
	re.SetColor( color );
	for ( ; *s; s++ ) {
		const glyphInfo_t *g = &assets.font.glyphs[(unsigned char)*s];
		float gx = x, gy = baseline - g->top * scale, gw = g->imageWidth * scale, gh = g->imageHeight * scale;
		if ( g->glyph && gw > 0 ) {
			SCR_AdjustFrom640( &gx, &gy, &gw, &gh );
			re.DrawStretchPic( gx, gy, gw, gh, g->s, g->t, g->s2, g->t2, g->glyph );
		}
		x += g->xSkip * scale;
	}
	re.SetColor( NULL );
}

static void VKB_DrawIconCell( int cell, float cx, float cy, float size, const float *color ) {
	float x = cx - size / 2, y = cy - size / 2, w = size, h = size;
	re.SetColor( color );
	SCR_AdjustFrom640( &x, &y, &w, &h );
	re.DrawStretchPic( x, y, w, h, (float)cell / VKB_ICON_CELLS, 0, (float)( cell + 1 ) / VKB_ICON_CELLS, 1, assets.icons );
	re.SetColor( NULL );
}

/* Without the icon strip, the arrows, backspace, enter and tab are built from character-set glyphs. */
static void VKeyboard_DrawGlyph( int ch, int x, int y, int size, const float *color ) {
	char str[2] = { (char)ch, '\0' };
	SCR_DrawStringExt( x, y, size, str, color, qtrue, qtrue );
}

static void VKeyboard_DrawFallbackIcon( int cell, const vkbRect_t *r, const float *color ) {
	const int arrow = 18, line = 14, overlap = 4;
	const int cx = r->x + r->w / 2, cy = r->y + r->h / 2;
	int startX;
	switch ( cell ) {
		case ICON_LEFT:
			VKeyboard_DrawGlyph( 136, cx - 5, cy - 5, 10, color );
			break;
		case ICON_RIGHT:
			VKeyboard_DrawGlyph( 141, cx - 5, cy - 5, 10, color );
			break;
		case ICON_UP:
			VKeyboard_DrawGlyph( 135, cx - 5, cy - 5, 10, color );
			break;
		case ICON_DOWN:
			VKeyboard_DrawGlyph( 134, cx - 5, cy - 5, 10, color );
			break;
		case ICON_BACKSPACE:
			startX = cx - ( arrow + line - overlap ) / 2 - 2;
			VKeyboard_DrawGlyph( 136, startX, cy - arrow / 2, arrow, color );
			VKeyboard_DrawGlyph( 30, startX + arrow - overlap, cy - line / 2, line, color );
			break;
		case ICON_ENTER:
			startX = cx - ( line + arrow - overlap ) / 2 + 2;
			VKeyboard_DrawGlyph( 30, startX, cy - line / 2, line, color );
			VKeyboard_DrawGlyph( 141, startX + line - overlap, cy - arrow / 2, arrow, color );
			break;
		case ICON_TAB:
			startX = cx - ( line + arrow + line / 3 - overlap * 2 ) / 2;
			VKeyboard_DrawGlyph( 30, startX, cy - line / 2, line, color );
			VKeyboard_DrawGlyph( 141, startX + line - overlap, cy - arrow / 2, arrow, color );
			VKeyboard_DrawGlyph( 21, startX + line + arrow - overlap * 2, cy - line / 2, line, color );
			break;
	}
}

/*
=================
Show, hide, state
=================
*/
void VKeyboard_Show( void ) {
	vkb.active = qtrue;
	VKB_ModsReset( &vkb.mods );
	vkb.lastHover[VKB_HAND_PRIMARY] = vkb.lastHover[VKB_HAND_OFFHAND] = NULL;
	vkb.repeatKey = NULL;
	if ( !vkb.clickSound ) {
		vkb.clickSound = S_RegisterSound( "sound/misc/click.wav", qfalse );
	}
}

void VKeyboard_Hide( void ) {
	vkb.active = qfalse;
	vkb.repeatKey = NULL;
	vkb.clickSound = 0;	// stale across sound-subsystem reset (mod switch, snd_restart)
	vr.vkbOffhandTriggerDown = qfalse;
}

qboolean VKeyboard_IsActive( void ) {
	return vkb.active;
}

/*
=================
Drawing
=================
*/
static int VKeyboard_IconCell( const vkbKey_t *key ) {
	switch ( key->action ) {
		case VKB_BACKSPACE: return ICON_BACKSPACE;
		case VKB_ENTER: return ICON_ENTER;
		case VKB_TAB: return ICON_TAB;
		case VKB_LEFT: return ICON_LEFT;
		case VKB_RIGHT: return ICON_RIGHT;
		case VKB_UP: return ICON_UP;
		case VKB_DOWN: return ICON_DOWN;
		default: return -1;
	}
}

static void VKeyboard_DrawCap( const vkbKey_t *key, const vkbRect_t *r, qboolean hovered, qboolean pressed, qboolean active ) {
	const float *base = active ? capActive : key->action == VKB_CHAR ? capPlain : capSpecial;
	const float gain = pressed ? 0.75f : hovered ? 1.35f : 1.0f;
	const int drop = pressed ? VKB_PRESS_DROP : 0;
	vec4_t tint;
	int i;
	for ( i = 0; i < 3; i++ )
		tint[i] = Com_Clamp( 0, 1, base[i] * gain );
	tint[3] = 1;
	if ( !assets.cap ) {
		SCR_FillRect( r->x, r->y + drop, r->w, r->h, tint );		// no cap art: a flat key in the same color
		return;
	}
	VKB_Draw9( assets.cap, r->x, r->y + drop, r->w, r->h, VKB_CAP_CORNER, tint );
	if ( active && assets.glow )
		VKB_Draw9( assets.glow, r->x - 2, r->y + r->h - 8, r->w + 4, 14, VKB_GLOW_CORNER / 2, emberUnderline );
}

static void VKeyboard_DrawGlow( const vkbRect_t *r, const float *color ) {
	if ( assets.cap && assets.glow )
		VKB_Draw9( assets.glow, r->x - VKB_GLOW_SPREAD, r->y - VKB_GLOW_SPREAD,
			r->w + 2 * VKB_GLOW_SPREAD, r->h + 2 * VKB_GLOW_SPREAD, VKB_GLOW_CORNER, color );
}

static void VKeyboard_DrawLabel( const vkbKey_t *key, const vkbRect_t *r, qboolean hovered, qboolean pressed, qboolean active ) {
	const float cx = r->x + r->w / 2.0f, cy = r->y + r->h / 2.0f + ( pressed ? VKB_PRESS_DROP : 0 );
	const float *color = ( hovered || active ) ? colorWhite : key->action == VKB_CHAR ? textPlain : textSpecial;
	const int cell = VKeyboard_IconCell( key );
	char str[2] = { 0, 0 };

	if ( cell >= 0 ) {
		// Enter, Backspace and Tab sit on wide keys and read better half again as large as the arrows
		const float iconPx = ( cell == ICON_ENTER || cell == ICON_BACKSPACE || cell == ICON_TAB ) ? VKB_ICON_PX * 1.5f : VKB_ICON_PX;
		if ( assets.icons )
			VKB_DrawIconCell( cell, cx, cy, iconPx, color );
		else
			VKeyboard_DrawFallbackIcon( cell, r, color );
		return;
	}
	if ( key->label ) {
		if ( assets.font.glyphScale ) {
			// Capital-letter size where the key is wide enough; the one-unit navigation keys take the smaller size
			const float px = VKB_TextWidth( key->label, VKB_FontScale( VKB_LABEL_PX ) ) <= r->w - 8 ? VKB_LABEL_PX : VKB_SMALL_PX;
			VKB_DrawText( key->label, cx, cy, px, color );
		} else {
			const int cw = strlen( key->label ) > 3 ? 8 : 10;	// a four-letter label at 10 would overhang a one-unit key
			SCR_DrawStringExt( (int)cx - (int)strlen( key->label ) * cw / 2, (int)cy - cw / 2, cw, key->label, color, qtrue, qfalse );
		}
		return;
	}
	str[0] = VKB_Glyph( key, &vkb.mods );
	if ( str[0] <= ' ' )
		return;
	if ( !assets.font.glyphScale ) {
		SCR_DrawStringExt( (int)cx - 5, (int)cy - 5, 10, str, color, qtrue, qfalse );
		return;
	}
	VKB_DrawText( str, cx, cy, VKB_LABEL_PX, color );
}

void VKeyboard_Draw( void ) {
	const vkbKey_t *hoverKey, *offhandHoverKey;
	vkbRect_t panel;
	int cursorX, cursorY, offhandCursorX, offhandCursorY;
	int pass, row, i;

	if ( !vkb.active ) {
		return;
	}
	if ( !assets.loaded ) {
		VKeyboard_LoadAssets();
	}

	if ( vr.menuCursorActive ) {
		cursorX = vr.menuCursorX;
		cursorY = vr.menuCursorY;
	} else {
		cursorX = SCREEN_WIDTH / 2;
		cursorY = SCREEN_HEIGHT / 2;
	}
	offhandCursorX = vr.offhandCursorX;
	offhandCursorY = vr.offhandCursorY;

	hoverKey = VKB_HoverAt( cursorX, cursorY, CL_VRInput_PointerOnScreen( vr.menuLeftHanded ? 0 : 1 ) );
	offhandHoverKey = VKB_HoverAt( offhandCursorX, offhandCursorY, CL_VRInput_PointerOnScreen( vr.menuLeftHanded ? 1 : 0 ) );

	// Draw runs for both eyes; the second pass sees no change, so each crossing ticks once
	if ( VKB_HoverChanged( &vkb.lastHover[VKB_HAND_PRIMARY], hoverKey ) )
		CL_VRInput_HoverTick( vr.menuLeftHanded ? 0 : 1 );
	if ( VKB_HoverChanged( &vkb.lastHover[VKB_HAND_OFFHAND], offhandHoverKey ) )
		CL_VRInput_HoverTick( vr.menuLeftHanded ? 1 : 0 );

	if ( vkb.repeatKey ) {
		qboolean triggerDown;
		const vkbKey_t *ownerHoverKey;

		if ( vkb.repeatHand == VKB_HAND_PRIMARY ) {
			triggerDown = keys[K_MOUSE1].down;
			ownerHoverKey = hoverKey;
		} else {
			triggerDown = vr.vkbOffhandTriggerDown;
			ownerHoverKey = offhandHoverKey;
		}

		if ( !triggerDown || ownerHoverKey != vkb.repeatKey ) {
			vkb.repeatKey = NULL;
		} else {
			int elapsed = cls.realtime - vkb.repeatPressTime;
			if ( !vkb.repeatStarted ) {
				if ( elapsed >= KEY_REPEAT_DELAY ) {
					vkb.repeatStarted = qtrue;
					vkb.repeatLastTime = cls.realtime;
					VKeyboard_FireAction( vkb.repeatChar, vkb.repeatAction );
				}
			} else if ( cls.realtime - vkb.repeatLastTime >= KEY_REPEAT_RATE ) {
				// Draw runs for both eyes. Never catch up twice in one frame.
				vkb.repeatLastTime = cls.realtime;
				VKeyboard_FireAction( vkb.repeatChar, vkb.repeatAction );
			}
		}
	}

	panel = VKB_PanelRect();
	if ( assets.panel )
		VKB_Draw9( assets.panel, panel.x, panel.y, panel.w, panel.h, VKB_PANEL_CORNER, colorWhite );
	else
		SCR_FillRect( panel.x, panel.y, panel.w, panel.h, panelFlat );

	// Glows, then caps, then labels: each pass finishes before the next overdraws it
	for ( pass = 0; pass < 3; pass++ ) {
		for ( row = 0; row < VKB_ROWS; row++ ) {
			const vkbKey_t *rowKeys = VKB_Row( row );
			for ( i = 0; rowKeys[i].units > 0; i++ ) {
				const vkbKey_t *key = &rowKeys[i];
				const vkbRect_t r = VKB_KeyRect( row, i );
				const qboolean hoveredPrimary = key == hoverKey, hoveredOffhand = key == offhandHoverKey;
				const qboolean hovered = hoveredPrimary || hoveredOffhand;
				const qboolean hoveredLeft = vr.menuLeftHanded ? hoveredPrimary : hoveredOffhand;	// the blue pointer's hand
				const qboolean pressed = ( hoveredPrimary && keys[K_MOUSE1].down ) || ( hoveredOffhand && vr.vkbOffhandTriggerDown );
				const qboolean active = ( key->action == VKB_SHIFT && vkb.mods.shift ) || ( key->action == VKB_CAPS && vkb.mods.caps );
				if ( pass == 0 ) {
					if ( hoveredPrimary && hoveredOffhand )
						VKeyboard_DrawGlow( &r, violet );
					else if ( hoveredLeft )
						VKeyboard_DrawGlow( &r, azure );
					else if ( hovered )
						VKeyboard_DrawGlow( &r, ember );
				} else if ( pass == 1 ) {
					VKeyboard_DrawCap( key, &r, hovered, pressed, active );
				} else {
					VKeyboard_DrawLabel( key, &r, hovered, pressed, active );
				}
			}
		}
	}

	// Blue = left physical hand, red = right physical hand; the pointers' pools of light stand in for the dots
	if ( vr.pointerMode != VR_POINTER_DRAWN ) {
		#define CURSOR_DOT_SIZE	6

		// menuLeftHanded means the left physical hand drives the primary cursor
		vec4_t colorLeft  = {0.3f, 0.5f, 1.0f, 1.0f};
		vec4_t colorRight = {1.0f, 0.3f, 0.3f, 1.0f};
		float *primaryColor = vr.menuLeftHanded ? colorLeft : colorRight;
		float *offhandColor = vr.menuLeftHanded ? colorRight : colorLeft;

		// Drawn first so the primary dot lands on top
		SCR_FillRect( offhandCursorX - CURSOR_DOT_SIZE / 2, offhandCursorY - CURSOR_DOT_SIZE / 2,
			CURSOR_DOT_SIZE, CURSOR_DOT_SIZE, offhandColor );

		SCR_FillRect( cursorX - CURSOR_DOT_SIZE / 2, cursorY - CURSOR_DOT_SIZE / 2,
			CURSOR_DOT_SIZE, CURSOR_DOT_SIZE, primaryColor );
	}
}

/*
=================
Sending
=================
*/
static void VKeyboard_Tap( int key ) {
	CL_KeyEvent( key, qtrue, cls.realtime );
	CL_KeyEvent( key, qfalse, cls.realtime );
}

static void VKeyboard_PlayClickSound( void ) {
	if ( vkb.clickSound ) {
		S_StartLocalSound( vkb.clickSound, CHAN_LOCAL_SOUND );
	}
}

/* No modifier side effects, so key repeat can reuse it. */
static void VKeyboard_FireAction( int ch, int action ) {
	VKeyboard_PlayClickSound();

	if ( ch ) {
		CL_CharEvent( ch );
		return;
	}

	switch ( action ) {
		case VKB_BACKSPACE:
			CL_CharEvent( 'h' - 'a' + 1 );
			break;
		case VKB_TAB:
			VKeyboard_Tap( K_TAB );
			break;
		case VKB_ENTER:
			VKeyboard_Tap( K_ENTER );
			break;
		case VKB_HOME:
			VKeyboard_Tap( K_HOME );
			break;
		case VKB_END:
			VKeyboard_Tap( K_END );
			break;
		case VKB_PGUP:
			VKeyboard_Tap( K_PGUP );
			break;
		case VKB_PGDN:
			VKeyboard_Tap( K_PGDN );
			break;
		case VKB_UP:
			VKeyboard_Tap( K_UPARROW );
			break;
		case VKB_DOWN:
			VKeyboard_Tap( K_DOWNARROW );
			break;
		case VKB_LEFT:
			VKeyboard_Tap( K_LEFTARROW );
			break;
		case VKB_RIGHT:
			VKeyboard_Tap( K_RIGHTARROW );
			break;
		default:
			break;
	}
}

/* handIndex takes ownership of the repeat, so the last hand to press wins. */
static void VKeyboard_ProcessKeyPress( const vkbKey_t *key, int handIndex ) {
	qboolean previousHand = vr.menuLeftHanded;
	char ch;
	vr.menuLeftHanded = handIndex == VKB_HAND_PRIMARY ? previousHand : !previousHand;
	CL_VRInput_HapticEvent( "menu_move", 0, 0, 0, 0, 0 );
	vr.menuLeftHanded = previousHand;

	switch ( key->action ) {
		case VKB_SHIFT:
		case VKB_CAPS:
			VKeyboard_PlayClickSound();
			VKB_ModsPress( &vkb.mods, key );
			vkb.repeatKey = NULL;
			return;
		case VKB_ENTER:
			// The console takes further commands; a UI menu is done after Enter
			if ( !( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) {
				VKeyboard_PlayClickSound();
				VKeyboard_Tap( K_ENTER );
				VKeyboard_Hide();
				return;
			}
			break;
		default:
			break;
	}

	ch = VKB_Glyph( key, &vkb.mods );
	VKeyboard_FireAction( ch, key->action );
	// A character consumes a one-shot shift; the repeat keeps the glyph resolved before that
	VKB_ModsPress( &vkb.mods, key );
	vkb.repeatKey = key;
	vkb.repeatChar = ch;
	vkb.repeatAction = key->action;
	vkb.repeatPressTime = cls.realtime;
	vkb.repeatLastTime = cls.realtime;
	vkb.repeatStarted = qfalse;
	vkb.repeatHand = handIndex;
}

static void VKeyboard_DismissWithConsole( void ) {
	VKeyboard_Hide();
	if ( Key_GetCatcher() & KEYCATCH_CONSOLE ) {
		Con_ToggleConsole_f();
	}
}

/*
=================
VKeyboard_HandleKey

Returns qtrue if the keyboard handled this key event (primary hand via K_MOUSE1).
=================
*/
qboolean VKeyboard_HandleKey( int key ) {
	const vkbKey_t *keyDef;
	int cursorX, cursorY;

	if ( !vkb.active ) {
		return qfalse;
	}

	if ( key == K_ESCAPE || key == K_MENU ) {
		VKeyboard_DismissWithConsole();
		return qtrue;
	}

	if ( key != K_MOUSE1 ) {
		return qfalse;
	}

	if ( vr.menuCursorActive ) {
		cursorX = vr.menuCursorX;
		cursorY = vr.menuCursorY;
	} else {
		cursorX = SCREEN_WIDTH / 2;
		cursorY = SCREEN_HEIGHT / 2;
	}

	keyDef = VKB_KeyAt( cursorX, cursorY, NULL );
	if ( !keyDef ) {
		if ( VKB_InPanel( cursorX, cursorY ) ) {
			return qtrue;	// near miss
		}
		VKeyboard_DismissWithConsole();
		return qtrue;
	}

	VKeyboard_ProcessKeyPress( keyDef, VKB_HAND_PRIMARY );
	return qtrue;
}

void VKeyboard_HandleOffhandKey( qboolean down ) {
	const vkbKey_t *keyDef;

	if ( !vkb.active ) {
		return;
	}

	if ( !down ) {
		if ( vkb.repeatHand == VKB_HAND_OFFHAND ) {
			vkb.repeatKey = NULL;
		}
		return;
	}

	keyDef = VKB_KeyAt( vr.offhandCursorX, vr.offhandCursorY, NULL );
	if ( !keyDef ) {
		if ( VKB_InPanel( vr.offhandCursorX, vr.offhandCursorY ) ) {
			return;		// near miss
		}
		VKeyboard_DismissWithConsole();
		return;
	}

	VKeyboard_ProcessKeyPress( keyDef, VKB_HAND_OFFHAND );
}
