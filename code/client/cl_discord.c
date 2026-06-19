#include "client.h"
#include "cl_discord.h"
#include "cl_discord_proto.h"
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define DISCORD_GETPID() ( (int)_getpid() )
#else
#include <unistd.h>
#define DISCORD_GETPID() ( (int)getpid() )
#endif

#define DISCORD_CLIENT_ID		"1517569490139746404"
#define DISCORD_TICK_MS			1000	// evaluate presence at most once per second
#define DISCORD_RETRY_MS		15000	// wait between connect attempts when Discord absent
#define DISCORD_HANDSHAKE_MS	5000	// deadline for READY after handshake sent
#define DISCORD_CS_MESSAGE		3		// game configstring holding the worldspawn message (map display title)

typedef enum {
	DC_DISCONNECTED,
	DC_HANDSHAKING,
	DC_CONNECTED
} discordState_t;

static cvar_t			*cl_discordRichPresence;
static discordConn_t	*dc_conn;
static discordState_t	dc_state;
static int				dc_nextAttempt;			// Sys_Milliseconds gate for reconnect
static int				dc_nextTick;			// Sys_Milliseconds gate for tick body
static int				dc_handshakeDeadline;	// Sys_Milliseconds deadline for READY
static int				dc_nonce;
static discordActivity_t dc_last;				// last activity successfully sent
static qboolean			dc_haveLast;

void CL_Discord_Init( void ) {
	cl_discordRichPresence = Cvar_Get( "cl_discordRichPresence", "1", CVAR_ARCHIVE );
	dc_state = DC_DISCONNECTED;
	dc_conn = NULL;
	dc_nextAttempt = 0;
	dc_nextTick = 0;
	dc_handshakeDeadline = 0;
	dc_nonce = 0;
	dc_haveLast = qfalse;
}

static void CL_Discord_Reset( void ) {
	if ( dc_conn ) {
		Sys_DiscordClose( dc_conn );
		dc_conn = NULL;
	}
	dc_state = DC_DISCONNECTED;
	dc_haveLast = qfalse;
	dc_nextAttempt = Sys_Milliseconds() + DISCORD_RETRY_MS;
}

static qboolean CL_Discord_Send( const char *frame, int len ) {
	if ( Sys_DiscordWrite( dc_conn, frame, len ) != len ) {
		CL_Discord_Reset();
		return qfalse;
	}
	return qtrue;
}

/* Map current engine state to a phase + gametype, build the desired activity. */
static void CL_Discord_CurrentActivity( discordActivity_t *act ) {
	discordPhase_t phase;
	const char *serverInfo = "";
	const char *mapMessage = "";
	int gametype = 0;

	switch ( cls.state ) {
		case CA_DISCONNECTED:
		case CA_UNINITIALIZED:
			phase = DISCORD_MENU; break;
		case CA_CONNECTING:
		case CA_CHALLENGING:
		case CA_CONNECTED:
			phase = DISCORD_CONNECTING; break;
		case CA_LOADING:
		case CA_PRIMED:
			phase = DISCORD_LOADING; break;
		case CA_CINEMATIC:
			phase = DISCORD_CINEMATIC; break;
		case CA_ACTIVE:
			if ( tvPlay.active ) {
				phase = tvPlay.live ? DISCORD_WATCHING_TV : DISCORD_WATCHING_TVD;
			} else if ( clc.demoplaying ) {
				phase = DISCORD_WATCHING_DEMO;
			} else {
				phase = DISCORD_PLAYING;
			}
			break;
		default:
			phase = DISCORD_MENU;
			break;
	}

	if ( phase == DISCORD_PLAYING || phase == DISCORD_WATCHING_DEMO
		|| phase == DISCORD_WATCHING_TVD || phase == DISCORD_WATCHING_TV ) {
		serverInfo = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
		mapMessage = cl.gameState.stringData + cl.gameState.stringOffsets[ DISCORD_CS_MESSAGE ];
		gametype = atoi( Info_ValueForKey( serverInfo, "g_gametype" ) );
	}

	Discord_MapActivity( act, phase, serverInfo, mapMessage, gametype,
		(int)time( NULL ), dc_haveLast ? &dc_last : NULL );
}

