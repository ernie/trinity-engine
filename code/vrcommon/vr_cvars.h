#ifndef VR_CVARS_H
#define VR_CVARS_H
#include "../qcommon/q_shared.h"
void VR_InitCvars( void );
extern cvar_t *vr_worldscale;
extern cvar_t *vr_worldscaleScaler;
extern cvar_t *vr_currentHudDepth;
extern cvar_t *vr_righthanded;
extern cvar_t *vr_switchThumbsticks;
extern cvar_t *vr_snapturn;
extern cvar_t *vr_heightAdjust;
extern cvar_t *vr_directionMode;
extern cvar_t *vr_weaponPitch;
extern cvar_t *vr_twoHandedWeapons;
extern cvar_t *vr_refreshrate;
extern cvar_t *vr_refreshrates;
extern cvar_t *vr_superSampling;
extern cvar_t *vr_weaponScope;
extern cvar_t *vr_6dof;
extern cvar_t *vr_hudYOffset;
extern cvar_t *vr_hudScale;
extern cvar_t *vr_sendRollToServer;
extern cvar_t *vr_hapticIntensity;
extern cvar_t *vr_bhaptics;
extern cvar_t *vr_weaponSelectorMode;
extern cvar_t *vr_currentHudDrawStatus;
extern cvar_t *vr_showConsoleMessages;
extern cvar_t *vr_desktopContentFit;
extern cvar_t *vr_desktopContentType;
extern cvar_t *vr_desktopMenuStyle;
extern cvar_t *vr_mirrorEnabled;
extern cvar_t *vr_mirrorFullscreen;
extern cvar_t *vr_mirrorWidth;
extern cvar_t *vr_mirrorHeight;
void VR_InitMirrorCvars( void );
extern cvar_t *vr_virtualScreenMode;
extern cvar_t *vr_screenCurvature;
extern cvar_t *vr_thumbstickDeadzone;
extern cvar_t *vr_thumbstickFullDeflection;
extern cvar_t *vr_triggerSensitivity;
extern cvar_t *vr_gripThreshold;
extern cvar_t *vr_trackpadThreshold;
extern cvar_t *vr_analogWalk;
extern cvar_t *vr_foveation;
extern cvar_t *vr_foveationStrength;
extern cvar_t *vr_foveationCaps;
#endif
