// cl_trinity.h -- Trinity tracker integration
#ifndef CL_TRINITY_H
#define CL_TRINITY_H

extern cvar_t *cl_trinityToken;
extern cvar_t *cl_trinityUser;
extern cvar_t *cl_trinityTracker;
extern cvar_t *cl_trinityLoginStatus;

void CL_TrinityInit( void );
void CL_TrinityShutdown( void );
void CL_TrinityPerformLogin( void );

#endif
