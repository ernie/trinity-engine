#ifndef CL_VR_BIND_H
#define CL_VR_BIND_H
#include "../qcommon/q_shared.h"
#include "../vrcommon/vr_bind.h"

/* Registered before the config executes, so saved vrbind lines parse at startup. */
void CL_VRBind_InitCommands( void );
void CL_VRBind_Write( fileHandle_t f );
void CL_VRBind_GameRestart( void );
/* A vrLookup_t: plain gameplay reads the ordinary bind table, the other sets their own tables. */
const char *CL_VRBind_Lookup( vrContext_t context, int alt, vrKey_t key, void *user );
/* Tracks the active controller type; the first known type applies its defaults when nothing is bound. */
void CL_VRBind_SetProfile( int profile );
int CL_VRBind_Profile( void );
/* The VR key that runs command in context, or -1; altKey is the Alt key of a combo, else -1. */
int CL_VRBind_CommandKey( vrContext_t context, const char *command, int *altKey );
/* Module GetValue keys: "vr_bindkeys <context>[+alt] <command>", "vr_binding <context>[+alt] <keynum>",
 * "vr_keyglyph <keynum>", "vr_keydefault <context>[+alt] <index> <command>" and vr_keyfirst. */
qboolean CL_VRBind_GetValue( const char *key, char *value, int size );
#endif
