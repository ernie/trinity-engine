#include "client.h"
#include "cl_vr.h"
#include "cl_vr_input.h"
#include "cl_vr_modules.h"
#include "cl_renderer_recovery.h"
#include "../qcommon/vm_vr.h"
#include "../vrcommon/vr_state.h"
#include "../vrcommon/xr_loader.h"

static cvar_t *vr_enabled;
static qboolean restarting, preparingXR, frameOpen, stereo, restartQueued;
static qboolean errorAborted, forcedStartupAttempt, failureDialogPending;
static int errorRecoveryMode = -1;
static int recoveryCount, recoveryWindowStart;
static int nextTargetCheck, nextHeadsetCheck;
static refXRFrame_t xrFrame;
static qboolean waitingForHeadset;
static qboolean trackingRequested, trackingSampled, trackingMoved;
static refXREye_t initialTrackingPose;
qboolean CL_VR_WaitingForTracking( void ) {
	return trackingRequested && !trackingMoved && vr_enabled && vr_enabled->integer &&
			!(vr_enabled->latchedString && !atoi( vr_enabled->latchedString ));
}
qboolean CL_VR_DesktopWaitingFrame( void ) {
	return !restarting && !restartQueued && frameOpen && !stereo && VR_IsActiveMode() &&
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
static qboolean probeKnown;
static char lastFailure[512];
static qboolean startupPending;
static qboolean resumeVR;

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
static qboolean CL_VR_WatchAvailable( void ) {
	cachedProbe.status = XRLoader_WatchBegin( &cachedProbe );
	probeKnown = qtrue;
	return cachedProbe.status == XRLOADER_AVAILABLE && cachedProbe.vulkanEnable2;
}
static void CL_VR_Failure( const char *reason ) {
	Q_strncpyz( lastFailure, reason && *reason ? reason : "XR backend unavailable",
				sizeof( lastFailure ) );
	if ( forcedStartupAttempt )
		failureDialogPending = qtrue;
	Com_Printf( "Cannot use VR: %s. Staying in flatscreen. Retry: set vr_enabled 1; vid_restart\n",
				lastFailure );
}
static void CL_VR_Request( qboolean active ) {
	if ( !active )
		forcedStartupAttempt = failureDialogPending = qfalse;
	Cvar_Set( "vr_enabled", active ? "1" : "0" );
	if ( !restartQueued ) {
		restartQueued = qtrue;
		Cbuf_InsertText( "vid_restart\n" );
	}
}
static void CL_VR_Status_f( void ) {
	Com_Printf( "VR requested=%s; active=%s; restart=%s; availability=%s; last failure=%s\n",
				(vr_enabled->latchedString ? atoi( vr_enabled->latchedString ) : vr_enabled->integer)
					? "VR"
					: "flat",
				VR_IsActiveMode() ? "VR" : "flat", restarting || restartQueued ? "pending" : "idle",
				probeKnown ? cachedProbe.message : "not probed", lastFailure );
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
	restarting = preparingXR = frameOpen = stereo = restartQueued = qfalse;
	errorAborted = probeKnown = failureDialogPending = qfalse;
	errorRecoveryMode = -1;
	recoveryCount = recoveryWindowStart = 0;
	trackingRequested = trackingSampled = trackingMoved = qfalse;
	waitingForHeadset = qfalse;
	forcedStartupAttempt = vr_enabled->integer != 0;
	memset( &cachedProbe, 0, sizeof( cachedProbe ) );
	lastFailure[0] = 0;
	CL_VRInput_Init();
	CL_VR_ClearActive();
	Cmd_AddCommand( "vr_status", CL_VR_Status_f );
	startupPending = vr_enabled->integer != 0;
}
int CL_VR_Gametype( void ) {
	return clc.serverGametype;
}
/* Select modules before vid_restart rebuilds their renderer assets. */
void CL_VR_RestartBegin( void ) {
	restartQueued = qfalse;
	vr_enabled = Cvar_Get( "vr_enabled", "0",
							CVAR_ARCHIVE | CVAR_LATCH | CVAR_NORESTART ); /* apply requested value */
	preparingXR = vr_enabled->integer != 0;
	if ( !preparingXR || !trackingRequested )
		trackingSampled = trackingMoved = qfalse;
	trackingRequested = preparingXR;
	waitingForHeadset = qfalse;
	XRLoader_WatchEnd();
	if ( preparingXR ) {
		if ( cls.state != CA_ACTIVE && cls.state != CA_DISCONNECTED && !VR_IsActiveMode() ) {
			CL_VR_Failure( "wait for an active match or the main menu" );
			preparingXR = qfalse;
		} else if ( (!re.XRStatus || re.XRStatus() <= 0) && !CL_VR_WatchAvailable() ) {
			preparingXR = qfalse;
			if ( cachedProbe.status == XRLOADER_NO_HEADSET ) {
				waitingForHeadset = qtrue;
				nextHeadsetCheck = cls.realtime + 2000;
				failureDialogPending = qfalse;
				Q_strncpyz( lastFailure, cachedProbe.message, sizeof( lastFailure ) );
				Com_Printf( "VR requested; waiting for a headset. Apply flatscreen mode to cancel.\n" );
			} else
				CL_VR_Failure( cachedProbe.message );
		} else if ( !CL_VRModulesPreflight() || !CL_VRModulesCommit() ) {
			CL_VR_Failure( CL_VRModulesLastError() );
			preparingXR = qfalse;
		}
	}
	if ( !preparingXR ) {
		if ( !waitingForHeadset )
			Cvar_Set2( "vr_enabled", "0", qtrue );
		CL_VRModulesReset();
	}
	CL_VR_EndFrame();
	CL_VRInput_Reset();
	CL_VR_ClearActive();
	vr.virtual_screen = vr.sp_intermission_active = qfalse;
	restarting = qtrue;
	errorAborted = qfalse;
}
void CL_VR_PrepareRenderer( void ) {
	if ( preparingXR && (!re.XRPrepareInit || !re.XRPrepareInit( qtrue )) )
		CL_RendererError( ERR_DROP, "VR initialization failed: %s",
							re.XRLastError ? re.XRLastError() : "backend unavailable" );
}
void CL_VR_CGameLoading( void ) {
	/* The replacement renderer and UI are ready. Activate before CG_INIT
	 * registers its VR mirror, so nested loading updates use screen mode.
	 * Rendering remains blocked until cgame has registered its VR interface. */
	if ( !restarting || !preparingXR || !cls.rendererStarted || !VM_VRRegistered( uivm ) )
		return;
	if ( !re.XRSetActive || !re.XRSetActive( qtrue ) )
		CL_RendererError( ERR_DROP, "VR loading activation failed: %s",
							re.XRLastError ? re.XRLastError() : "backend unavailable" );
	CL_VR_SetMode( qtrue );
}
qboolean CL_VR_RestartComplete( void ) {
	if ( preparingXR ) {
		if ( !VM_VRRegistered( uivm ) || (cgvm && !VM_VRRegistered( cgvm )) ) {
			CL_VR_Failure( "reloaded modules are not VR-compatible" );
			return qfalse;
		}
		if ( !re.XRSetActive || !re.XRSetActive( qtrue ) ) {
			CL_VR_Failure( re.XRLastError ? re.XRLastError() : "VR activation failed" );
			return qfalse;
		}
		CL_VR_SetMode( qtrue );
		if ( Key_GetCatcher() & (KEYCATCH_CONSOLE | KEYCATCH_MESSAGE) )
			VKeyboard_Show();
		lastFailure[0] = 0;
		forcedStartupAttempt = failureDialogPending = qfalse;
	}
	restarting = qfalse;
	nextTargetCheck = cls.realtime + 1000;
	return qtrue;
}
void CL_VR_RestartFallback( const char *reason ) {
	trackingRequested = qfalse;
	resumeVR = qfalse;
	if ( reason && *reason )
		CL_VR_Failure( reason );
	waitingForHeadset = qfalse;
	XRLoader_WatchEnd();
	preparingXR = qfalse;
	frameOpen = stereo = qfalse;
	Cvar_Set2( "vr_enabled", "0", qtrue );
	CL_VRModulesReset();
	CL_VRInput_Reset();
	CL_VR_ClearActive();
}
qboolean CL_VR_ModePending( void ) {
	return restartQueued || (vr_enabled && vr_enabled->latchedString);
}
qboolean CL_VR_RestartWantsVR( void ) {
	return preparingXR;
}
/* The renderer rebuilds outside vid_restart, so leave VR and resume via the ordinary restart. */
void CL_VR_GameSwitch( void ) {
	if ( !VR_IsActiveMode() && !preparingXR )
		return;
	resumeVR = qtrue;
	if ( re.XRSetActive )
		re.XRSetActive( qfalse );
	CL_VR_EndFrame();
	CL_VRInput_Reset();
	CL_VR_ClearActive();
	preparingXR = restarting = qfalse;
}
qboolean CL_VR_RenderingBlocked( void ) {
	return (restarting && !stereo) || restartQueued || (VR_IsActiveMode() && !stereo);
}
void CL_VR_Frame( void ) {
	if ( restarting || restartQueued || frameOpen )
		return;
	/* Com_Error has already unwound the abandoned VM/renderer frame. Rebuild via
	 * the ordinary restart rather than submitting partially recorded work.
	 * A failure during VR startup only gets a flat recovery attempt. */
	if ( errorRecoveryMode >= 0 ) {
		qboolean recoverVR = errorRecoveryMode != 0;
		errorRecoveryMode = -1;
		if ( recoverVR ) {
			/* Prevents a persistent VR error from restarting forever. */
			if ( cls.realtime - recoveryWindowStart > 30000 ) {
				recoveryCount = 0;
				recoveryWindowStart = cls.realtime;
			}
			if ( ++recoveryCount > 2 ) {
				recoveryCount = 0;
				CL_VR_Failure( "repeated errors while in VR" );
				recoverVR = qfalse;
			}
		}
		CL_VR_Request( recoverVR );
		return;
	}
	if ( resumeVR ) {
		if ( vr_enabled->latchedString ? !atoi( vr_enabled->latchedString ) : !vr_enabled->integer ) {
			resumeVR = qfalse;
			return;
		}
		if ( cls.state != CA_ACTIVE && cls.state != CA_DISCONNECTED )
			return;
		resumeVR = qfalse;
		CL_VR_Request( qtrue );
		return;
	}
	if ( failureDialogPending && cls.rendererStarted && uivm ) {
		int choice;
		failureDialogPending = qfalse;
		choice = Sys_VRFailureDialog( lastFailure );
		if ( choice == 1 )
			CL_VR_Request( qtrue );
		else {
			forcedStartupAttempt = qfalse;
			if ( choice == 2 )
				Cbuf_AddText( "quit\n" );
		}
		return;
	}
	if ( waitingForHeadset ) {
		/* Respect a staged flat selection immediately. */
		if ( !vr_enabled->integer || (vr_enabled->latchedString && !atoi( vr_enabled->latchedString )) ) {
			waitingForHeadset = qfalse;
			XRLoader_WatchEnd();
			return;
		}
		/* Never restart modules in the middle of loading/connecting. */
		if ( cls.state != CA_ACTIVE && cls.state != CA_DISCONNECTED )
			return;
		if ( cls.realtime < nextHeadsetCheck )
			return;
		nextHeadsetCheck = cls.realtime + 2000;
		cachedProbe.status = XRLoader_WatchPoll( &cachedProbe );
		probeKnown = qtrue;
		if ( cachedProbe.status == XRLOADER_AVAILABLE && cachedProbe.vulkanEnable2 ) {
			waitingForHeadset = qfalse;
			XRLoader_WatchEnd();
			Com_DPrintf( "OpenXR headset appeared; restarting video for VR\n" );
			CL_VR_Request( qtrue );
		} else if ( cachedProbe.status != XRLOADER_NO_HEADSET ) {
			waitingForHeadset = qfalse;
			XRLoader_WatchEnd();
			Cvar_Set2( "vr_enabled", "0", qtrue );
			CL_VR_Failure( cachedProbe.message );
		}
		return;
	}
	if ( !VR_IsActiveMode() )
		return;
	/* Disconnect clears connection-specific native selection, so the ordinary UI
	 * reload may pick a non-VR QVM. At the main menu that calls for preflight and
	 * reload, not for leaving VR; preflight enforces the game-directory
	 * restrictions and handles an unavailable fallback. */
	if ( cls.state == CA_DISCONNECTED && uivm && !VM_VRRegistered( uivm ) ) {
		CL_VR_Request( qtrue );
		return;
	}
	if ( CL_VRModulesNeedsRestart() ) {
		/* A new QVM could only be classified after CG_INIT. Reconstruct its
		 * already-loaded world before initializing the native replacement. */
		if ( cls.state == CA_ACTIVE )
			CL_VR_Request( qtrue );
		return;
	}
	/* Map/connect transitions temporarily unload modules. Only a loaded,
	 * incompatible module requires falling back; absence during reload does not. */
	if ( (uivm && !VM_VRRegistered( uivm )) || (cgvm && !VM_VRRegistered( cgvm )) ||
		(cl_connectedToPureServer && ((uivm && uivm->entryPoint) || (cgvm && cgvm->entryPoint))) ) {
		if ( re.XRSetActive )
			re.XRSetActive( qfalse );
		CL_VRInput_Reset();
		CL_VR_ClearActive();
		CL_VR_Request( qfalse );
		return;
	}
	if ( re.XRResolutionChanged && cls.realtime >= nextTargetCheck &&
		(cls.state == CA_ACTIVE || cls.state == CA_DISCONNECTED) ) {
		nextTargetCheck = cls.realtime + 1000;
		if ( re.XRResolutionChanged() ) {
			Com_DPrintf( "OpenXR eye resolution changed; restarting video for VR\n" );
			CL_VR_Request( qtrue );
			return;
		}
	}
	if ( re.XRStatus && re.XRStatus() == 1 && re.XRSetActive )
		re.XRSetActive( qtrue );
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

	stereo = qfalse;
	frameOpen = qfalse;
	if ( errorAborted ) {
		return errorRecoveryMode < 0 && !restartQueued;
	}
	if ( !re.XRStatus || re.XRStatus() <= 0 ) {
		if ( VR_IsActiveMode() ) {
			CL_VR_Failure( "VR session unavailable" );
			CL_VR_Request( qfalse );
			CL_VRInput_Reset();
			CL_VR_ClearActive();
		}
		return qtrue;
	}

	result = re.XRBeginFrame( &xrFrame );
	if ( result < 0 ) {
		CL_VR_Request( qfalse );
		CL_VR_Failure( re.XRLastError ? re.XRLastError() : "VR session ended" );
		if ( re.XRSetActive ) {
			re.XRSetActive( qfalse );
		}
		CL_VRInput_Reset();
		CL_VR_ClearActive();
		return qtrue;
	}

	frameOpen = qtrue;
	stereo = VR_IsActiveMode() && xrFrame.renderable;
	CL_VR_CheckTracking();
	vr.virtual_screen = VR_IsActiveMode() && CL_VR_UseVirtualScreen();
	if ( re.XRSetVirtualScreen ) {
		if ( resetScreen || screenConnectionState != cls.state ) {
			re.XRSetVirtualScreen( qfalse, &xrFrame );
		}
		resetScreen = qfalse;
		screenConnectionState = cls.state;
		re.XRSetVirtualScreen( vr.virtual_screen, &xrFrame );
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
			vr.virtual_screen = nextScreen;
			if ( re.XRSetVirtualScreen ) {
				re.XRSetVirtualScreen( nextScreen, &xrFrame );
			}
			if ( !vr.menuYawLocked && xrFrame.screen.visible ) {
				vr.menuYaw = xrFrame.screen.yaw * 180 / (float)M_PI;
			}
		}
	}
	if ( re.XRSetZoom ) {
		re.XRSetZoom( vr.weapon_zoomed, &vr.weapon_zoomLevel );
	}
	return !VR_IsActiveMode() || stereo || CL_VR_DesktopWaitingFrame();
}
qboolean CL_VR_BeginFrame( void ) {
	return CL_VR_BeginFrameInternal( qtrue );
}
qboolean CL_VR_BeginLoadingFrame( void ) {
	if ( frameOpen || restartQueued || errorAborted || !VR_IsActiveMode() || !cls.rendererStarted ||
		(cls.state != CA_LOADING && cls.state != CA_PRIMED) )
		return qfalse;
	if ( restarting && (!preparingXR || !VM_VRRegistered( uivm ) || !VM_VRRegistered( cgvm )) )
		return qfalse;
	/* Loading callbacks can run inside CG_INIT. Update the tracked screen,
	 * but never dispatch controller events into a partially initialized VM. */
	CL_VR_BeginFrameInternal( qfalse );
	return frameOpen;
}
void CL_VR_EndFrame( void ) {
	if ( frameOpen && re.XREndFrame && re.XREndFrame() < 0 ) {
		CL_VR_Request( qfalse );
		CL_VR_Failure( "VR frame submission failed" );
		if ( re.XRSetActive )
			re.XRSetActive( qfalse );
		CL_VRInput_Reset();
		CL_VR_ClearActive();
	}
	frameOpen = stereo = qfalse;
}
void CL_VR_ResetForError( void ) {
	errorRecoveryMode =
		(VR_IsActiveMode() || preparingXR) ? (VR_IsActiveMode() && !restarting ? 1 : 0) : -1;
	CL_RendererRecoveryReset();
	restarting = preparingXR = frameOpen = stereo = restartQueued = qfalse;
	waitingForHeadset = qfalse;
	XRLoader_WatchEnd();
	trackingRequested = qfalse;
	resumeVR = qfalse;
	errorAborted = qtrue;
	if ( vr_enabled )
		Cvar_Set2( "vr_enabled", "0", qtrue );
	CL_VR_ClearActive();
	CL_VRInput_Reset();
	vr.virtual_screen = vr.sp_intermission_active = qfalse;
	memset( &xrFrame, 0, sizeof( xrFrame ) );
	if ( forcedStartupAttempt )
		failureDialogPending = qtrue;
}
/* The renderer is untouched; only the recovery guard needs releasing. */
void CL_VR_RestartAborted( void ) {
	CL_RendererRecoveryReset();
	CL_VR_EndFrame();
	if ( preparingXR && !VR_IsActiveMode() )
		resumeVR = qtrue;
	restarting = preparingXR = restartQueued = qfalse;
	frameOpen = stereo = qfalse;
}
qboolean CL_VR_RenderStereo( void ) {
	return stereo;
}
qboolean CL_VR_ConsumeStartupRequest( void ) {
	qboolean pending = startupPending;
	startupPending = qfalse;
	return pending;
}
void CL_VR_Shutdown( void ) {
	trackingRequested = qfalse;
	waitingForHeadset = qfalse;
	XRLoader_WatchEnd();
	resumeVR = qfalse;
	forcedStartupAttempt = failureDialogPending = qfalse;
	CL_VR_EndFrame();
	CL_VRModulesReset();
	restarting = preparingXR = restartQueued = qfalse;
	CL_VRInput_Reset();
	CL_VR_ClearActive();
	CL_VRInput_Shutdown();
	Cmd_RemoveCommand( "vr_status" );
}
