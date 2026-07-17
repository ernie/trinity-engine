#ifndef VM_VR_H
#define VM_VR_H

#include "vm_local.h"

// VR module selection + shared-state protocol glue between vm.c and vrcommon.
// All VR-specific VM logic lives here so the vendored vm.c stays a clean drop
// from trinity-engine (end-state: one shared vm.c across all three engines).

void VM_VRInit( void );
qboolean VM_VRSelectModule( vm_t *vm, vmInterpret_t *interpret, qboolean qvmOnly, vmHeader_t **header );
int VM_VRLoadQVMFile( vm_t *vm, const char *filename, void **buffer );
void VM_VRModuleUnloaded( vm_t *vm );
void VM_VRCallEnter( vm_t *vm );
void VM_VRCallLeave( vm_t *vm );
void VM_RegisterVRShared( vm_t *vm, int writer, intptr_t vmAddr, int structSize, int apiVersion );
qboolean VM_VRSentinel( vm_t *vm );
qboolean VM_VRRegistered( vm_t *vm );

#endif // VM_VR_H
