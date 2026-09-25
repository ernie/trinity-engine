#include "client.h"
#include "cl_vr.h"
#include "cl_vr_modules.h"
#include "../qcommon/vm_vr.h"

/* The fallback choice of the connection that made it; CL_VRModulesPreflight applies it to each load. */
static qboolean contextNativeCG, contextNativeUI;
static char contextGame[MAX_QPATH];
static int contextChallenge, contextServerId;

#define NOTICE_MSEC 5000

/* The pak, or the game directory for a loose file, of the QVM each fallback module replaced. */
static char replacedSource[VM_COUNT][MAX_QPATH];
static enum { NOTICE_NONE, NOTICE_PENDING, NOTICE_SHOWING, NOTICE_DONE } notice;
static int noticeStart;
/* Empty for the fallback notice, whose pak follows the running modules. */
static char noticeText[2][MAX_QPATH + 32];

void CL_VRModulesReset( void ) {
	VM_VRCancelNativeFallback( VM_CGAME );
	VM_VRCancelNativeFallback( VM_UI );
	contextNativeCG = contextNativeUI = qfalse;
}

static qboolean CL_VRModulesBaseGame( const char *gameDir ) {
	return !gameDir[0] || !Q_stricmp( gameDir, "baseq3" ) || !Q_stricmp( gameDir, "missionpack" );
}

// A saved fallback choice holds only for the connection and game directory that made it.
void CL_VRModulesValidateContext( void ) {
	if ( !contextNativeCG && !contextNativeUI )
		return;
	if ( cl_connectedToPureServer || Cvar_VariableIntegerValue( "fs_restrict" ) ||
		contextChallenge != clc.challenge || Q_stricmp( contextGame, FS_GetCurrentGameDir() ) ) {
		CL_VRModulesReset();
		return;
	}
	if ( contextServerId != cl.serverId ) {
		qboolean missionpack = !Q_stricmp( contextGame, "missionpack" );
		/* Without the saved fallbacks the winning QVMs load, and their registration decides. */
		if ( (contextNativeCG && !VM_VRPrepareNativeFallback( VM_CGAME, qfalse, missionpack )) ||
			(contextNativeUI && !VM_VRPrepareNativeFallback( VM_UI, qfalse, missionpack )) ) {
			Com_Printf( "Bundled VR modules are unavailable after the map change\n" );
			CL_VRModulesReset();
			return;
		}
		contextServerId = cl.serverId;
	}
}

static void CL_VRModulesQVMSource( vmIndex_t index, char *source, int size ) {
	char filename[MAX_QPATH], pak[MAX_OSPATH];
	fileHandle_t f;
	qboolean inPak;

	Com_sprintf( filename, sizeof( filename ), "vm/%s.qvm", index == VM_CGAME ? "cgame" : "ui" );
	Q_strncpyz( source, FS_GetCurrentGameDir(), size );
	FS_FOpenFileRead( filename, &f, qfalse );
	if ( f == FS_INVALID_HANDLE )
		return;
	inPak = FS_PakIndexForHandle( f ) >= 0;
	FS_FCloseFile( f );
	// FS_FileIsInPAK skips loose files, so it names the winner only when a pak supplies it.
	if ( inPak && FS_FileIsInPAK( filename, NULL, pak ) )
		Com_sprintf( source, size, "%s.pk3", COM_SkipPath( pak ) );
}

static void CL_VRModulesReplaced( vmIndex_t index ) {
	CL_VRModulesQVMSource( index, replacedSource[index], sizeof( replacedSource[index] ) );
	if ( index == VM_CGAME && notice == NOTICE_NONE )
		notice = NOTICE_PENDING;
}

// Notices belong to the connection; the UI's record belongs to the loaded UI.
void CL_VRModulesNoticeReset( void ) {
	notice = NOTICE_NONE;
	noticeText[0][0] = '\0';
	replacedSource[VM_CGAME][0] = '\0';
}

void CL_VRModulesNoticeShow( const char *headline, const char *detail ) {
	Q_strncpyz( noticeText[0], headline, sizeof( noticeText[0] ) );
	Q_strncpyz( noticeText[1], detail, sizeof( noticeText[1] ) );
	notice = NOTICE_PENDING;
}

// The cgame's pak outranks the UI's; the timer starts on the first frame of play.
const char *CL_VRModulesNotice( const char **detail ) {
	const char *headline = noticeText[0];

	if ( notice != NOTICE_PENDING && notice != NOTICE_SHOWING )
		return NULL;
	if ( cls.state != CA_ACTIVE )
		return NULL;
	if ( headline[0] ) {
		*detail = noticeText[1];
	} else {
		static char fallbackDetail[MAX_QPATH + 32];
		const char *source = NULL;

		if ( cgvm && VM_VRNativeFallback( cgvm ) && replacedSource[VM_CGAME][0] )
			source = replacedSource[VM_CGAME];
		else if ( uivm && VM_VRNativeFallback( uivm ) && replacedSource[VM_UI][0] )
			source = replacedSource[VM_UI];
		if ( !source )
			return NULL;
		headline = "Fallback VR modules active";
		Com_sprintf( fallbackDetail, sizeof( fallbackDetail ), "%s QVMs are VR-incompatible", source );
		*detail = fallbackDetail;
	}
	if ( notice == NOTICE_PENDING ) {
		notice = NOTICE_SHOWING;
		noticeStart = cls.realtime;
	}
	if ( cls.realtime - noticeStart >= NOTICE_MSEC ) {
		notice = NOTICE_DONE;
		return NULL;
	}
	return headline;
}

