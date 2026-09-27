#include "client.h"
#include "cl_vr.h"
#include "cl_vr_input.h"
#include "cl_vr_modules.h"
#include "cl_vr_state.h"
#include "cl_renderer_recovery.h"
#include "../qcommon/vm_vr.h"
#include "../vrcommon/vr_state.h"
#include "../vrcommon/xr_loader.h"

static cvar_t *vr_enabled;
/* stereo is the mode the open XR frame began in. */
static struct {
	qboolean	open;
	qboolean	stereo;
} frame;
static qboolean forcedStartupAttempt, xrFailed;
static vrMachine_t machine;
static int nextTargetCheck, nextHeadsetCheck;
static refXRFrame_t xrFrame;
static qboolean trackingRequested, trackingSampled, trackingMoved;
static refXREye_t initialTrackingPose;
qboolean CL_VR_WaitingForTracking( void ) {
	return trackingRequested && !trackingMoved && vr_enabled && vr_enabled->integer &&
			!(vr_enabled->latchedString && !atoi( vr_enabled->latchedString ));
}
qboolean CL_VR_DesktopWaitingFrame( void ) {
	return !machine.running && !VRState_RestartQueued( &machine ) && frame.open && !frame.stereo && VR_IsActiveMode() &&
			CL_VR_WaitingForTracking();
}
static void CL_VR_CheckTracking( void ) {
	int i;
	qboolean same = qtrue, opposite = qtrue;

	if ( !trackingRequested || trackingMoved || !xrFrame.renderable ) {
		return;
	}
	if ( !trackingSampled ) {
		initialTrackingPose = xrFrame.head;
		trackingSampled = qtrue;
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		if ( initialTrackingPose.position[i] != xrFrame.head.position[i] ) {
			trackingMoved = qtrue;
		}
	}
	for ( i = 0; i < 4; i++ ) {
		if ( initialTrackingPose.orientation[i] != xrFrame.head.orientation[i] ) {
			same = qfalse;
		}
		if ( initialTrackingPose.orientation[i] != -xrFrame.head.orientation[i] ) {
			opposite = qfalse;
		}
	}
	/* q and -q represent the same rotation. */
	if ( !same && !opposite ) {
		trackingMoved = qtrue;
	}
}

static xrLoaderInfo_t cachedProbe;
static char lastFailure[512];
static qboolean startupDialog;
static char dialogText[512];

static qboolean CL_VR_Wanted( void ) {
	return vr_enabled && (vr_enabled->latchedString ? atoi( vr_enabled->latchedString ) : vr_enabled->integer) != 0;
}
static vrAction_t CL_VR_Send( vrEvent_t *ev ) {
	vrState_t before = machine.state;
	vrAction_t action;

	ev->now = cls.realtime;
	ev->wanted = CL_VR_Wanted();
	ev->vrActive = VR_IsActiveMode();
	ev->disconnected = cls.state == CA_DISCONNECTED;
	ev->settled = cls.state == CA_ACTIVE || ev->disconnected;
	ev->safePoint = VRState_SafePoint( cls.state == CA_ACTIVE, ev->disconnected, ev->vrActive, CL_GameSwitch() );
	ev->modChange = (Cvar_Flags( "fs_game" ) & CVAR_MODIFIED) != 0;
	action = VRState_Reduce( &machine, ev );
	if ( machine.state != before )
		Com_DPrintf( "VR state %s -> %s\n", VRState_Name( before ), VRState_Name( machine.state ) );
	return action;
}
static vrAction_t CL_VR_Step( vrEventType_t type, int value ) {
	vrEvent_t ev;

	Com_Memset( &ev, 0, sizeof( ev ) );
	ev.type = type;
	ev.value = value;
	return CL_VR_Send( &ev );
}

/* The cvar write only queues a userinfo update. Command width follows
 * clc.vrIdentity, the identity in the last userinfo the client sent, so the
 * width holds until CL_WritePacket carries the update ahead of the move. */
