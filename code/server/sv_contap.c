/*
 * Console tap: streams raw console output (^-color codes intact) over
 * a loopback TCP socket for the collector. Same discipline as the TV
 * tap: non-blocking, polled per frame, slow consumers dropped. Port is
 * always kernel-assigned and published via the sv_conPort serverinfo
 * cvar; discovery is the only dial path.
 */
#include "server.h"

#ifdef _WIN32
#	include <winsock2.h>
#	include <ws2tcpip.h>
typedef int socklen_t;
#	define CT_INVALID_SOCKET (-1)
#	define ct_close closesocket
#	define ct_wouldblock(e) ((e) == WSAEWOULDBLOCK)
#	define CT_SEND_FLAGS 0
static int ct_lasterr( void ) { return WSAGetLastError(); }
static void ct_nonblock( int fd ) { u_long one = 1; ioctlsocket( fd, FIONBIO, &one ); }
#else
#	include <sys/socket.h>
#	include <netinet/in.h>
#	include <netinet/tcp.h>
#	include <arpa/inet.h>
#	include <sys/ioctl.h>
#	include <errno.h>
#	include <unistd.h>
#	define CT_INVALID_SOCKET (-1)
#	define ct_close close
#	define ct_wouldblock(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#	ifdef MSG_NOSIGNAL
#		define CT_SEND_FLAGS MSG_NOSIGNAL
#	else
#		define CT_SEND_FLAGS 0 // macOS/BSD: rely on SO_NOSIGPIPE set per-socket
#	endif
static int ct_lasterr( void ) { return errno; }
static void ct_nonblock( int fd ) { int one = 1; ioctl( fd, FIONBIO, &one ); }
#endif

static cvar_t *sv_conTap;
static cvar_t *sv_conPort;

conTapState_t contap;

static void SV_ConTap_DropConsumer( conTapConsumer_t *c ) {
	if ( c->fd != CT_INVALID_SOCKET ) {
		ct_close( c->fd );
	}
	c->fd = CT_INVALID_SOCKET;
	c->outHead = c->outTail = 0;
}

/*
==================
SV_ConTap_Init

Bind a kernel-assigned loopback port and publish it in serverinfo.
Called from every SV_SpawnServer, so idempotent: the listener survives
map rotations; only a full SV_Shutdown closes it, and the next spawn
rebinds on a new port that collectors re-discover via getstatus.
listenFd > 0 only while open (zero-init 0, shutdown resets to -1).
==================
*/
void SV_ConTap_Init( void ) {
	struct sockaddr_in addr;
	socklen_t alen;
	int fd, i, one = 1;

	if ( contap.listenFd > 0 ) {
		return; // already listening (re-entry on a normal map rotation)
	}

	contap.listenFd = CT_INVALID_SOCKET;
	for ( i = 0; i < MAX_CONTAP_CONSUMERS; i++ ) {
		contap.consumers[i].fd = CT_INVALID_SOCKET;
	}

	sv_conTap = Cvar_Get( "sv_conTap", "0", CVAR_ARCHIVE );
	sv_conPort = Cvar_Get( "sv_conPort", "0", CVAR_SERVERINFO | CVAR_ROM );

	if ( !sv_conTap->integer ) {
		Cvar_Set( "sv_conPort", "0" );
		return;
	}

	fd = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	if ( fd == CT_INVALID_SOCKET ) {
		Com_Printf( "ConTap: socket() failed: %d\n", ct_lasterr() );
		return;
	}
	setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof( one ) );
	ct_nonblock( fd );

	Com_Memset( &addr, 0, sizeof( addr ) );
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); // 127.0.0.1 only
	addr.sin_port = 0;                               // kernel-assigned, always

	if ( bind( fd, (struct sockaddr *)&addr, sizeof( addr ) ) != 0 ) {
		Com_Printf( "ConTap: bind(127.0.0.1:0) failed: %d\n", ct_lasterr() );
		ct_close( fd );
		return;
	}
	alen = sizeof( addr );
	if ( getsockname( fd, (struct sockaddr *)&addr, &alen ) != 0 ) {
		Com_Printf( "ConTap: getsockname() failed: %d\n", ct_lasterr() );
		ct_close( fd );
		return;
	}
	if ( listen( fd, MAX_CONTAP_CONSUMERS ) != 0 ) {
		Com_Printf( "ConTap: listen() failed: %d\n", ct_lasterr() );
		ct_close( fd );
		return;
	}

	contap.listenFd = fd;
	contap.listenPort = (int)ntohs( addr.sin_port );
	Cvar_Set( "sv_conPort", va( "%d", contap.listenPort ) );
	Com_Printf( "ConTap: listening on 127.0.0.1:%d (TCP)\n", contap.listenPort );
}

