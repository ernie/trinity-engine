/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include <signal.h>

#ifdef _DEBUG
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#ifndef DEDICATED
#include "../renderer/tr_local.h"
#endif

// Pending async signal recorded by signal_handler_async. sig_atomic_t
// + volatile is the only safe type combination to read/write across
// a signal handler per POSIX. Drained by Sys_CheckPendingSignal from
// non-signal context (the main loop in Sys_Main).
static volatile sig_atomic_t signal_pending = 0;

// Recursion guard for the in-context fault handler.
static qboolean fault_caught = qfalse;

extern void NORETURN Sys_Exit( int code );

// Async-safe handler for "external please stop" signals (TERM, HUP,
// QUIT). Sets a flag and returns; the engine's main loop drains the
// flag between Com_Frame iterations and runs SV_Shutdown from normal
// context. The previous design called VM_Call(GAME_SHUTDOWN) from the
// signal handler itself, which lands inside whatever VM call was
// already on the stack (G_RunFrame, BotAIStartFrame, ClientThink),
// reentering the Q3 VM which has single-set statics — leading to
// partial or missing ShutdownGame: log output depending on what
// frame state the SIGTERM happened to interrupt.
static void signal_handler_async( int sig )
{
	if ( signal_pending == 0 ) {
		signal_pending = sig;
	}
	// Second signal while one is pending: do nothing. We're already
	// shutting down at the next frame boundary. The previous design
	// had a DOUBLE-SIGNAL-FAULT path that Sys_Exit(1)'d here; that
	// existed because the in-handler SV_Shutdown could not safely
	// recurse. With shutdown deferred to main-loop context, there is
	// nothing to recurse into and the signal is harmless to drop.
}

// In-context handler for synchronous fault signals (SEGV, ILL, BUS,
// FPE, TRAP, IOT). Returning from these is undefined behavior on
// most platforms, so we have to do whatever cleanup we can right
// here, accepting the VM-reentrancy risk. Worst case: shutdown is
// partial and we fall through to Sys_Exit(1) on a re-fault.
static void signal_handler_fatal( int sig )
{
	char msg[32];

	if ( fault_caught ) {
		printf( "DOUBLE SIGNAL FAULT: Received signal %d, exiting...\n", sig );
		Sys_Exit( 1 ); // abstraction
	}
	fault_caught = qtrue;

	printf( "Received signal %d, exiting...\n", sig );

#ifdef _DEBUG
	if ( sig == SIGSEGV || sig == SIGILL || sig == SIGBUS )
	{
		void *syms[10];
		const size_t size = backtrace( syms, ARRAY_LEN( syms ) );
		backtrace_symbols_fd( syms, size, STDERR_FILENO );
	}
#endif

	sprintf( msg, "Signal caught (%d)", sig );
	VM_Forced_Unload_Start();
#ifndef DEDICATED
	CL_Shutdown( msg, qtrue );
#endif
	SV_Shutdown( msg );
	VM_Forced_Unload_Done();
	Sys_Exit( 0 ); // send a 0 to avoid DOUBLE SIGNAL FAULT
}


// Drains the pending-async-signal flag. Called by Sys_Main's main
// loop between Com_Frame iterations. NORETURN when a signal was
// pending — Sys_Exit terminates the process after a clean
// SV_Shutdown that completes from non-signal context (so G_RunFrame
// is fully unwound and VM_Call into GAME_SHUTDOWN is a top-level
// invocation, the way the VM expects).
void Sys_CheckPendingSignal( void )
{
	int  sig;
	char msg[32];

	sig = signal_pending;
	if ( sig == 0 ) {
		return;
	}

	printf( "Received signal %d, exiting...\n", sig );
	sprintf( msg, "Signal caught (%d)", sig );

	VM_Forced_Unload_Start();
#ifndef DEDICATED
	CL_Shutdown( msg, qtrue );
#endif
	SV_Shutdown( msg );
	VM_Forced_Unload_Done();
	Sys_Exit( 0 );
}


void InitSig( void )
{
	// Q3 has historically ignored SIGINT — kept that way.
	signal( SIGINT,  SIG_IGN );

	// External-request signals: deferred to main-loop context to
	// avoid reentering the VM mid-frame.
	signal( SIGHUP,  signal_handler_async );
	signal( SIGQUIT, signal_handler_async );
	signal( SIGTERM, signal_handler_async );

	// Synchronous fault signals: handle in-context, no deferral
	// possible — returning would be UB.
	signal( SIGILL,  signal_handler_fatal );
	signal( SIGTRAP, signal_handler_fatal );
	signal( SIGIOT,  signal_handler_fatal );
	signal( SIGBUS,  signal_handler_fatal );
	signal( SIGFPE,  signal_handler_fatal );
	signal( SIGSEGV, signal_handler_fatal );
}