static void CL_VR_SetMode( qboolean active ) {
	VR_SetActiveMode( active );
	Cvar_Set2( "vr", active ? "1" : "0", qtrue );
}
int CL_VR_UsercmdButtonBits( void ) {
	return (clc.serverSupportsVR && clc.vrIdentity) ? 32 : 16;
}
static void CL_VR_ClearActive( void ) {
	if ( VR_IsActiveMode() )
		cls.glconfig.stereoEnabled = qfalse;
	VKeyboard_Hide();
	CL_VR_SetMode( qfalse );
}
static vrProbe_t CL_VR_WatchBegin( void ) {
	cachedProbe.status = XRLoader_WatchBegin( &cachedProbe );
	return VRState_Probe( cachedProbe.status, cachedProbe.vulkanEnable2 );
}
static void CL_VR_Failure( const char *reason ) {
	Q_strncpyz( lastFailure, reason && *reason ? reason : "XR backend unavailable",
				sizeof( lastFailure ) );
	Com_Printf( "Cannot use VR: %s\n", lastFailure );
}
static qboolean CL_VR_Connected( void ) {
	return cls.state > CA_DISCONNECTED && cls.state != CA_CINEMATIC;
}
/* The flat rebuild that follows reloads the UI, which shows com_errorMessage in its error popup. */
static void CL_VR_ErrorPopup( const char *text ) {
	char error[MAX_STRING_CHARS], message[MAX_STRING_CHARS];

	Cvar_VariableStringBuffer( "com_errorMessage", error, sizeof( error ) );
	if ( error[0] )
		Com_sprintf( message, sizeof( message ), "%s - %s", error, text );
	else
		Q_strncpyz( message, text, sizeof( message ) );
	Cvar_Set( "com_errorMessage", message );
}
/* The next launch or vid_restart probes again, so vr_enabled keeps the player's choice. */
static void CL_VR_Incapable( void ) {
	XRLoader_WatchEnd();
	trackingRequested = qfalse;
	CL_VR_Failure( cachedProbe.vulkanEnable && !cachedProbe.vulkanEnable2 ?
				   "OpenXR runtime lacks XR_KHR_vulkan_enable2" : cachedProbe.message );
	if ( forcedStartupAttempt ) {
		Q_strncpyz( dialogText, lastFailure, sizeof( dialogText ) );
		startupDialog = qtrue;
	}
}
/* CL_VR_Frame's headset watch resumes VR from WAITING_HEADSET, so a wait reports only to the console and vr_status. */
static void CL_VR_Wait( const char *failure ) {
	nextHeadsetCheck = cls.realtime + 2000;
	startupDialog = qfalse;
	if ( failure ) {
		CL_VR_Failure( failure );
		Com_Printf( "VR is waiting for the headset and runtime. Apply flatscreen mode to cancel.\n" );
		return;
	}
	Q_strncpyz( lastFailure, cachedProbe.message, sizeof( lastFailure ) );
	Com_Printf( "VR requested; waiting for a headset. Apply flatscreen mode to cancel.\n" );
}
static void CL_VR_ReportCap( void ) {
	Q_strncpyz( lastFailure, "repeated errors while in VR", sizeof( lastFailure ) );
	Com_Printf( "Repeated errors while in VR. Continuing in flatscreen.\n" );
	if ( CL_VR_Connected() )
		CL_VRModulesNoticeShow( "VR stopped after repeated errors", "Continuing in flatscreen" );
	else
		CL_VR_ErrorPopup( "VR stopped after repeated errors" );
}
static void CL_VR_Lost( const char *reason ) {
	vrAction_t action = CL_VR_Step( VREV_LOST, 0 );

	if ( action == VRACT_NONE )
		return;
	Q_strncpyz( lastFailure, reason, sizeof( lastFailure ) );
	Com_Printf( "VR session lost: %s\n", lastFailure );
	if ( re.XRSetActive )
		re.XRSetActive( qfalse );
	CL_VRInput_Reset();
	CL_VR_ClearActive();
	if ( action == VRACT_CAP )
		CL_VR_ReportCap();
}
/* Flat until the next connect, disconnect or game restart; vr_enabled keeps the player's choice. */
static void CL_VR_Unsupported( const char *reason, const char *source ) {
	char detail[MAX_QPATH + 32];

	Q_strncpyz( lastFailure, reason, sizeof( lastFailure ) );
	Com_sprintf( detail, sizeof( detail ), "%s QVMs are VR-incompatible", *source ? source : FS_GetCurrentGameDir() );
	Com_Printf( "Game modules are not VR-compatible (%s). Continuing in flatscreen.\n", lastFailure );
	// The menu has no match to draw over, so it gets the UI's error popup.
	if ( CL_VR_Connected() )
		CL_VRModulesNoticeShow( "VR unavailable", detail );
	else
		CL_VR_ErrorPopup( va( "VR unavailable - %s", detail ) );
	CL_VR_Step( VREV_UNSUPPORTED, 0 );
}
void CL_VR_ConnectionEnded( void ) {
	CL_VR_Step( VREV_CONNECTION_ENDED, cls.state == CA_CINEMATIC );
}
static void CL_VR_Status_f( void ) {
	Com_Printf( "VR state=%s; requested=%s; active=%s; restart=%s; availability=%s; last failure=%s\n",
				VRState_Name( machine.state ), CL_VR_Wanted() ? "VR" : "flat",
				VR_IsActiveMode() ? "VR" : "flat", machine.running || machine.pending != VRPENDING_NONE ? "pending" : "idle",
				cachedProbe.message[0] ? cachedProbe.message : "not probed", lastFailure );
	CL_VRModulesStatus();
}
static void CL_VR_Info_f( void ) {
	if ( !re.XRInfo ) {
		Com_Printf( "xr_info: this renderer has no OpenXR support\n" );
		return;
	}
	re.XRInfo();
}
void CL_VR_Init( void ) {
	/* Supported modes, not current headset availability. Never archived. */
#if defined(USE_VULKAN_API) && !defined(__EMSCRIPTEN__)
	Cvar_Get( "vr_availableModes", "flat vr", CVAR_ROM | CVAR_PROTECTED );
#else
	Cvar_Get( "vr_availableModes", "flat", CVAR_ROM | CVAR_PROTECTED );
#endif
	/* Mod changes reset cvars; preserve the selected mode across that reset. */
	vr_enabled = Cvar_Get( "vr_enabled", "0", CVAR_ARCHIVE | CVAR_LATCH | CVAR_NORESTART );
	Cvar_CheckRange( vr_enabled, "0", "1", CV_INTEGER );

	nextTargetCheck = cls.realtime + 1000;
	frame.open = frame.stereo = qfalse;
	xrFailed = qfalse;
	startupDialog = qfalse;
	dialogText[0] = 0;
	Com_Memset( &machine, 0, sizeof( machine ) );
	/* The first CL_StartHunkUsers builds the renderer the player's choice asks for. */
	if ( vr_enabled->integer )
		machine.pending = VRPENDING_AT_HUNK_USERS;
	trackingRequested = trackingSampled = trackingMoved = qfalse;
	forcedStartupAttempt = vr_enabled->integer != 0;
	memset( &cachedProbe, 0, sizeof( cachedProbe ) );
	lastFailure[0] = 0;
	CL_VRInput_Init();
	CL_VR_ClearActive();
	Cmd_AddCommand( "vr_status", CL_VR_Status_f );
	Cmd_AddCommand( "xr_info", CL_VR_Info_f );
}
int CL_VR_Gametype( void ) {
	return clc.serverGametype;
}
/* Decides what this restart builds from the player's choice and the headset. */
void CL_VR_RestartBegin( void ) {
	vrAction_t action;

	vr_enabled = Cvar_Get( "vr_enabled", "0",
							CVAR_ARCHIVE | CVAR_LATCH | CVAR_NORESTART ); /* apply requested value */
	action = CL_VR_Step( VREV_BEGIN, 0 );
	if ( action != VRACT_PROBE || !trackingRequested )
		trackingSampled = trackingMoved = qfalse;
	trackingRequested = action == VRACT_PROBE;
	XRLoader_WatchEnd();
	if ( action == VRACT_PROBE ) {
		vrProbe_t probe = re.XRStatus && re.XRStatus() > 0 ? VRPROBE_READY : CL_VR_WatchBegin();

		action = CL_VR_Step( VREV_PROBED, probe );
		if ( action == VRACT_INCAPABLE )
			CL_VR_Incapable();
		else if ( action == VRACT_WAIT )
			CL_VR_Wait( NULL );
	} else if ( action == VRACT_DEFER ) {
		Com_Printf( "VR resumes at the next match or the main menu.\n" );
	}
	if ( !machine.xrRenderer )
		CL_VRModulesReset();
	CL_VR_EndFrame();
	CL_VRInput_Reset();
	CL_VR_ClearActive();
	vr.sp_intermission_active = qfalse;
	CL_VRInput_SetVirtualScreen( qfalse );
}
void CL_VR_PrepareRenderer( void ) {
	if ( machine.xrRenderer && (!re.XRPrepareInit || !re.XRPrepareInit( qtrue )) ) {
		xrFailed = qtrue;
		CL_RendererError( ERR_DROP, "VR initialization failed: %s",
							re.XRLastError ? re.XRLastError() : "backend unavailable" );
	}
}
void CL_VR_CGameLoading( void ) {
	/* The replacement renderer and UI are ready. Activate before CG_INIT
	 * registers its VR mirror, so nested loading updates use screen mode.
	 * Rendering remains blocked until cgame has registered its VR interface. */
	if ( !machine.running || !machine.xrRenderer || !cls.rendererStarted || !VM_VRRegistered( uivm ) )
		return;
	if ( !re.XRSetActive || !re.XRSetActive( qtrue ) ) {
		xrFailed = qtrue;
		CL_RendererError( ERR_DROP, "VR loading activation failed: %s",
							re.XRLastError ? re.XRLastError() : "backend unavailable" );
	}
	CL_VR_SetMode( qtrue );
}
qboolean CL_VR_RestartComplete( void ) {
	vrModuleVerdict_t verdict = VRMOD_OK;

	if ( machine.xrRenderer ) {
		const char *reason, *source;
		verdict = CL_VRModulesCheck( &reason, &source );
		if ( verdict == VRMOD_UNSUPPORTED ) {
			CL_VR_Unsupported( reason, source );
			return qfalse;
		}
		if ( !re.XRSetActive || !re.XRSetActive( qtrue ) ) {
			xrFailed = qtrue;
			Q_strncpyz( lastFailure, re.XRLastError ? re.XRLastError() : "VR activation failed",
						sizeof( lastFailure ) );
			return qfalse;
		}
	}
	if ( CL_VR_Step( VREV_COMPLETE, verdict ) == VRACT_ACTIVE ) {
		CL_VR_SetMode( qtrue );
		if ( Key_GetCatcher() & (KEYCATCH_CONSOLE | KEYCATCH_MESSAGE) )
			VKeyboard_Show();
		lastFailure[0] = 0;
		forcedStartupAttempt = qfalse;
		startupDialog = qfalse;
	}
	nextTargetCheck = cls.realtime + 1000;
	return qtrue;
}
/* Unwinds a failed VR attempt so the caller's flat rebuild starts clean. */
static void CL_VR_LeaveRestart( void ) {
	XRLoader_WatchEnd();
	frame.open = frame.stereo = qfalse;
	CL_VRModulesReset();
	CL_VRInput_Reset();
	CL_VR_ClearActive();
}
/* qtrue: begin the restart again in VR. qfalse: rebuild flat in the state set here. */
qboolean CL_VR_RestartFailed( const char *failure, qboolean retried ) {
	vrEvent_t ev;
	vrAction_t action;

	Com_Memset( &ev, 0, sizeof( ev ) );
	ev.type = VREV_FAILED;
	ev.value = xrFailed || (re.XRStatus && re.XRStatus() < 0) ||
		(failure && CL_RendererFailureCode() == ERR_FATAL);
	ev.retried = retried;
	xrFailed = qfalse;
	if ( failure && machine.state != VRSTATE_UNSUPPORTED )
		Q_strncpyz( lastFailure, failure, sizeof( lastFailure ) );
	action = CL_VR_Send( &ev );
	if ( action == VRACT_RETRY ) {
		Com_Printf( "VR restart failed: %s. Retrying.\n", lastFailure );
		return qtrue;
	}
	if ( action != VRACT_WAIT )
		trackingRequested = qfalse;
	CL_VR_LeaveRestart();
	if ( action == VRACT_WAIT )
		CL_VR_Wait( lastFailure );
	else if ( action == VRACT_CAP )
		CL_VR_ReportCap();
	return qfalse;
}
qboolean CL_VR_ModePending( void ) {
	return VRState_RestartQueued( &machine ) || (vr_enabled && vr_enabled->latchedString);
}
qboolean CL_VR_RestartWantsVR( void ) {
	return machine.xrRenderer;
}
/* CL_StartHunkUsers rebuilds through CL_Vid_Restart once the new game's configuration has run. */
void CL_VR_BeginGameSwitch( void ) {
	if ( CL_VR_Step( VREV_SWITCH_BEGIN, 0 ) != VRACT_TEARDOWN )
		return;
	if ( re.XRSetActive )
		re.XRSetActive( qfalse );
	CL_VR_EndFrame();
	CL_VRInput_Reset();
	CL_VR_ClearActive();
}
qboolean CL_VR_RenderingBlocked( void ) {
	return (machine.running && !frame.stereo) || VRState_RestartQueued( &machine ) || (VR_IsActiveMode() && !frame.stereo);
}
static void CL_VR_FrameChecks( void ) {
	vrModuleVerdict_t verdict;
	vrProbe_t probe;
	const char *reason, *source;

	if ( startupDialog && cls.rendererStarted && uivm ) {
		int choice;
		startupDialog = qfalse;
		choice = Sys_VRFailureDialog( dialogText );
		if ( choice == 1 ) {
			Cvar_Set( "vr_enabled", "1" );
			CL_VR_Step( VREV_DIALOG, qtrue );
		} else if ( choice == 2 ) {
			Cbuf_AddText( "quit\n" );
		} else {
			/* Flatscreen here is the player's choice to stop asking for VR. */
			forcedStartupAttempt = qfalse;
			Cvar_Set2( "vr_enabled", "0", qtrue );
			XRLoader_WatchEnd();
			CL_VR_Step( VREV_DIALOG, qfalse );
		}
		return;
	}
	if ( machine.state == VRSTATE_WAITING_HEADSET ) {
		/* Respect a staged flat selection immediately. */
		if ( !CL_VR_Wanted() ) {
			XRLoader_WatchEnd();
			CL_VR_Step( VREV_HEADSET, VRPROBE_WAIT );
			return;
		}
		/* Never restart modules in the middle of loading/connecting. */
		if ( cls.state != CA_ACTIVE && cls.state != CA_DISCONNECTED )
			return;
		if ( cls.realtime < nextHeadsetCheck )
			return;
		nextHeadsetCheck = cls.realtime + 2000;
		cachedProbe.status = XRLoader_WatchPoll( &cachedProbe );
		probe = VRState_Probe( cachedProbe.status, cachedProbe.vulkanEnable2 );
		if ( probe == VRPROBE_READY ) {
			XRLoader_WatchEnd();
			Com_DPrintf( "OpenXR headset appeared; restarting video for VR\n" );
		}
		if ( CL_VR_Step( VREV_HEADSET, probe ) == VRACT_INCAPABLE )
			CL_VR_Incapable();
		else if ( probe == VRPROBE_WAIT )
			Q_strncpyz( lastFailure, cachedProbe.message, sizeof( lastFailure ) );
		return;
	}
	if ( !VR_IsActiveMode() )
		return;
	verdict = CL_VRModulesCheck( &reason, &source );
	if ( verdict == VRMOD_UNSUPPORTED ) {
		if ( re.XRSetActive )
			re.XRSetActive( qfalse );
		CL_VRInput_Reset();
		CL_VR_ClearActive();
		CL_VR_Unsupported( reason, source );
		return;
	}
	if ( verdict == VRMOD_FALLBACK ) {
		CL_VR_Step( VREV_REQUEST, VRPENDING_AT_SAFE_POINT );
		return;
	}
	if ( re.XRResolutionChanged && cls.realtime >= nextTargetCheck &&
		(cls.state == CA_ACTIVE || cls.state == CA_DISCONNECTED) ) {
		nextTargetCheck = cls.realtime + 1000;
		if ( re.XRResolutionChanged() ) {
			Com_DPrintf( "OpenXR eye resolution changed; restarting video for VR\n" );
			CL_VR_Step( VREV_REQUEST, VRPENDING_NOW );
			return;
		}
	}
	if ( re.XRStatus && re.XRStatus() == 1 && re.XRSetActive )
		re.XRSetActive( qtrue );
}
void CL_VR_Frame( void ) {
	vrAction_t action;

	if ( frame.open )
		return;
	if ( VRState_Idle( &machine ) )
		CL_VR_FrameChecks();
	action = CL_VR_Step( VREV_FRAME, 0 );
	if ( action == VRACT_CAP ) {
		CL_VR_ReportCap();
		action = VRACT_RESTART;
	}
	if ( action == VRACT_RESTART )
		CL_Vid_Restart( REF_DESTROY_WINDOW );
}
void CL_VR_FrameDone( void ) {
	if ( VRState_RestartQueued( &machine ) )
		CL_VR_Frame();
}

