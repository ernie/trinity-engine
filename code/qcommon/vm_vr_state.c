#include "vm_local.h"
#include "vm_vr.h"
#include "../vrcommon/vr_state.h"

void VM_VRModuleUnloaded( vm_t *vm ) {
	if ( vm->vrShared ) {
		VR_SharedModuleUnloaded( vm->vrWriter );
	}
	vm->vrShared = NULL;
	vm->vrStructSize = 0;
}

void VM_VRCallEnter( vm_t *vm ) {
	if ( vm->callLevel == 1 && vm->vrShared ) {
		VR_SharedSyncIn( vm->vrShared, vm->vrStructSize );
	}
}

void VM_VRCallLeave( vm_t *vm ) {
	if ( vm->callLevel == 1 && vm->vrShared ) {
		VR_SharedSyncOut( vm->vrShared, vm->vrWriter, vm->vrStructSize );
	}
}

void VM_RegisterVRShared( vm_t *vm, int writer, intptr_t vmAddr, int structSize, int apiMajor, int apiMinor ) {
	uintptr_t address = (uintptr_t)vmAddr;
	int expectedWriter = vm->index == VM_CGAME ? VR_WRITER_CGAME :
		vm->index == VM_GAME ? VR_WRITER_GAME :
		vm->index == VM_UI ? VR_WRITER_UI : -1;

	if ( apiMajor != VR_API_MAJOR || apiMinor < 0 || apiMinor > VR_API_MINOR || writer != expectedWriter ) {
		Com_Error( ERR_DROP, "%s: VR API registration incompatible (engine %d.%d, mod %d.%d, writer %d expected %d)",
			vm->name, VR_API_MAJOR, VR_API_MINOR, apiMajor, apiMinor, writer, expectedWriter );
	}
	/* Preserve API 1.0 prefix-size semantics; bounds cover only bytes used. */
	if ( structSize < 0 ) {
		structSize = 0;
	}
	if ( structSize > (int)sizeof( vr_shared_t ) ) {
		structSize = sizeof( vr_shared_t );
	}
	if ( !address || (address & 3) ) {
		Com_Error( ERR_DROP, "VM_RegisterVRShared: invalid address" );
	}
	if ( !vm->entryPoint ) {
		/* Do not mask untrusted addresses: wrapping can validate another block. */
		size_t arena = (size_t)vm->dataMask + 1;
		if ( !vm->dataBase || address >= arena || (size_t)structSize > arena - address ) {
			Com_Error( ERR_DROP, "VM_RegisterVRShared: state outside VM memory" );
		}
		vm->vrShared = (vr_shared_t *)(vm->dataBase + address);
	} else {
		vm->vrShared = (vr_shared_t *)address;
	}
	vm->vrWriter = writer;
	vm->vrStructSize = structSize;
	VR_SharedSyncIn( vm->vrShared, vm->vrStructSize );
	Com_Printf( "%s: VR shared state registered (mod VR API %d.%d, %s)\n",
		vm->name, apiMajor, apiMinor, vm->entryPoint ? "native" : "QVM" );
}

qboolean VM_VRSentinel( vm_t *vm ) {
	return vm && vm->vrSentinel ? qtrue : qfalse;
}

qboolean VM_VRRegistered( vm_t *vm ) {
	return vm && vm->vrShared ? qtrue : qfalse;
}
