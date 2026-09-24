#ifndef CL_RENDERER_RECOVERY_H
#define CL_RENDERER_RECOVERY_H
#include "../qcommon/q_shared.h"
/* A caller spanning module initialization must force-unload abandoned VM calls before retrying. */
qboolean CL_RendererTry( void (*operation)(void), char *message, int size );
/* Outer engine error unwinds can bypass the renderer's private jump target. */
void CL_RendererRecoveryReset( void );
void NORETURN FORMAT_PRINTF(2, 3) QDECL CL_RendererError( errorParm_t code, const char *fmt, ... );
errorParm_t CL_RendererFailureCode( void );
#endif