void CL_VRModulesStatus( void ) {
	vm_t *vms[2];
	int i;

	vms[0] = cgvm;
	vms[1] = uivm;
	for ( i = 0; i < 2; i++ ) {
		if ( !vms[i] || !VM_VRNativeFallback( vms[i] ) )
			continue;
		if ( replacedSource[vms[i]->index][0] )
			Com_Printf( "%s: bundled VR fallback module; VR-incompatible QVM from %s\n", vms[i]->name,
				replacedSource[vms[i]->index] );
		else
			Com_Printf( "%s: bundled VR fallback module\n", vms[i]->name );
	}
}

static void CL_VRModulesReportFallback( const vm_t *vm ) {
	if ( vm->entryPoint ) {
		Com_Printf( "%s: selecting bundled native VR fallback (native module requires bundled VR selection)\n",
			vm->name );
		return;
	}
	Com_Printf( "%s: selecting bundled native VR fallback (%s QVM %s)\n", vm->name, replacedSource[vm->index],
		vm->vrSentinel ? "did not register VR shared state" : "has no supported VR API marker" );
}

void CL_VRModulesPreflight( vmIndex_t index ) {
	const char *gameDir = FS_GetCurrentGameDir();
	qboolean wantVR = CL_VR_RestartWantsVR();
	qboolean native;

	if ( !wantVR ) {
		VM_VRSetNativeFallback( index, qfalse );
		return;
	}
	// A fallback UI is announced at the connection's cgame load, so the notice shows in the match.
	if ( index == VM_CGAME && notice == NOTICE_NONE && uivm && VM_VRNativeFallback( uivm ) && replacedSource[VM_UI][0] )
		notice = NOTICE_PENDING;
	if ( index == VM_CGAME ? contextNativeCG : contextNativeUI ) {
		VM_VRSetNativeFallback( index, qtrue );
		return;
	}
	native = !cl_connectedToPureServer && CL_VRModulesBaseGame( gameDir ) && !VM_VRQVMAccepted( index ) &&
		VM_VRPrepareNativeFallback( index, qfalse, !Q_stricmp( gameDir, "missionpack" ) );
	VM_VRSetNativeFallback( index, native );
	if ( native ) {
		CL_VRModulesReplaced( index );
		Com_Printf( "%s: %s QVM has no supported VR API marker; selecting bundled native VR fallback\n",
			index == VM_CGAME ? "cgame" : "ui", replacedSource[index] );
	} else {
		replacedSource[index][0] = '\0';
	}
}

vrModuleVerdict_t CL_VRModulesCheck( const char **reason, const char **source ) {
	static char text[MAX_STRING_CHARS], worstSource[MAX_QPATH];
	const char *gameDir = FS_GetCurrentGameDir();
	qboolean missionpack = !Q_stricmp( gameDir, "missionpack" );
	vrModuleVerdict_t worst = VRMOD_OK;
	vmIndex_t worstIndex = VM_UI;
	qboolean fallback[2];
	vm_t *vms[2];
	int i;

	vms[0] = uivm;
	vms[1] = cgvm;
	*reason = "";
	*source = "";
	for ( i = 0; i < 2; i++ ) {
		vrModuleCheck_t check;
		vrModuleVerdict_t verdict;
		const char *why;

		fallback[i] = qfalse;
		if ( !vms[i] )
			continue;
		check.registered = VM_VRRegistered( vms[i] ) && !(cl_connectedToPureServer && vms[i]->entryPoint);
		check.provisional = cls.state > CA_DISCONNECTED && cls.state < CA_LOADING;
		check.pure = cl_connectedToPureServer != 0;
		check.baseGame = CL_VRModulesBaseGame( gameDir );
		check.fallbackRunning = VM_VRNativeFallback( vms[i] );
		check.fallbackAvailable = !check.registered && !check.provisional && !check.pure && check.baseGame &&
			!check.fallbackRunning && VM_VRPrepareNativeFallback( vms[i]->index, qfalse, missionpack );
		verdict = VRState_ModuleVerdict( &check, &why );
		fallback[i] = verdict == VRMOD_FALLBACK;
		if ( verdict > worst ) {
			worst = verdict;
			worstIndex = vms[i]->index;
			Com_sprintf( text, sizeof( text ), "%s: %s", vms[i]->name, why );
			*reason = text;
		}
	}
	if ( worst == VRMOD_UNSUPPORTED ) {
		CL_VRModulesQVMSource( worstIndex, worstSource, sizeof( worstSource ) );
		*source = worstSource;
	}
	if ( worst != VRMOD_FALLBACK )
		return worst;
	for ( i = 0; i < 2; i++ ) {
		if ( !fallback[i] )
			continue;
		if ( vms[i]->entryPoint )
			replacedSource[vms[i]->index][0] = '\0';
		else
			CL_VRModulesReplaced( vms[i]->index );
		CL_VRModulesReportFallback( vms[i] );
		if ( vms[i]->index == VM_CGAME )
			contextNativeCG = qtrue;
		else
			contextNativeUI = qtrue;
	}
	Q_strncpyz( contextGame, gameDir, sizeof( contextGame ) );
	contextChallenge = clc.challenge;
	contextServerId = cl.serverId;
	return worst;
}
