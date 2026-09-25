#ifndef CL_VR_STATE_H
#define CL_VR_STATE_H
#include "../qcommon/q_shared.h"
#include "../vrcommon/xr_loader.h"

typedef enum {
	VRSTATE_FLAT,
	VRSTATE_VR,
	VRSTATE_TRANSITION,
	VRSTATE_WAITING_HEADSET,
	VRSTATE_UNSUPPORTED
} vrState_t;

typedef enum {
	VRBEGIN_FLAT,
	VRBEGIN_UNSUPPORTED,
	VRBEGIN_DEFER,
	VRBEGIN_PROBE
} vrBegin_t;

typedef struct {
	int count;
	int windowStart;
} vrRecoveryCap_t;

const char *VRState_Name( vrState_t state );
/* Capped and unsupported restarts land flat while VR stays wanted. */
vrBegin_t VRState_Begin( qboolean wanted, qboolean capped, qboolean unsupported, qboolean safePoint );
/* qtrue when this recovery is the third inside 30 seconds. */
qboolean VRState_CountRecovery( vrRecoveryCap_t *cap, int now );

typedef enum {
	VRFAIL_RETRY_VR,
	VRFAIL_WAIT_HEADSET,
	VRFAIL_FLAT
} vrFailAction_t;

/* An error or game switch returns to VR from these states. */
qboolean VRState_Resumes( qboolean wanted, vrState_t state );
/* For a failed VR attempt that has already counted toward the cap. */
vrFailAction_t VRState_RestartFailed( qboolean xrFailure, qboolean retried, qboolean capFired );

typedef enum {
	VRPROBE_READY,
	VRPROBE_WAIT,
	VRPROBE_INCAPABLE
} vrProbe_t;

/* INCAPABLE holds for the whole session, so it lands flat and skips the headset watch. */
vrProbe_t VRState_Probe( xrLoaderStatus_t status, qboolean vulkanEnable2 );

typedef enum {
	VRMOD_OK,
	VRMOD_WAIT,
	VRMOD_FALLBACK,
	VRMOD_UNSUPPORTED
} vrModuleVerdict_t;

typedef struct {
	qboolean	registered;
	qboolean	provisional;		/* downloads can still replace the module */
	qboolean	pure;
	qboolean	baseGame;			/* baseq3 or missionpack, which have a bundled fallback */
	qboolean	fallbackRunning;
	qboolean	fallbackAvailable;
} vrModuleCheck_t;

/* Verdicts rise in severity; *reason names the proof for VRMOD_UNSUPPORTED. */
vrModuleVerdict_t VRState_ModuleVerdict( const vrModuleCheck_t *check, const char **reason );
/* A VR_UNSUPPORTED proof at the menu belongs to the mod, so a pending mod change tries VR again. */
qboolean VRState_ModChangeRetries( vrState_t state, qboolean disconnected, qboolean modChangePending );

/* A VR restart outside these points waits for the next one. */
qboolean VRState_SafePoint( qboolean active, qboolean disconnected, qboolean vrActive, qboolean gameSwitch );

/* Ranked: a request keeps the higher of itself and what is already pending. */
typedef enum {
	VRPENDING_NONE,
	VRPENDING_AT_SAFE_POINT,	/* CA_ACTIVE or CA_DISCONNECTED */
	VRPENDING_NOW,
	VRPENDING_AFTER_ERROR,		/* counts toward the recovery cap first */
	VRPENDING_NOW_FLAT,			/* the cap fired, so this restart lands flat */
	VRPENDING_AT_HUNK_USERS		/* after the new game's configuration; holds vid_restart and snd_restart */
} vrPending_t;

typedef struct {
	vrState_t		state;
	vrPending_t		pending;
	qboolean		running;		/* inside CL_Vid_Restart */
	qboolean		xrRenderer;		/* the renderer being built or running is the XR one */
	qboolean		xrTainted;		/* an error abandoned XR frame state; no XR calls until the next restart */
	vrRecoveryCap_t	cap;
} vrMachine_t;

typedef enum {
	VREV_ABORT,				/* Com_Error unwound the stack */
	VREV_ERROR,				/* after VREV_ABORT, for errors other than a disconnect */
	VREV_DISCONNECT_ERROR,	/* after VREV_ABORT, for ERR_DISCONNECT and ERR_SERVERDISCONNECT */
	VREV_SWITCH_BEGIN,
	VREV_HUNK_USERS,
	VREV_FRAME,
	VREV_BEGIN,
	VREV_PROBED,			/* value: vrProbe_t, after VRACT_PROBE */
	VREV_COMPLETE,			/* value: vrModuleVerdict_t of an XR restart, VRMOD_OK for a flat one */
	VREV_FAILED,			/* value: qtrue for an XR failure */
	VREV_LOST,
	VREV_HEADSET,			/* value: vrProbe_t from the headset watch */
	VREV_UNSUPPORTED,
	VREV_CONNECTION_ENDED,	/* value: qtrue when a cinematic ended */
	VREV_DIALOG,			/* value: qtrue for Retry, qfalse for Flatscreen on the startup dialog */
	VREV_REQUEST			/* value: vrPending_t */
} vrEventType_t;

typedef struct {
	vrEventType_t	type;
	int				value;
	int				now;
	qboolean		wanted;
	qboolean		vrActive;
	qboolean		settled;		/* CA_ACTIVE or CA_DISCONNECTED */
	qboolean		safePoint;		/* VRState_SafePoint */
	qboolean		disconnected;
	qboolean		modChange;		/* fs_game is modified */
	qboolean		retried;		/* VREV_FAILED: this restart already retried VR */
} vrEvent_t;

typedef enum {
	VRACT_NONE,
	VRACT_RESTART,		/* run CL_Vid_Restart */
	VRACT_CAP,			/* report the cap; after VREV_FRAME, restart too */
	VRACT_TEARDOWN,		/* leave VR mode */
	VRACT_PROBE,		/* probe the runtime, then send VREV_PROBED */
	VRACT_DEFER,
	VRACT_WAIT,
	VRACT_INCAPABLE,
	VRACT_ACTIVE,
	VRACT_RETRY,		/* begin the restart again in VR */
	VRACT_LEAVE			/* rebuild flat */
} vrAction_t;

vrPending_t VRState_Request( vrPending_t pending, vrPending_t request );
/* CL_Vid_Restart runs at the next CL_VR_Frame or CL_VR_FrameDone. */
qboolean VRState_RestartQueued( const vrMachine_t *m );
/* vid_restart and snd_restart wait for CL_StartHunkUsers. */
qboolean VRState_HoldsRestarts( const vrMachine_t *m );
/* The frame checks may request a restart, so they wait until this holds. */
qboolean VRState_Idle( const vrMachine_t *m );
/* An error-tainted frame skips drawing while a restart is due to rebuild the renderer. */
qboolean VRState_BlocksRendering( const vrMachine_t *m );
vrAction_t VRState_Reduce( vrMachine_t *m, const vrEvent_t *ev );
#endif
