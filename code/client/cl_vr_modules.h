#ifndef CL_VR_MODULES_H
#define CL_VR_MODULES_H

#include "cl_vr_state.h"

// Chooses where a ui or cgame module loads from, just before VM_Create.
void CL_VRModulesPreflight( vmIndex_t index );
void CL_VRModulesReset( void );
void CL_VRModulesValidateContext( void );
/* After UI_INIT and CG_INIT; a fallback verdict asks for a module restart. */
vrModuleVerdict_t CL_VRModulesCheck( const char **reason, const char **source );
// The on-screen notice's two lines, or NULL while it is hidden.
const char *CL_VRModulesNotice( const char **detail );
// Replaces the connection's notice; it draws once play starts.
void CL_VRModulesNoticeShow( const char *headline, const char *detail );
void CL_VRModulesNoticeReset( void );
void CL_VRModulesStatus( void );

#endif