static void CL_Discord_Update( void ) {
	char frame[ 1024 ];
	discordActivity_t want;
	int len;

	CL_Discord_CurrentActivity( &want );

	if ( dc_haveLast && Discord_ActivityEqual( &want, &dc_last ) &&
	     want.startTimestamp == dc_last.startTimestamp ) {
		return;	// nothing changed
	}

	len = Discord_BuildSetActivity( frame, sizeof( frame ), &want, DISCORD_GETPID(), ++dc_nonce );
	if ( len > 0 && CL_Discord_Send( frame, len ) ) {
		dc_last = want;
		dc_haveLast = qtrue;
		Com_DPrintf( "Discord: %s | %s\n", want.details, want.state );
	}
}

/* Drain inbound bytes; detect READY during handshake. Returns qfalse on closed. */
static qboolean CL_Discord_Pump( qboolean *gotReady ) {
	char buf[ 2048 ];
	int n = Sys_DiscordRead( dc_conn, buf, sizeof( buf ) );
	if ( n < 0 ) {
		CL_Discord_Reset();
		return qfalse;
	}
	if ( n > 0 && gotReady && Discord_BufContains( buf, n, "\"evt\":\"READY\"" ) ) {
		*gotReady = qtrue;
	}
	return qtrue;
}

void CL_Discord_Frame( void ) {
	int now;

	if ( !cl_discordRichPresence || !cl_discordRichPresence->integer ) {
		if ( dc_conn ) {
			CL_Discord_Shutdown();	// clears activity and closes
		}
		return;
	}

	now = Sys_Milliseconds();
	if ( now < dc_nextTick ) {
		return;
	}
	dc_nextTick = now + DISCORD_TICK_MS;

	switch ( dc_state ) {
		case DC_DISCONNECTED: {
			char frame[ 512 ];
			int len;
			if ( now < dc_nextAttempt ) {
				return;
			}
			dc_conn = Sys_DiscordConnect();
			if ( !dc_conn ) {
				dc_nextAttempt = now + DISCORD_RETRY_MS;
				return;
			}
			len = Discord_BuildHandshake( frame, sizeof( frame ), DISCORD_CLIENT_ID );
			if ( len <= 0 || !CL_Discord_Send( frame, len ) ) {
				return;
			}
			dc_state = DC_HANDSHAKING;
			dc_handshakeDeadline = now + DISCORD_HANDSHAKE_MS;
			break;
		}
		case DC_HANDSHAKING: {
			qboolean ready = qfalse;
			if ( !CL_Discord_Pump( &ready ) ) {
				return;
			}
			if ( ready ) {
				dc_state = DC_CONNECTED;
				CL_Discord_Update();
			} else if ( now >= dc_handshakeDeadline ) {
				CL_Discord_Reset();
				return;
			}
			break;
		}
		case DC_CONNECTED:
			if ( !CL_Discord_Pump( NULL ) ) {
				return;
			}
			CL_Discord_Update();
			break;
	}
}

void CL_Discord_Shutdown( void ) {
	if ( dc_conn ) {
		if ( dc_state == DC_CONNECTED ) {
			char frame[ 512 ];
			int len = Discord_BuildClearActivity( frame, sizeof( frame ),
				DISCORD_GETPID(), ++dc_nonce );
			if ( len > 0 ) {
				Sys_DiscordWrite( dc_conn, frame, len );
			}
		}
		Sys_DiscordClose( dc_conn );
		dc_conn = NULL;
	}
	dc_state = DC_DISCONNECTED;
	dc_haveLast = qfalse;
}
