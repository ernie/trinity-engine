#ifndef CL_VR_H
#define CL_VR_H
#include "../qcommon/q_shared.h"
qboolean CL_VR_WaitingForTracking( void );
qboolean CL_VR_DesktopWaitingFrame( void );
void CL_VR_Init( void );
/* The connected server's g_gametype, as sent in the gamestate serverinfo. */
int CL_VR_Gametype( void );
qboolean CL_VR_RebuildPending( void );
qboolean CL_VR_ConsumeRebuildRequest( void );
void CL_VR_Shutdown( void );
void CL_VR_Frame( void );
/* End of CL_Frame: a restart requested during this frame runs before the next frame's commands. */
void CL_VR_FrameDone( void );
void CL_VR_PrepareRenderer( void );
void CL_VR_CGameLoading( void );
void CL_VR_RestartBegin( void );
qboolean CL_VR_RestartComplete( void );
qboolean CL_VR_RestartFailed( const char *failure, qboolean retried );
qboolean CL_VR_RestartWantsVR( void );
qboolean CL_VR_ModePending( void );
/* Before the renderer is torn down for a game directory change. */
void CL_VR_BeginGameSwitch( void );
qboolean CL_VR_RenderingBlocked( void );
/* Com_Error only, before CL_Disconnect; keepRenderer (disconnect errors) submits an open XR frame. */
void CL_VR_AbortUnwind( qboolean keepRenderer );
/* Error unwind only, before renderer teardown. Never calls the XR backend. */
void CL_VR_ResetForError( void );
/* Disconnect unwind only; keeps the active renderer and mode. */
void CL_VR_RestartAborted( void );
/* After a VR_UNSUPPORTED proof, each connect, disconnect or game restart tries VR once at the next safe point. */
void CL_VR_ConnectionEnded( void );
qboolean CL_VR_BeginFrame( void );
/* Returns ownership: the caller must pair a successful begin with EndFrame. */
qboolean CL_VR_BeginLoadingFrame( qboolean tracked );
qboolean CL_VR_FrameOpen( void );
void CL_VR_EndFrame( void );
void CL_VR_ResetVirtualScreen( void );
qboolean CL_VR_RenderStereo( void );
int CL_VR_UsercmdButtonBits( void );

/* 0 Flatscreen (also dismissal), 1 Retry, 2 Quit. */
int Sys_VRFailureDialog( const char *reason );
#endif
