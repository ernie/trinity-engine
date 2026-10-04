#ifndef VM_VR_H
#define VM_VR_H

#include "vm_local.h"

// VR module selection + shared-state protocol glue between vm.c and vrcommon.
// Everything VR-specific in the VM lives beside this header, so the shared vm.c
// stays a clean drop between trinity-engine and trinity-standalone.

qboolean VM_VRSelectModule( vm_t *vm, vmInterpret_t *interpret, qboolean qvmOnly, vmHeader_t **header );
int VM_VRLoadQVMFile( vm_t *vm, const char *filename, void **buffer );
void VM_VRModuleUnloaded( vm_t *vm );
void VM_VRCallEnter( vm_t *vm );
void VM_VRCallLeave( vm_t *vm );
void VM_RegisterVRShared( vm_t *vm, int writer, intptr_t vmAddr, int structSize, int apiMajor, int apiMinor );
qboolean VM_VRSentinel( vm_t *vm );
qboolean VM_VRRegistered( vm_t *vm );

#endif // VM_VR_H
