#ifndef CL_VR_INPUT_H
#define CL_VR_INPUT_H
#include "../vrcommon/vr_clientinfo.h"
#include "../vrcommon/vr_cvars.h"
extern vr_clientinfo_t vr;
/* Include after client.h (refXRFrame_t/usercmd_t). */
void CL_VRInput_Init( void );
void CL_VRInput_Shutdown( void );
void CL_VRInput_Reset( void );
void CL_VRInput_BindCapture( void );
void CL_VRInput_CancelCapture( void );
void CL_VRInput_Frame( const refXRFrame_t *frame );
void CL_VRInput_SetVirtualScreen( qboolean enabled );
qboolean CL_VRInput_ApplyMove( usercmd_t *cmd );
/* Bound kbutton state (cl_input.c): buttons, digital moves and walking, as the flatscreen command gets them. */
void CL_VRInput_KeyState( usercmd_t *cmd );
void CL_VRInput_HapticEvent( const char *event, int position, int flags, int intensity, float angle, float height );
void CL_VRInput_HoverTick( int hand );
qboolean CL_VRInput_PointerOnScreen( int hand );
void VKeyboard_Show( void );
void VKeyboard_Hide( void );
qboolean VKeyboard_IsActive( void );
void VKeyboard_Draw( void );
qboolean VKeyboard_HandleKey( int key );
void VKeyboard_HandleOffhandKey( qboolean down );
void VKeyboard_RendererStarted( void );
#endif
