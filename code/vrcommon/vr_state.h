#ifndef TRINITY_VR_STATE_H
#define TRINITY_VR_STATE_H

#include "vr_shared.h"
#include "vr_clientinfo.h"

#define VR_WRITER_CGAME 0
#define VR_WRITER_GAME 1
#define VR_WRITER_UI 2

extern vr_clientinfo_t vr;
void VR_SharedSyncIn( vr_shared_t *state, int structSize );
void VR_SharedSyncOut( const vr_shared_t *state, int writer, int structSize );
void VR_SharedModuleUnloaded( int writer );
void VR_SetActiveMode( qboolean active );
qboolean VR_IsActiveMode( void );

#endif
