// cl_vkb_layout.h -- the VR keyboard's layout, geometry and modifier state; no engine dependencies
#ifndef CL_VKB_LAYOUT_H
#define CL_VKB_LAYOUT_H

#define VKB_ROWS		5
#define VKB_UNITS		16		// key units across every row
#define VKB_UNIT_W		34
#define VKB_KEY_H		32
#define VKB_SPACING		4
#define VKB_PADDING		12
#define VKB_SCREEN_W	640
#define VKB_SCREEN_H	480

typedef enum {
	VKB_CHAR,		// sends plain or shifted
	VKB_SPACE,
	VKB_BACKSPACE,
	VKB_TAB,
	VKB_ENTER,
	VKB_SHIFT,
	VKB_CAPS,
	VKB_HOME,
	VKB_END,
	VKB_PGUP,
	VKB_PGDN,
	VKB_UP,
	VKB_DOWN,
	VKB_LEFT,
	VKB_RIGHT
} vkbAction_t;

typedef struct {
	char		plain;		// VKB_CHAR only
	char		shifted;	// VKB_CHAR only
	vkbAction_t	action;
	float		units;		// width; 0 ends a row
	const char	*label;		// text for keys drawn with neither a glyph nor an icon
} vkbKey_t;

typedef struct { int x, y, w, h; } vkbRect_t;

typedef struct { int shift, caps; } vkbMods_t;

const vkbKey_t *VKB_Row( int row );		// terminated by a key with units 0
vkbRect_t VKB_PanelRect( void );
vkbRect_t VKB_KeyRect( int row, int index );
const vkbKey_t *VKB_KeyAt( int x, int y, vkbRect_t *rect );
int VKB_InPanel( int x, int y );

int VKB_IsLetter( const vkbKey_t *key );
char VKB_Glyph( const vkbKey_t *key, const vkbMods_t *mods );
void VKB_ModsReset( vkbMods_t *mods );
void VKB_ModsPress( vkbMods_t *mods, const vkbKey_t *key );
const vkbKey_t *VKB_HoverAt( int x, int y, int aimed );
int VKB_HoverChanged( const vkbKey_t **last, const vkbKey_t *now );

#endif
