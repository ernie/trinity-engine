#include "cl_vr_state.h"

const char *VRState_Name( vrState_t state ) {
	switch ( state ) {
		case VRSTATE_FLAT:
			return "FLAT";
		case VRSTATE_VR:
			return "VR";
		case VRSTATE_TRANSITION:
			return "VR_TRANSITION";
		case VRSTATE_WAITING_HEADSET:
			return "WAITING_HEADSET";
		case VRSTATE_UNSUPPORTED:
			return "VR_UNSUPPORTED";
	}
	return "UNKNOWN";
}

vrBegin_t VRState_Begin( qboolean wanted, qboolean capped, qboolean unsupported, qboolean safePoint ) {
	if ( !wanted || capped )
		return VRBEGIN_FLAT;
	if ( unsupported )
		return VRBEGIN_UNSUPPORTED;
	if ( !safePoint )
		return VRBEGIN_DEFER;
	return VRBEGIN_PROBE;
}

qboolean VRState_CountRecovery( vrRecoveryCap_t *cap, int now ) {
	if ( now - cap->windowStart > 30000 ) {
		cap->count = 0;
		cap->windowStart = now;
	}
	if ( ++cap->count > 2 ) {
		cap->count = 0;
		return qtrue;
	}
	return qfalse;
}

qboolean VRState_Resumes( qboolean wanted, vrState_t state ) {
	return wanted && (state == VRSTATE_VR || state == VRSTATE_TRANSITION || state == VRSTATE_UNSUPPORTED);
}

vrFailAction_t VRState_RestartFailed( qboolean xrFailure, qboolean retried, qboolean capFired ) {
	if ( capFired )
		return VRFAIL_FLAT;
	if ( xrFailure || retried )
		return VRFAIL_WAIT_HEADSET;
	return VRFAIL_RETRY_VR;
}

vrProbe_t VRState_Probe( xrLoaderStatus_t status, qboolean vulkanEnable2 ) {
	switch ( status ) {
		case XRLOADER_AVAILABLE:
			return vulkanEnable2 ? VRPROBE_READY : VRPROBE_INCAPABLE;
		case XRLOADER_NO_HEADSET:
			/* Enumeration has already shown the runtime's Vulkan bindings. */
			return vulkanEnable2 ? VRPROBE_WAIT : VRPROBE_INCAPABLE;
		case XRLOADER_NO_RUNTIME:
		case XRLOADER_PROBE_FAILED:
			return VRPROBE_WAIT;
		case XRLOADER_NO_LOADER:
		case XRLOADER_NO_VULKAN:
			break;
	}
	return VRPROBE_INCAPABLE;
}

vrModuleVerdict_t VRState_ModuleVerdict( const vrModuleCheck_t *check, const char **reason ) {
	*reason = "";
	if ( check->registered )
		return VRMOD_OK;
	if ( check->provisional )
		return VRMOD_WAIT;
	if ( check->pure ) {
		*reason = "the pure server's QVM has no VR support";
		return VRMOD_UNSUPPORTED;
	}
	if ( !check->baseGame ) {
		*reason = "this mod has no VR-compatible module";
		return VRMOD_UNSUPPORTED;
	}
	if ( check->fallbackRunning ) {
		*reason = "the bundled VR module did not register";
		return VRMOD_UNSUPPORTED;
	}
	if ( !check->fallbackAvailable ) {
		*reason = "the bundled VR module is unavailable";
		return VRMOD_UNSUPPORTED;
	}
	return VRMOD_FALLBACK;
}

qboolean VRState_ModChangeRetries( vrState_t state, qboolean disconnected, qboolean modChangePending ) {
	return state == VRSTATE_UNSUPPORTED && disconnected && modChangePending;
}

qboolean VRState_SafePoint( qboolean active, qboolean disconnected, qboolean vrActive, qboolean gameSwitch ) {
	return active || disconnected || vrActive || gameSwitch;
}

vrPending_t VRState_Request( vrPending_t pending, vrPending_t request ) {
	return request > pending ? request : pending;
}

qboolean VRState_RestartQueued( const vrMachine_t *m ) {
	return m->pending == VRPENDING_NOW || m->pending == VRPENDING_NOW_FLAT;
}

qboolean VRState_HoldsRestarts( const vrMachine_t *m ) {
	return m->pending == VRPENDING_AT_HUNK_USERS;
}

qboolean VRState_Idle( const vrMachine_t *m ) {
	return m->pending == VRPENDING_NONE && !m->running;
}

qboolean VRState_BlocksRendering( const vrMachine_t *m ) {
	return m->xrTainted && (m->pending == VRPENDING_AFTER_ERROR || VRState_RestartQueued( m ));
}

