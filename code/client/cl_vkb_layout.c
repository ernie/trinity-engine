// cl_vkb_layout.c -- the VR keyboard's layout, geometry and modifier state
#include "cl_vkb_layout.h"
#include <stddef.h>

#define C( p, s )					{ p, s, VKB_CHAR, 1.0f, NULL }
#define K( action, units, label )	{ 0, 0, action, units, label }
#define END							{ 0, 0, VKB_CHAR, 0, NULL }

// The 65% PC layout: a main block of 15 units plus a navigation column, every row 16 units.
static const vkbKey_t row0[] = {
	C( '`', '~' ), C( '1', '!' ), C( '2', '@' ), C( '3', '#' ), C( '4', '$' ), C( '5', '%' ), C( '6', '^' ),
	C( '7', '&' ), C( '8', '*' ), C( '9', '(' ), C( '0', ')' ), C( '-', '_' ), C( '=', '+' ),
	K( VKB_BACKSPACE, 2.0f, NULL ), K( VKB_HOME, 1.0f, "HOME" ), END
};
static const vkbKey_t row1[] = {
	K( VKB_TAB, 1.5f, NULL ), C( 'q', 'Q' ), C( 'w', 'W' ), C( 'e', 'E' ), C( 'r', 'R' ), C( 't', 'T' ),
	C( 'y', 'Y' ), C( 'u', 'U' ), C( 'i', 'I' ), C( 'o', 'O' ), C( 'p', 'P' ), C( '[', '{' ), C( ']', '}' ),
	{ '\\', '|', VKB_CHAR, 1.5f, NULL }, K( VKB_END, 1.0f, "END" ), END
};
static const vkbKey_t row2[] = {
	K( VKB_CAPS, 1.75f, "CAPS" ), C( 'a', 'A' ), C( 's', 'S' ), C( 'd', 'D' ), C( 'f', 'F' ), C( 'g', 'G' ),
	C( 'h', 'H' ), C( 'j', 'J' ), C( 'k', 'K' ), C( 'l', 'L' ), C( ';', ':' ), C( '\'', '"' ),
	K( VKB_ENTER, 2.25f, NULL ), K( VKB_PGUP, 1.0f, "PGUP" ), END
};
static const vkbKey_t row3[] = {
	K( VKB_SHIFT, 2.25f, "SHIFT" ), C( 'z', 'Z' ), C( 'x', 'X' ), C( 'c', 'C' ), C( 'v', 'V' ), C( 'b', 'B' ),
	C( 'n', 'N' ), C( 'm', 'M' ), C( ',', '<' ), C( '.', '>' ), C( '/', '?' ),
	K( VKB_SHIFT, 1.75f, "SHIFT" ), K( VKB_UP, 1.0f, NULL ), K( VKB_PGDN, 1.0f, "PGDN" ), END
};
static const vkbKey_t row4[] = {
	K( VKB_SPACE, 13.0f, NULL ), K( VKB_LEFT, 1.0f, NULL ), K( VKB_DOWN, 1.0f, NULL ), K( VKB_RIGHT, 1.0f, NULL ), END
};
static const vkbKey_t *rows[VKB_ROWS] = { row0, row1, row2, row3, row4 };

#define VKB_PITCH	( VKB_UNIT_W + VKB_SPACING )
#define VKB_ROW_W	( VKB_UNITS * VKB_PITCH - VKB_SPACING )
#define VKB_PANEL_W	( VKB_ROW_W + 2 * VKB_PADDING )
#define VKB_PANEL_H	( VKB_ROWS * ( VKB_KEY_H + VKB_SPACING ) - VKB_SPACING + 2 * VKB_PADDING )

const vkbKey_t *VKB_Row( int row ) {
	return ( row >= 0 && row < VKB_ROWS ) ? rows[row] : NULL;
}

vkbRect_t VKB_PanelRect( void ) {
	vkbRect_t r;
	r.w = VKB_PANEL_W;
	r.h = VKB_PANEL_H;
	r.x = ( VKB_SCREEN_W - r.w ) / 2;
	r.y = VKB_SCREEN_H - r.h;
	return r;
}

static int VKB_Round( float v ) {
	return (int)( v + 0.5f );
}

// Both edges come from rounded unit offsets, so fractional widths still land each key exactly one spacing after the last.
vkbRect_t VKB_KeyRect( int row, int index ) {
	vkbRect_t panel = VKB_PanelRect(), r = { 0, 0, 0, 0 };
	const vkbKey_t *keys = VKB_Row( row );
	float p = 0;
	int i;
	if ( !keys )
		return r;
	for ( i = 0; keys[i].units > 0; i++ ) {
		if ( i == index ) {
			r.x = panel.x + VKB_PADDING + VKB_Round( p * VKB_PITCH );
			r.y = panel.y + VKB_PADDING + row * ( VKB_KEY_H + VKB_SPACING );
			r.w = VKB_Round( ( p + keys[i].units ) * VKB_PITCH ) - VKB_Round( p * VKB_PITCH ) - VKB_SPACING;
			r.h = VKB_KEY_H;
			return r;
		}
		p += keys[i].units;
	}
	return r;
}

static int VKB_Inside( const vkbRect_t *r, int x, int y ) {
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

const vkbKey_t *VKB_KeyAt( int x, int y, vkbRect_t *rect ) {
	int row, i;
	for ( row = 0; row < VKB_ROWS; row++ ) {
		for ( i = 0; rows[row][i].units > 0; i++ ) {
			vkbRect_t r = VKB_KeyRect( row, i );
			if ( VKB_Inside( &r, x, y ) ) {
				if ( rect )
					*rect = r;
				return &rows[row][i];
			}
		}
	}
	return NULL;
}

int VKB_InPanel( int x, int y ) {
	vkbRect_t p = VKB_PanelRect();
	return VKB_Inside( &p, x, y );
}

int VKB_IsLetter( const vkbKey_t *key ) {
	return key->action == VKB_CHAR && key->plain >= 'a' && key->plain <= 'z';
}

// What a press sends right now; 0 for keys that send a key event instead
char VKB_Glyph( const vkbKey_t *key, const vkbMods_t *mods ) {
	if ( key->action == VKB_SPACE )
		return ' ';
	if ( key->action != VKB_CHAR )
		return 0;
	if ( VKB_IsLetter( key ) )
		return ( mods->caps != mods->shift ) ? key->shifted : key->plain;
	return mods->shift ? key->shifted : key->plain;
}

void VKB_ModsReset( vkbMods_t *mods ) {
	mods->shift = mods->caps = 0;
}

void VKB_ModsPress( vkbMods_t *mods, const vkbKey_t *key ) {
	switch ( key->action ) {
		case VKB_SHIFT:
			mods->shift = !mods->shift;
			break;
		case VKB_CAPS:
			mods->caps = !mods->caps;
			break;
		case VKB_CHAR:
			mods->shift = 0;
			break;
		default:
			break;
	}
}

// The key under a hand's cursor, or nothing when the hand's ray is not on the screen: a resting hand hovers nowhere
const vkbKey_t *VKB_HoverAt( int x, int y, int aimed ) {
	return aimed ? VKB_KeyAt( x, y, NULL ) : NULL;
}

// True when the hand's hover moved onto a key it was not on; leaving the keys is not a change
int VKB_HoverChanged( const vkbKey_t **last, const vkbKey_t *now ) {
	int changed = now != NULL && now != *last;
	*last = now;
	return changed;
}