void SV_ConTap_Shutdown( void ) {
	int i;
	for ( i = 0; i < MAX_CONTAP_CONSUMERS; i++ ) {
		SV_ConTap_DropConsumer( &contap.consumers[i] );
	}
	if ( contap.listenFd != CT_INVALID_SOCKET && contap.listenFd > 0 ) {
		ct_close( contap.listenFd );
	}
	contap.listenFd = CT_INVALID_SOCKET;
	if ( sv_conPort ) {
		Cvar_Set( "sv_conPort", "0" );
	}
}

// Queue bytes for a consumer. Returns qfalse on overflow (caller drops
// the consumer: no mid-stream byte gaps, ever; the collector redials).
static qboolean SV_ConTap_Enqueue( conTapConsumer_t *c, const void *data, int len ) {
	if ( c->outHead == c->outTail ) {
		c->outHead = c->outTail = 0;
	}
	if ( c->outTail + len > CONTAP_OUTBUF_SIZE ) {
		int pending = c->outTail - c->outHead;
		if ( pending + len > CONTAP_OUTBUF_SIZE ) {
			return qfalse; // overflow: slow consumer, drop it
		}
		memmove( c->out, c->out + c->outHead, pending );
		c->outHead = 0;
		c->outTail = pending;
	}
	Com_Memcpy( c->out + c->outTail, data, len );
	c->outTail += len;
	return qtrue;
}

// Send as much queued data as the socket will take without blocking.
// Returns qfalse on a fatal socket error (caller drops the consumer).
static qboolean SV_ConTap_PumpConsumer( conTapConsumer_t *c ) {
	while ( c->outHead < c->outTail ) {
		int n = (int)send( c->fd, (const char *)( c->out + c->outHead ),
			c->outTail - c->outHead, CT_SEND_FLAGS );
		if ( n > 0 ) {
			c->outHead += n;
			continue;
		}
		if ( n < 0 && ct_wouldblock( ct_lasterr() ) ) {
			return qtrue; // socket full for now; try again next frame
		}
		return qfalse; // 0 (peer closed) or real error
	}
	if ( c->outHead == c->outTail ) {
		c->outHead = c->outTail = 0;
	}
	return qtrue;
}

/*
==================
SV_ConTap_Print

The Com_Printf hook. Runs inside Com_Printf: never print, never block;
just memcpy into consumer buffers (overflow drops the consumer).
Fragments forwarded as-is; the collector assembles lines.
==================
*/
void SV_ConTap_Print( const char *msg ) {
	int i, len;

	if ( contap.listenFd <= 0 ) {
		return; // not initialized / disabled / shut down
	}
	len = (int)strlen( msg );
	if ( len <= 0 ) {
		return;
	}
	for ( i = 0; i < MAX_CONTAP_CONSUMERS; i++ ) {
		conTapConsumer_t *c = &contap.consumers[i];
		if ( c->fd == CT_INVALID_SOCKET ) {
			continue;
		}
		if ( !SV_ConTap_Enqueue( c, msg, len ) ) {
			SV_ConTap_DropConsumer( c ); // silent: no printing from the hook
		}
	}
}

// Per-frame: accept new consumers (greeted with an identity line) and
// pump queued bytes.
void SV_ConTap_RunListener( void ) {
	int i;

	if ( contap.listenFd <= 0 ) {
		return;
	}

	for ( ;; ) {
		int fd = (int)accept( contap.listenFd, NULL, NULL );
		if ( fd == CT_INVALID_SOCKET ) {
			break; // EWOULDBLOCK (no more pending) or error: stop accepting
		}
		{
			char hello[MAX_QPATH + 64];
			int slot = -1, j;
			for ( j = 0; j < MAX_CONTAP_CONSUMERS; j++ ) {
				if ( contap.consumers[j].fd == CT_INVALID_SOCKET ) { slot = j; break; }
			}
			if ( slot < 0 ) {
				ct_close( fd ); // full
				continue;
			}
			ct_nonblock( fd );
#ifdef TCP_NODELAY
			{ int one = 1; setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof( one ) ); }
#endif
#ifdef SO_NOSIGPIPE
			{ int one = 1; setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof( one ) ); }
#endif
			contap.consumers[slot].fd = fd;
			contap.consumers[slot].outHead = contap.consumers[slot].outTail = 0;
			Com_sprintf( hello, sizeof( hello ), "CON1 %d %s\n",
				(int)Cvar_VariableValue( "net_port" ), Cvar_VariableString( "mapname" ) );
			if ( !SV_ConTap_Enqueue( &contap.consumers[slot], hello, (int)strlen( hello ) ) ) {
				SV_ConTap_DropConsumer( &contap.consumers[slot] );
			}
		}
	}

	// Pump queued output; drop dead/slow consumers (gameplay never waits).
	for ( i = 0; i < MAX_CONTAP_CONSUMERS; i++ ) {
		conTapConsumer_t *c = &contap.consumers[i];
		if ( c->fd == CT_INVALID_SOCKET ) continue;
		if ( !SV_ConTap_PumpConsumer( c ) ) {
			SV_ConTap_DropConsumer( c );
		}
	}
}