static vrAction_t VRState_OnError( vrMachine_t *m, const vrEvent_t *ev ) {
	qboolean resumes = VRState_Resumes( ev->wanted, m->state );

	if ( resumes && m->state == VRSTATE_VR )
		m->resuming = qtrue;
	/* A cap restart still rebuilds flat, and the restart after it probes VR again. */
	if ( resumes )
		m->pending = VRPENDING_AFTER_ERROR;
	/* A staged flat choice with the XR renderer loaded applies its latch through a restart. */
	else if ( m->pending != VRPENDING_NOW_FLAT )
		m->pending = m->xrRenderer ? VRPENDING_NOW : VRPENDING_NONE;
	m->xrRenderer = qfalse;
	m->xrTainted = qtrue;
	if ( resumes )
		m->state = VRSTATE_TRANSITION;
	else if ( m->state != VRSTATE_WAITING_HEADSET )
		m->state = VRSTATE_FLAT;
	return VRACT_NONE;
}

static vrAction_t VRState_OnSwitchBegin( vrMachine_t *m, const vrEvent_t *ev ) {
	qboolean resume;

	/* The switch's own renderer start builds the flat renderer the cap asked for. */
	if ( m->pending == VRPENDING_NOW_FLAT ) {
		m->pending = VRPENDING_NONE;
		m->xrRenderer = qfalse;
	}
	resume = VRState_Resumes( ev->wanted, m->state );
	if ( !resume && !ev->vrActive && !m->xrRenderer )
		return VRACT_NONE;
	if ( resume && m->state == VRSTATE_VR )
		m->resuming = qtrue;
	/* CL_ShutdownRef compares this with rendererWasXR to pick the DLL unload. */
	m->xrRenderer = resume;
	m->pending = VRPENDING_AT_HUNK_USERS;
	m->state = resume ? VRSTATE_TRANSITION : VRSTATE_FLAT;
	return VRACT_TEARDOWN;
}

static vrAction_t VRState_OnFrame( vrMachine_t *m, const vrEvent_t *ev ) {
	if ( m->running )
		return VRACT_NONE;
	/* Com_Error abandoned a partially recorded frame, so rebuild through a full restart. */
	if ( m->pending == VRPENDING_AFTER_ERROR ) {
		if ( VRState_CountRecovery( &m->cap, ev->now ) ) {
			m->pending = VRPENDING_NOW_FLAT;
			m->state = VRSTATE_FLAT;
			return VRACT_CAP;
		}
		m->pending = VRPENDING_NOW;
	}
	if ( VRState_RestartQueued( m ) )
		return VRACT_RESTART;
	if ( m->pending != VRPENDING_AT_SAFE_POINT )
		return VRACT_NONE;
	/* A transition still waiting for its safe point honors a staged flat choice at once. */
	if ( m->state == VRSTATE_TRANSITION && !ev->wanted ) {
		m->pending = VRPENDING_NONE;
		m->state = VRSTATE_FLAT;
		return VRACT_NONE;
	}
	return ev->settled ? VRACT_RESTART : VRACT_NONE;
}

static vrAction_t VRState_OnBegin( vrMachine_t *m, const vrEvent_t *ev ) {
	qboolean capped = m->pending == VRPENDING_NOW_FLAT;
	vrBegin_t begin;

	/* This restart performs whatever was pending, including a switch rebuild. */
	m->pending = VRPENDING_NONE;
	/* A modified fs_game makes this restart's FS_ConditionalRestart switch mods. */
	if ( VRState_ModChangeRetries( m->state, ev->disconnected, ev->modChange ) )
		m->state = VRSTATE_TRANSITION;
	begin = VRState_Begin( ev->wanted, capped, m->state == VRSTATE_UNSUPPORTED, ev->safePoint );
	m->running = qtrue;
	m->xrRenderer = m->xrTainted = qfalse;
	if ( begin == VRBEGIN_PROBE )
		return VRACT_PROBE;
	if ( begin == VRBEGIN_DEFER ) {
		m->pending = VRPENDING_AT_SAFE_POINT;
		m->state = VRSTATE_TRANSITION;
		return VRACT_DEFER;
	}
	if ( begin == VRBEGIN_FLAT )
		m->state = VRSTATE_FLAT;
	return VRACT_NONE;
}

static vrAction_t VRState_OnFailed( vrMachine_t *m, const vrEvent_t *ev ) {
	vrFailAction_t fail;

	if ( m->state == VRSTATE_UNSUPPORTED ) {
		m->xrRenderer = qfalse;
		return VRACT_LEAVE;
	}
	fail = VRState_RestartFailed( ev->value != 0, ev->retried, VRState_CountRecovery( &m->cap, ev->now ) );
	if ( fail == VRFAIL_RETRY_VR )
		return VRACT_RETRY;
	m->xrRenderer = qfalse;
	if ( fail == VRFAIL_WAIT_HEADSET ) {
		m->state = VRSTATE_WAITING_HEADSET;
		return VRACT_WAIT;
	}
	m->state = VRSTATE_FLAT;
	return VRACT_CAP;
}

