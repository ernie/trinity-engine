#ifndef CL_VR_MODULES_H
#define CL_VR_MODULES_H

// Select fallback modules before ordinary vid_restart reloads them.
qboolean CL_VRModulesPreflight( void );
qboolean CL_VRModulesCommit( void );
void CL_VRModulesCancel( void );
void CL_VRModulesReset( void );
void CL_VRModulesValidateContext( void );
/* Called after CG_INIT has had a chance to register a compatible QVM. */
qboolean CL_VRModulesPrepareForCGame( void );
qboolean CL_VRModulesNeedsRestart( void );
const char *CL_VRModulesLastError( void );

#endif
