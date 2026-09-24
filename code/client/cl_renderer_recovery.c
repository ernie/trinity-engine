#include "cl_renderer_recovery.h"
#include "../qcommon/qcommon.h"
#include <setjmp.h>
static jmp_buf recovery;
static qboolean guarded;
static char failure[MAXPRINTMSG];
static errorParm_t failureCode;

void CL_RendererRecoveryReset( void ) {
	/* No renderer callbacks: its frame may already have been unwound. */
	guarded = qfalse;
	failure[0] = 0;
}

qboolean CL_RendererTry( void (*operation)(void), char *message, int size ) {
	if ( guarded )
		Com_Error( ERR_FATAL, "Nested renderer recovery scope" );
	if ( Q_setjmp( recovery ) ) {
		guarded = qfalse;
		Q_strncpyz( message, failure, size );
		return qfalse;
	}
	guarded = qtrue;
	operation();
	guarded = qfalse;
	if ( size > 0 )
		message[0] = 0;
	return qtrue;
}

void NORETURN QDECL CL_RendererError( errorParm_t code, const char *fmt, ... ) {
	va_list args;
	va_start( args, fmt );
	Q_vsnprintf( failure, sizeof( failure ), fmt, args );
	va_end( args );
	failureCode = code;
	if ( guarded && (code == ERR_DROP || code == ERR_FATAL) )
		Q_longjmp( recovery, 1 );
	Com_Error( code, "%s", failure );
}

errorParm_t CL_RendererFailureCode( void ) {
	return failureCode;
}