static vrAction_t VRState_ReduceEvent( vrMachine_t *m, const vrEvent_t *ev ) {
	switch ( ev->type ) {
		case VREV_ABORT:
			m->running = qfalse;
			return VRACT_NONE;
		case VREV_ERROR:
			return VRState_OnError( m, ev );
		case VREV_DISCONNECT_ERROR:
			/* A disconnect defers a switch rebuild or transition to the next safe point; a pending error recovery stays. */
			if ( m->pending == VRPENDING_AT_HUNK_USERS ||
				(m->state == VRSTATE_TRANSITION && m->pending != VRPENDING_AFTER_ERROR) ) {
				m->pending = VRPENDING_AT_SAFE_POINT;
				m->state = VRSTATE_TRANSITION;
			}
			/* The XR renderer survives a disconnect in VR, so later module loads still select VR modules. */
			if ( !ev->vrActive )
				m->xrRenderer = qfalse;
			return VRACT_NONE;
		case VREV_SWITCH_BEGIN:
			return VRState_OnSwitchBegin( m, ev );
		case VREV_HUNK_USERS:
			if ( m->pending != VRPENDING_AT_HUNK_USERS )
				return VRACT_NONE;
			m->pending = VRPENDING_NONE;
			return VRACT_RESTART;
		case VREV_FRAME:
			return VRState_OnFrame( m, ev );
		case VREV_BEGIN:
			return VRState_OnBegin( m, ev );
		case VREV_PROBED:
			if ( ev->value == VRPROBE_READY ) {
				m->xrRenderer = qtrue;
				m->state = VRSTATE_TRANSITION;
				return VRACT_NONE;
			}
			/* A fresh request without a headset falls back like a broken install; only rebuilding VR that ran waits. */
			if ( ev->value == VRPROBE_WAIT && m->resuming ) {
				m->state = VRSTATE_WAITING_HEADSET;
				return VRACT_WAIT;
			}
			m->state = VRSTATE_FLAT;
			return VRACT_INCAPABLE;
		case VREV_COMPLETE:
			/* A QVM's VR compatibility is known only after its INIT, when its world is already loaded. */
			if ( ev->value == VRMOD_FALLBACK )
				m->pending = VRState_Request( m->pending, VRPENDING_AT_SAFE_POINT );
			m->running = qfalse;
			if ( !m->xrRenderer )
				return VRACT_NONE;
			m->state = VRSTATE_VR;
			return VRACT_ACTIVE;
		case VREV_FAILED:
			return VRState_OnFailed( m, ev );
		case VREV_LOST:
			if ( m->state != VRSTATE_VR )
				return VRACT_NONE;
			if ( VRState_CountRecovery( &m->cap, ev->now ) ) {
				m->state = VRSTATE_FLAT;
				m->pending = VRState_Request( m->pending, VRPENDING_NOW_FLAT );
				return VRACT_CAP;
			}
			/* The restart probes the runtime and waits for it when it stays away. */
			m->state = VRSTATE_TRANSITION;
			m->resuming = qtrue;
			m->pending = VRState_Request( m->pending, VRPENDING_NOW );
			return VRACT_TEARDOWN;
		case VREV_HEADSET:
			if ( !ev->wanted || ev->value == VRPROBE_INCAPABLE ) {
				m->state = VRSTATE_FLAT;
				return ev->wanted ? VRACT_INCAPABLE : VRACT_NONE;
			}
			if ( ev->value == VRPROBE_READY ) {
				m->state = VRSTATE_TRANSITION;
				m->pending = VRState_Request( m->pending, VRPENDING_NOW );
			}
			return VRACT_NONE;
		case VREV_UNSUPPORTED:
			m->state = VRSTATE_UNSUPPORTED;
			/* Inside a restart, CL_Vid_Restart's own flat rebuild follows. */
			if ( !m->running )
				m->pending = VRState_Request( m->pending, VRPENDING_NOW );
			return VRACT_NONE;
		case VREV_CONNECTION_ENDED:
			if ( m->state == VRSTATE_UNSUPPORTED && !ev->value ) {
				m->state = VRSTATE_TRANSITION;
				m->pending = VRState_Request( m->pending, VRPENDING_AT_SAFE_POINT );
			} else if ( m->state == VRSTATE_VR && m->pending == VRPENDING_AT_SAFE_POINT ) {
				/* The module restart belonged to the connection that ended. */
				m->pending = VRPENDING_NONE;
			}
			return VRACT_NONE;
		case VREV_REQUEST:
			m->pending = VRState_Request( m->pending, (vrPending_t)ev->value );
			return VRACT_NONE;
	}
	return VRACT_NONE;
}

vrAction_t VRState_Reduce( vrMachine_t *m, const vrEvent_t *ev ) {
	vrAction_t action = VRState_ReduceEvent( m, ev );

	/* Landing flat or in VR ends any recovery, so the next request is a fresh one. */
	if ( m->state == VRSTATE_FLAT || m->state == VRSTATE_VR )
		m->resuming = qfalse;
	return action;
}