static qboolean resetScreen;
static connstate_t screenConnectionState;
void CL_VR_ResetVirtualScreen( void ) {
	resetScreen = qtrue;
}
static qboolean CL_VR_UseVirtualScreen( void ) {
	qboolean menu = (Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CONSOLE)) != 0;
	qboolean intermission = cl.snap.ps.pm_type == PM_INTERMISSION;
	if ( intermission && CL_VR_Gametype() == GT_SINGLE_PLAYER ) {
		if ( !vr.sp_intermission_active ) {
			const float *q = xrFrame.head.orientation;
			vr.sp_intermission_yaw =
				atan2f( 2 * (q[3] * q[1] + q[2] * q[0]), 1 - 2 * (q[0] * q[0] + q[1] * q[1]) ) * 180 /
				(float)M_PI;
		}
		vr.sp_intermission_active = qtrue;
	} else
		vr.sp_intermission_active = qfalse;
	vr.in_menu = menu;
	vr.first_person_following = ((cl.snap.ps.pm_flags & PMF_FOLLOW) || clc.demoplaying) &&
								vr.follow_mode == VRFM_FIRSTPERSON;
	if ( menu && !(intermission && CL_VR_Gametype() == GT_SINGLE_PLAYER) )
		return qtrue;
	if ( intermission && cls.state == CA_ACTIVE )
		return qfalse;
	return cls.state != CA_ACTIVE || vr.first_person_following;
}
static qboolean CL_VR_BeginFrameInternal( qboolean updateInput ) {
	int result;

	frame.stereo = qfalse;
	frame.open = qfalse;
	if ( machine.xrTainted ) {
		return !VRState_BlocksRendering( &machine );
	}
	if ( !re.XRStatus || re.XRStatus() <= 0 ) {
		if ( VR_IsActiveMode() )
			CL_VR_Lost( "VR session unavailable" );
		return qtrue;
	}

	result = re.XRBeginFrame( &xrFrame );
	if ( result < 0 ) {
		CL_VR_Lost( re.XRLastError ? re.XRLastError() : "VR session ended" );
		return qtrue;
	}

	frame.open = qtrue;
	frame.stereo = VR_IsActiveMode() && xrFrame.renderable;
	CL_VR_CheckTracking();
	CL_VRInput_SetVirtualScreen( VR_IsActiveMode() && CL_VR_UseVirtualScreen() );
	if ( re.XRSetVirtualScreen ) {
		/* A locked menu yaw (timeline scrub) suppresses the connection-state re-anchor, but the
		 * state is still tracked so the edge isn't replayed once the lock lifts. */
		if ( resetScreen || (screenConnectionState != cls.state && !vr.menuYawLocked) ) {
			re.XRSetVirtualScreen( qfalse, vr.menuYawLocked, &xrFrame );
		}
		resetScreen = qfalse;
		screenConnectionState = cls.state;
		re.XRSetVirtualScreen( vr.virtual_screen, vr.menuYawLocked, &xrFrame );
	}
	if ( !vr.menuYawLocked && xrFrame.screen.visible ) {
		vr.menuYaw = xrFrame.screen.yaw * 180 / (float)M_PI;
	}
	if ( updateInput ) {
		qboolean nextScreen;
		CL_VRInput_Frame( &xrFrame );
		/* Controller Escape can change the UI catcher during input dispatch.
		 * Keep the pre-input screen for cursor hit tests, then publish the
		 * resulting menu state before this frame is drawn. */
		nextScreen = VR_IsActiveMode() && CL_VR_UseVirtualScreen();
		if ( nextScreen != vr.virtual_screen ) {
			CL_VRInput_SetVirtualScreen( nextScreen );
			if ( re.XRSetVirtualScreen ) {
				re.XRSetVirtualScreen( nextScreen, vr.menuYawLocked, &xrFrame );
			}
			if ( !vr.menuYawLocked && xrFrame.screen.visible ) {
				vr.menuYaw = xrFrame.screen.yaw * 180 / (float)M_PI;
			}
		}
	}
	if ( re.XRSetZoom ) {
		re.XRSetZoom( vr.weapon_zoomed, &vr.weapon_zoomLevel );
	}
	return !VR_IsActiveMode() || frame.stereo || CL_VR_DesktopWaitingFrame();
}
qboolean CL_VR_BeginFrame( void ) {
	return CL_VR_BeginFrameInternal( qtrue );
}
qboolean CL_VR_BeginLoadingFrame( void ) {
	if ( frame.open || VRState_RestartQueued( &machine ) || machine.xrTainted || !VR_IsActiveMode() ||
		!cls.rendererStarted || (cls.state != CA_LOADING && cls.state != CA_PRIMED) )
		return qfalse;
	if ( machine.running && (!machine.xrRenderer || !VM_VRRegistered( uivm ) || !VM_VRRegistered( cgvm )) )
		return qfalse;
	/* Loading callbacks can run inside CG_INIT. Update the tracked screen,
	 * but never dispatch controller events into a partially initialized VM. */
	CL_VR_BeginFrameInternal( qfalse );
	return frame.open;
}
void CL_VR_EndFrame( void ) {
	if ( frame.open && re.XREndFrame && re.XREndFrame() < 0 )
		CL_VR_Lost( "VR frame submission failed" );
	frame.open = frame.stereo = qfalse;
}
void CL_VR_ResetForError( void ) {
	CL_VR_Step( VREV_ERROR, 0 );
	xrFailed = qfalse;
	XRLoader_WatchEnd();
	if ( machine.state != VRSTATE_WAITING_HEADSET )
		trackingRequested = qfalse;
	CL_VR_ClearActive();
	CL_VRInput_Reset();
	vr.sp_intermission_active = qfalse;
	CL_VRInput_SetVirtualScreen( qfalse );
	memset( &xrFrame, 0, sizeof( xrFrame ) );
}
void CL_VR_AbortUnwind( qboolean keepRenderer ) {
	if ( keepRenderer )
		CL_VR_EndFrame();
	frame.open = frame.stereo = qfalse;
	CL_VR_Step( VREV_ABORT, 0 );
}
/* The renderer is untouched, so an interrupted transition resumes at the next safe point. */
void CL_VR_RestartAborted( void ) {
	CL_VR_Step( VREV_DISCONNECT_ERROR, 0 );
}
qboolean CL_VR_RenderStereo( void ) {
	return frame.stereo;
}
qboolean CL_VR_RebuildPending( void ) {
	return VRState_HoldsRestarts( &machine );
}
qboolean CL_VR_ConsumeRebuildRequest( void ) {
	return CL_VR_Step( VREV_HUNK_USERS, 0 ) == VRACT_RESTART;
}
void CL_VR_Shutdown( void ) {
	Com_Memset( &machine, 0, sizeof( machine ) );
	trackingRequested = qfalse;
	XRLoader_WatchEnd();
	forcedStartupAttempt = qfalse;
	startupDialog = qfalse;
	CL_VR_EndFrame();
	CL_VRModulesReset();
	CL_VRInput_Reset();
	CL_VR_ClearActive();
	CL_VRInput_Shutdown();
	Cmd_RemoveCommand( "vr_status" );
	Cmd_RemoveCommand( "xr_info" );
}
