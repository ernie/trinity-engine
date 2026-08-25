#include "vm_local.h"
#include "vm_vr.h"

/*
Module selection behind the shared [vm_vr] seam: stock Quake3e, relocated
from the pre-seam VM_Create. No VR on flatscreen: the seam funcs are inert.
*/

// the shared vm.c unloads native modules through this wrapper; map it onto Sys_UnloadLibrary
void Sys_UnloadDll( void *dllHandle ) {
	if ( !dllHandle ) {
		Com_Printf( "Sys_UnloadDll(NULL)\n" );
		return;
	}
	Sys_UnloadLibrary( dllHandle );
}


/*
=================
Sys_LoadDll

Used to load a development dll instead of a virtual machine

TTimo: added some verbosity in debug
=================
*/
static void * QDECL VM_LoadDll( const char *name, vmMainFunc_t *entryPoint, dllSyscall_t systemcalls ) {

	char		filename[ MAX_QPATH ];
	void		*libHandle;
	dllEntry_t	dllEntry;

	Com_sprintf( filename, sizeof( filename ), "%s" ARCH_STRING DLL_EXT, name );

	libHandle = FS_LoadLibrary( filename );

	if ( !libHandle ) {
		Com_Printf( "VM_LoadDLL '%s' failed\n", filename );
		return NULL;
	}

	Com_Printf( "VM_LoadDLL '%s' ok\n", filename );

	dllEntry = /* ( dllEntry_t ) */ Sys_LoadFunction( libHandle, "dllEntry" );
	*entryPoint = /* ( dllSyscall_t ) */ Sys_LoadFunction( libHandle, "vmMain" );
	if ( !*entryPoint || !dllEntry ) {
		Sys_UnloadLibrary( libHandle );
		return NULL;
	}

	Com_Printf( "VM_LoadDll(%s) found **vmMain** at %p\n", name, *entryPoint );
	dllEntry( systemcalls );
	Com_Printf( "VM_LoadDll(%s) succeeded!\n", name );

	return libHandle;
}

qboolean VM_VRSelectModule( vm_t *vm, vmInterpret_t *interpret, qboolean qvmOnly, vmHeader_t **header ) {
	*header = NULL;

	// never allow dll loading with a demo
	if ( *interpret == VMI_NATIVE ) {
		if ( Cvar_VariableIntegerValue( "fs_restrict" ) ) {
			*interpret = VMI_COMPILED;
		}
	}

	if ( *interpret == VMI_NATIVE && !qvmOnly ) {
		// try to load as a system dll
		Com_Printf( "Loading dll file %s.\n", vm->name );
		vm->dllHandle = VM_LoadDll( vm->name, &vm->entryPoint, vm->dllSyscall );
		if ( vm->dllHandle ) {
			vm->privateFlag = 0; // allow reading private cvars
			vm->dataAlloc = ~0U;
			vm->dataMask = ~0U;
			vm->dataBase = 0;
			return qtrue;
		}
		Com_Printf( "Failed to load dll, looking for qvm.\n" );
		*interpret = VMI_COMPILED;
	}

	// a QVM can't run native; execute under the JIT
	if ( *interpret == VMI_NATIVE )
		*interpret = VMI_COMPILED;
	// load the qvm: FS-priority winner, any QVM, no inspection
	*header = VM_LoadQVM( vm, qtrue );
	return ( *header != NULL ) ? qtrue : qfalse;
}

int VM_VRLoadQVMFile( vm_t *vm, const char *filename, void **buffer ) {
	return FS_ReadFile( filename, buffer );
}

void VM_VRModuleUnloaded( vm_t *vm ) {
}

void VM_VRCallEnter( vm_t *vm ) {
}

void VM_VRCallLeave( vm_t *vm ) {
}

void VM_RegisterVRShared( vm_t *vm, int writer, intptr_t vmAddr, int structSize, int apiMajor, int apiMinor ) {
	Com_Error( ERR_DROP, "VM_RegisterVRShared: not a VR engine" );
}

qboolean VM_VRSentinel( vm_t *vm ) {
	return qfalse;
}

qboolean VM_VRRegistered( vm_t *vm ) {
	return qfalse;
}
