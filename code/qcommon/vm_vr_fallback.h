#ifndef VM_VR_FALLBACK_H
#define VM_VR_FALLBACK_H

#include "vm_local.h"

// The bundled native fallback for cgame and ui, an explicit choice cl_vr_modules.c makes after the
// modules register. Engine-only: the shared vm.c and vm_vr.h see none of it.
qboolean VM_VRQVMAccepted( vmIndex_t index );
qboolean VM_VRNativeFallback( const vm_t *vm );
qboolean VM_VRPrepareNativeFallback( vmIndex_t index, qboolean qvmOnly, qboolean missionpack );
void VM_VRSetNativeFallback( vmIndex_t index, qboolean enabled );
void VM_VRCancelNativeFallback( vmIndex_t index );

#endif // VM_VR_FALLBACK_H
