#ifndef VM_VR_H
#define VM_VR_H

#include "vm_local.h"

// VR module selection + shared-state protocol glue between vm.c and vrcommon.
// Everything VR-specific in the VM lives beside this header, so vm.c stays close to upstream Quake3e.

qboolean VM_VRSelectModule( vm_t *vm, vmInterpret_t *interpret, qboolean qvmOnly, vmHeader_t **header );
int VM_VRLoadQVMFile( vm_t *vm, const char *filename, void **buffer );
void VM_VRModuleUnloaded( vm_t *vm );
void VM_VRCallEnter( vm_t *vm );
void VM_VRCallLeave( vm_t *vm );
void VM_RegisterVRShared( vm_t *vm, int writer, intptr_t vmAddr, int structSize, int apiMajor, int apiMinor );
qboolean VM_VRSentinel( const vm_t *vm );
qboolean VM_VRApiMismatch( const vm_t *vm );		// marked for a VR API this engine can't satisfy
qboolean VM_VRRegistered( vm_t *vm );

// Major of the first TRINITY_VR_API/<major>.<minor> marker inside length; 0 when absent.
int VM_VRParseMarker( const byte *data, int length, int *minor );
// The engine runs a QVM whose major matches and whose minor it meets or exceeds.
qboolean VM_VRAccepts( int major, int minor, int engineMajor, int engineMinor );

// The bundled native modules that stand in for a flat-only cgame or ui.
qboolean VM_VRQVMAccepted( vmIndex_t index );
qboolean VM_VRQVMApiMismatch( vmIndex_t index );	// marked for a VR API this engine can't satisfy
qboolean VM_VRNativeFallback( const vm_t *vm );
qboolean VM_VRPrepareNativeFallback( vmIndex_t index, qboolean qvmOnly, qboolean missionpack );
void VM_VRSetNativeFallback( vmIndex_t index, qboolean enabled );
void VM_VRCancelNativeFallback( vmIndex_t index );

#endif // VM_VR_H
