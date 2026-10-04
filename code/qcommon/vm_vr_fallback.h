#ifndef VM_VR_FALLBACK_H
#define VM_VR_FALLBACK_H

#include "vm_local.h"

// Engine-only bundled fallback for cgame and ui; the shared vm.c and vm_vr.h see none of it.
qboolean VM_VRQVMAccepted( vmIndex_t index );
qboolean VM_VRNativeFallback( const vm_t *vm );
qboolean VM_VRPrepareNativeFallback( vmIndex_t index, qboolean qvmOnly, qboolean missionpack );
void VM_VRSetNativeFallback( vmIndex_t index, qboolean enabled );
void VM_VRCancelNativeFallback( vmIndex_t index );

#endif // VM_VR_FALLBACK_H
