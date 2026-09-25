#include "vm_local.h"
#include "vm_vr.h"
#include "vm_vr_select.h"
#include "../vrcommon/vr_shared.h"
#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/*
Module selection behind the shared [vm_vr] seam. Native VR fallback is an
explicit handoff choice, never an implicit change to normal QVM selection.
*/

typedef struct {
	qboolean selected;
	qboolean missionpack;
} vrNativeFallback_t;
static vrNativeFallback_t vrNative[VM_COUNT];

static qboolean VM_VRNativeIndex( vmIndex_t index ) {
	return index == VM_CGAME || index == VM_UI;
}

// Resolve the executable location, not fs_game, downloads, homepath or cwd.
// Loading a fixed child of this directory keeps the module a bundled binary.
static qboolean VM_VRNativePath( char *path, int size, vmIndex_t index, qboolean missionpack ) {
	char directory[MAX_OSPATH];
#ifdef _WIN32
	Q_strncpyz( directory, Sys_Pwd(), sizeof( directory ) ); // GetModuleFileName-based
	if ( !directory[0] || !directory[1] || directory[1] != ':' ) return qfalse;
#elif defined(__linux__)
	char *slash;
	int length = readlink( "/proc/self/exe", directory, sizeof( directory ) - 1 );
	if ( length <= 0 || length >= sizeof( directory ) - 1 ) return qfalse;
	directory[length] = '\0';
	slash = strrchr( directory, '/' );
	if ( !slash ) return qfalse;
	*slash = '\0';
#elif defined(__APPLE__)
	char *slash;
	uint32_t length = sizeof( directory );
	if ( _NSGetExecutablePath( directory, &length ) != 0 || directory[0] != '/' ) return qfalse;
	slash = strrchr( directory, '/' );
	if ( !slash ) return qfalse;
	*slash = '\0';
#else
	return qfalse;
#endif
	// the bundled modules live in the game directories beside the executable
	return Com_sprintf( path, size, "%s/%s/%s" ARCH_STRING DLL_EXT,
		directory, missionpack ? "missionpack" : "baseq3", index == VM_CGAME ? "cgame" : "ui" ) < size;
}

void VM_VRCancelNativeFallback( vmIndex_t index ) {
	vrNativeFallback_t *fallback;
	if ( !VM_VRNativeIndex( index ) ) return;
	fallback = &vrNative[index];
	memset( fallback, 0, sizeof( *fallback ) );
}

// Each caller owns this reference. Preflight releases it immediately; VM_Create
// keeps its fresh reference until the ordinary VM_Free path unloads it.
static void *VM_VRLoadNativeFallback( vmIndex_t index, qboolean missionpack,
	vmMainFunc_t *main, dllEntry_t *entry ) {
	char path[MAX_OSPATH];
	void *handle;
	int (QDECL *api)(void);
	int version;
	if ( !VM_VRNativePath( path, sizeof( path ), index, missionpack ) ) return NULL;
	handle = Sys_LoadLibrary( path );
	if ( !handle ) {
		Com_Printf( "VR fallback is unavailable: %s\n", path );
		return NULL;
	}
	*entry = (dllEntry_t)Sys_LoadFunction( handle, "dllEntry" );
	*main = (vmMainFunc_t)Sys_LoadFunction( handle, "vmMain" );
	api = (int (QDECL *)(void))Sys_LoadFunction( handle, "TrinityVRAPI" );
	version = api ? api() : 0;
	if ( !*entry || !*main || version != ( ( VR_API_MAJOR << 16 ) | VR_API_MINOR ) ) {
		Com_Printf( "VR fallback has no compatible VR ABI: %s\n", path );
		Sys_UnloadLibrary( handle );
		return NULL;
	}
	return handle;
}

qboolean VM_VRPrepareNativeFallback( vmIndex_t index, qboolean qvmOnly, qboolean missionpack ) {
	void *handle;
	vmMainFunc_t main;
	dllEntry_t entry;
	if ( !VM_VRNativeIndex( index ) || qvmOnly || Cvar_VariableIntegerValue( "fs_restrict" ) ) return qfalse;
	VM_VRCancelNativeFallback( index );
	handle = VM_VRLoadNativeFallback( index, missionpack, &main, &entry );
	if ( !handle ) return qfalse;
	Sys_UnloadLibrary( handle );
	vrNative[index].missionpack = missionpack;
	return qtrue;
}

void VM_VRSetNativeFallback( vmIndex_t index, qboolean enabled ) {
	if ( VM_VRNativeIndex( index ) ) vrNative[index].selected = enabled;
}

qboolean VM_VRQVMAccepted( vmIndex_t index ) {
	char filename[MAX_QPATH];
	void *buffer;
	int length, major, minor;
	qboolean accepted;

	Com_sprintf( filename, sizeof( filename ), "vm/%s.qvm", index == VM_CGAME ? "cgame" : "ui" );
	length = FS_ReadFile( filename, &buffer );
	if ( !buffer )
		return qfalse;
	major = VM_VRParseMarker( (const byte *)buffer, length, &minor );
	accepted = VM_VRAccepts( major, minor, VR_API_MAJOR, VR_API_MINOR );
	FS_FreeFile( buffer );
	return accepted;
}

qboolean VM_VRNativeFallback( const vm_t *vm ) {
	return vm && vm->vrNative;
}

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
	if ( VM_VRNativeIndex( vm->index ) && vrNative[vm->index].selected ) {
		vrNativeFallback_t *fallback = &vrNative[vm->index];
		dllEntry_t entry;
		vmMainFunc_t main;
		void *handle;
		// Recheck at consumption: a prepare from an earlier connection grants
		// no permission to substitute native code on a later pure connection.
		if ( qvmOnly || Cvar_VariableIntegerValue( "fs_restrict" ) ) {
			Com_Printf( "%s: native VR fallback blocked by pure/restricted policy\n", vm->name );
			return qfalse;
		}
		handle = VM_VRLoadNativeFallback( vm->index, fallback->missionpack, &main, &entry );
		if ( !handle ) return qfalse;
		vm->dllHandle = handle;
		vm->entryPoint = main;
		vm->privateFlag = 0;
		vm->dataAlloc = vm->dataMask = ~0U;
		vm->dataBase = NULL;
		vm->vrNative = qtrue;
		entry( vm->dllSyscall );
		Com_Printf( "%s: using bundled native VR fallback (VR API %d.%d) from %s\n",
			vm->name, VR_API_MAJOR, VR_API_MINOR, fallback->missionpack ? "missionpack" : "baseq3" );
		return qtrue;
	}

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
	// VR runs the FS-priority winner too: a server's cgame must match its game module.
	*header = VM_LoadQVM( vm, qtrue );
	if ( !*header ) return qfalse;
	if ( vm->vrSentinel )
		Com_Printf( "%s: loaded VR-aware QVM (VR API %d.%d)\n", vm->name, VR_API_MAJOR, VR_API_MINOR );
	else
		Com_Printf( "%s: loaded QVM without a supported VR API marker\n", vm->name );
	return qtrue;
}

int VM_VRLoadQVMFile( vm_t *vm, const char *filename, void **buffer ) {
	int length, major, minor;
	vm->vrSentinel = qfalse;
	length = FS_ReadFile( filename, buffer );
	if ( length <= 0 || !buffer || !*buffer ) return length;
	major = VM_VRParseMarker( (const byte *)*buffer, length, &minor );
	vm->vrSentinel = VM_VRAccepts( major, minor, VR_API_MAJOR, VR_API_MINOR );
	return length;
}
