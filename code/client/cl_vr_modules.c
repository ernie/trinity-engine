#include "client.h"
#include "cl_vr_modules.h"
#include "../qcommon/vm_vr.h"
#include "../vrcommon/vr_state.h"

static qboolean pending, replaceCG, replaceUI, contextValid;
static qboolean contextNativeCG, contextNativeUI;
static qboolean needsRestart;
static char contextGame[MAX_QPATH];
static int contextChallenge, contextServerId;
static connstate_t contextState;
static const char *lastError = "";

const char *CL_VRModulesLastError( void ) {
	return lastError;
}

void CL_VRModulesCancel( void ) {
	if ( replaceCG )
		VM_VRCancelNativeFallback( VM_CGAME );
	if ( replaceUI )
		VM_VRCancelNativeFallback( VM_UI );
	pending = replaceCG = replaceUI = qfalse;
}

void CL_VRModulesReset( void ) {
	CL_VRModulesCancel();
	VM_VRCancelNativeFallback( VM_CGAME );
	VM_VRCancelNativeFallback( VM_UI );
	contextValid = qfalse;
	contextNativeCG = contextNativeUI = qfalse;
	needsRestart = qfalse;
}

qboolean CL_VRModulesNeedsRestart( void ) {
	return needsRestart;
}

// Also called from ordinary module initialization, so an earlier non-pure
// selection cannot leak across a connection, gamestate, or game-directory change.
void CL_VRModulesValidateContext( void ) {
	if ( !contextValid )
		return;
	if ( cl_connectedToPureServer || Cvar_VariableIntegerValue( "fs_restrict" ) ||
		contextChallenge != clc.challenge || Q_stricmp( contextGame, FS_GetCurrentGameDir() ) ) {
		CL_VRModulesReset();
		return;
	}
	if ( contextServerId != cl.serverId ) {
		qboolean missionpack = !Q_stricmp( contextGame, "missionpack" );
		/* A map rotation unloads cgame before this call. Revalidate the saved
		 * selection rather than infer compatibility from the absent VM. Both
		 * replacements must be available before granting either selection. */
		if ( (contextNativeCG && !VM_VRPrepareNativeFallback( VM_CGAME, qfalse, missionpack )) ||
			(contextNativeUI && !VM_VRPrepareNativeFallback( VM_UI, qfalse, missionpack )) ) {
			lastError = "bundled VR modules are unavailable after map change";
			CL_VRModulesReset();
			return;
		}
		VM_VRSetNativeFallback( VM_CGAME, contextNativeCG );
		VM_VRSetNativeFallback( VM_UI, contextNativeUI );
		contextServerId = cl.serverId;
	}
}

static qboolean CL_VRModulesSessionEligible( void ) {
	if ( !VR_IsActiveMode() || !cgvm )
		return qfalse;
	if ( cl_connectedToPureServer || Cvar_VariableIntegerValue( "fs_restrict" ) )
		return qfalse;
	return qtrue;
}

static void CL_VRModulesReportFallback( const vm_t *vm ) {
	Com_Printf( "%s: selecting bundled native VR fallback (%s)\n", vm->name,
		vm->entryPoint ? "native module requires bundled VR selection"
		: vm->vrSentinel ? "QVM did not register VR shared state"
		: "QVM has no supported VR API marker" );
}

qboolean CL_VRModulesPrepareForCGame( void ) {
	const char *gameDir;
	qboolean missionpack, nativeCG, nativeUI;

	/* A missing VM does not establish incompatibility. CG_INIT must first get
	 * the chance to register the negotiated interface on a normal QVM. */
	if ( !CL_VRModulesSessionEligible() )
		return qfalse;
	gameDir = FS_GetCurrentGameDir();
	missionpack = !Q_stricmp( gameDir, "missionpack" );
	if ( !missionpack && gameDir[0] && Q_stricmp( gameDir, "baseq3" ) )
		return qfalse;

	nativeCG = !VM_VRRegistered( cgvm ) && !cgvm->entryPoint;
	nativeUI = uivm && !VM_VRRegistered( uivm );
	if ( !nativeCG && !nativeUI ) {
		needsRestart = qfalse;
		return qfalse;
	}
	if ( nativeCG && !VM_VRPrepareNativeFallback( VM_CGAME, qfalse, missionpack ) ) {
		return qfalse;
	}
	if ( nativeUI && !VM_VRPrepareNativeFallback( VM_UI, qfalse, missionpack ) ) {
		if ( nativeCG )
			VM_VRCancelNativeFallback( VM_CGAME );
		return qfalse;
	}
	if ( nativeCG )
		CL_VRModulesReportFallback( cgvm );
	if ( nativeUI )
		CL_VRModulesReportFallback( uivm );
	VM_VRSetNativeFallback( VM_CGAME, nativeCG );
	VM_VRSetNativeFallback( VM_UI, nativeUI );
	Q_strncpyz( contextGame, gameDir, sizeof( contextGame ) );
	contextChallenge = clc.challenge;
	contextServerId = cl.serverId;
	contextState = cls.state;
	contextNativeCG = nativeCG;
	contextNativeUI = nativeUI;
	contextValid = qtrue;
	needsRestart = nativeCG || nativeUI;
	return nativeCG;
}

