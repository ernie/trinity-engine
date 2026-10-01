#ifndef CL_VR_BIND_H
#define CL_VR_BIND_H
#include "../qcommon/q_shared.h"
#include "../vrcommon/vr_bind.h"

/* Registered before the config executes, so saved vrbind lines parse at startup. */
void CL_VRBind_InitCommands( void );
void CL_VRBind_Write( fileHandle_t f );
/* A vrLookup_t: plain gameplay reads the ordinary bind table, the other sets their own tables. */
const char *CL_VRBind_Lookup( vrContext_t context, int alt, vrKey_t key, void *user );
/* Tracks the active controller type; the first known type applies its defaults when nothing is bound. */
void CL_VRBind_SetProfile( int profile );
int CL_VRBind_Profile( void );
/* The display name of the first VR key bound to command in context. */
qboolean CL_VRBind_NameFor( const char *context, const char *command, char *buf, int size );
/* Module GetValue keys: vr_menu_skip_button, vr_menu_cancel_button, "vr_bindname <context>[+alt] <command>",
 * "vr_binding <context>[+alt] <keynum>", "vr_keyname <keynum>" and vr_keyfirst. */
qboolean CL_VRBind_GetValue( const char *key, char *value, int size );
#endif