static qboolean CL_VRModulesFail( const char *reason ) {
	lastError = reason;
	CL_VRModulesCancel();
	return qfalse;
}

qboolean CL_VRModulesPreflight( void ) {
	const char *gameDir;
	qboolean missionpack;
	CL_VRModulesValidateContext();
	CL_VRModulesCancel();
	lastError = "";
	if ( cls.state != CA_ACTIVE && cls.state != CA_DISCONNECTED )
		return CL_VRModulesFail( "wait for an active match or the main menu" );
	if ( !uivm ) {
		if ( cls.state != CA_DISCONNECTED )
			return CL_VRModulesFail( "UI is not initialized" );
		// Startup: no module is loaded yet, so there is nothing to replace. The
		// modules the restart loads register their VR interface themselves.
		replaceCG = replaceUI = qfalse;
		Q_strncpyz( contextGame, FS_GetCurrentGameDir(), sizeof( contextGame ) );
		contextChallenge = clc.challenge;
		contextServerId = cl.serverId;
		contextState = cls.state;
		contextValid = pending = qtrue;
		return qtrue;
	}
	// Even a registered native VM is unacceptable on a pure server.
	if ( cl_connectedToPureServer && (uivm->entryPoint || (cgvm && cgvm->entryPoint)) )
		return CL_VRModulesFail( "pure servers require compatible QVMs" );
	replaceCG = cgvm && (!(VM_VRRegistered( cgvm ) || VM_VRSentinel( cgvm )) || cgvm->entryPoint);
	replaceUI = !(VM_VRRegistered( uivm ) || VM_VRSentinel( uivm )) || uivm->entryPoint;
	if ( cl_connectedToPureServer && (replaceCG || replaceUI) )
		return CL_VRModulesFail( "pure servers require VR-compatible cgame and UI QVMs" );
	gameDir = FS_GetCurrentGameDir();
	missionpack = !Q_stricmp( gameDir, "missionpack" );
	if ( (replaceCG || replaceUI) && !missionpack && gameDir[0] && Q_stricmp( gameDir, "baseq3" ) )
		return CL_VRModulesFail( "native fallback only supports baseq3 and missionpack" );
	// Both DLLs must be available before either current module is destroyed.
	if ( replaceCG && !VM_VRPrepareNativeFallback( VM_CGAME, qfalse, missionpack ) )
		return CL_VRModulesFail( "bundled VR cgame is unavailable" );
	if ( replaceUI && !VM_VRPrepareNativeFallback( VM_UI, qfalse, missionpack ) )
		return CL_VRModulesFail( "bundled VR UI is unavailable" );
	Q_strncpyz( contextGame, gameDir, sizeof( contextGame ) );
	contextChallenge = clc.challenge;
	contextServerId = cl.serverId;
	contextState = cls.state;
	if ( replaceCG )
		CL_VRModulesReportFallback( cgvm );
	if ( replaceUI )
		CL_VRModulesReportFallback( uivm );
	contextValid = pending = qtrue;
	return qtrue;
}

qboolean CL_VRModulesCommit( void ) {
	if ( !pending )
		return CL_VRModulesFail( "no prepared module selection" );
	if ( contextChallenge != clc.challenge || contextServerId != cl.serverId ||
		contextState != cls.state || Q_stricmp( contextGame, FS_GetCurrentGameDir() ) ||
		(cl_connectedToPureServer && (replaceCG || replaceUI)) )
		return CL_VRModulesFail( "connection changed before restart" );
	VM_VRSetNativeFallback( VM_CGAME, replaceCG );
	VM_VRSetNativeFallback( VM_UI, replaceUI );
	contextNativeCG = replaceCG;
	contextNativeUI = replaceUI;
	pending = replaceCG = replaceUI = qfalse;
	needsRestart = qfalse;
	return qtrue;
}
