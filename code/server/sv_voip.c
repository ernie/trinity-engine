// sv_voip.c -- server VOIP implementation
// Ported from ioq3

#include "server.h"

#ifdef USE_VOIP

/*
==================
SV_ShouldIgnoreVoipSender

Blocking of voip packets based on source client
==================
*/
static qboolean SV_ShouldIgnoreVoipSender( const client_t *cl )
{
	if ( !sv_voip->integer )
		return qtrue;  // VoIP disabled on this server.
	else if ( !cl->hasVoip )  // client doesn't have VoIP support?!
		return qtrue;

	return qfalse;  // don't ignore.
}


/*
==================
SV_UserVoip

Parse clc_voipOpus (or legacy clc_voipSpeex) data from a client message
and route the encoded packet to all eligible recipients.

The server never decodes the audio -- it just forwards opaque Opus frames.
==================
*/
void SV_UserVoip( client_t *cl, msg_t *msg, qboolean ignoreData )
{
	int sender, generation, sequence, frames, packetsize;
	uint8_t recips[(MAX_CLIENTS + 7) / 8];
	int flags;
	int senderTeam = 0;
	byte encoded[sizeof(cl->voipPacket[0]->data)];
	client_t *client = NULL;
	voipServerPacket_t *packet = NULL;
	int i;

	sender = cl - svs.clients;
	generation = MSG_ReadByte( msg );
	sequence = MSG_ReadLong( msg );
	frames = MSG_ReadByte( msg );
	MSG_ReadData( msg, recips, sizeof( recips ) );
	flags = MSG_ReadByte( msg );
	packetsize = MSG_ReadShort( msg );

	if ( msg->readcount > msg->cursize )
		return;   // short/invalid packet, bail.

	if ( packetsize > (int)sizeof( encoded ) ) {  // overlarge packet?
		int bytesleft = packetsize;
		while ( bytesleft ) {
			int br = bytesleft;
			if ( br > (int)sizeof( encoded ) )
				br = sizeof( encoded );
			MSG_ReadData( msg, encoded, br );
			bytesleft -= br;
		}
		return;   // overlarge packet, bail.
	}

	MSG_ReadData( msg, encoded, packetsize );

	if ( ignoreData || SV_ShouldIgnoreVoipSender( cl ) )
		return;   // Blacklisted, disabled, etc.

	if ( flags & VOIP_TEAM ) {
		playerState_t *senderPs = SV_GameClientNum( sender );
		senderTeam = senderPs->persistant[PERS_TEAM];
	}

	// Capture for TVD recording (before per-client routing)
	if ( tv.recording && tv.voipCount < MAX_TV_VOIP_PACKETS
		&& tv.voipBufUsed + packetsize <= (int)sizeof( tv.voipBuf ) ) {
		tvVoipPacket_t *tvp = &tv.voipPackets[tv.voipCount];
		tvp->sender = sender;
		tvp->generation = generation;
		tvp->sequence = sequence;
		tvp->frames = frames;
		// For targeted sends with no channel flag (just recipient bits),
		// set VOIP_DIRECT so the TVD player knows to play it as direct audio.
		tvp->flags = flags;
		if ( !( flags & ( VOIP_SPATIAL | VOIP_TEAM | VOIP_ALL ) ) ) {
			for ( i = 0; i < (int)sizeof( recips ); i++ ) {
				if ( recips[i] ) {
					tvp->flags |= VOIP_DIRECT;
					break;
				}
			}
		}
		Com_Memcpy( tvp->recips, recips, sizeof( tvp->recips ) );
		tvp->offset = tv.voipBufUsed;
		tvp->len = packetsize;
		Com_Memcpy( tv.voipBuf + tv.voipBufUsed, encoded, packetsize );
		tv.voipBufUsed += packetsize;
		tv.voipCount++;
	}

	// decide who needs this VoIP packet sent to them...
	for ( i = 0, client = svs.clients; i < sv_maxclients->integer; i++, client++ ) {
		if ( client->state != CS_ACTIVE )
			continue;  // not in the game yet, don't send to this guy.
		else if ( i == sender )
			continue;  // don't send voice packet back to original author.
		else if ( !client->hasVoip )
			continue;  // no VoIP support, or unsupported protocol
		else if ( client->muteAllVoip )
			continue;  // client is ignoring everyone.
		else if ( client->ignoreVoipFromClient[sender] )
			continue;  // client is ignoring this talker.
		else if ( *cl->downloadName )
			continue;  // no VoIP allowed if downloading, to save bandwidth.

		{
			int pflags = flags;
			if ( pflags & VOIP_ALL ) {
				pflags |= VOIP_DIRECT;
			} else if ( pflags & VOIP_TEAM ) {
				playerState_t *recipPs = SV_GameClientNum( i );
				if ( recipPs->persistant[PERS_TEAM] == senderTeam ) {
					pflags |= VOIP_DIRECT;
				} else {
					pflags &= ~VOIP_DIRECT;
				}
			} else if ( Com_IsVoipTarget( recips, sizeof( recips ), i ) ) {
				pflags |= VOIP_DIRECT;
			} else {
				pflags &= ~VOIP_DIRECT;
			}

			if ( !( pflags & ( VOIP_SPATIAL | VOIP_DIRECT ) ) )
				continue;  // not addressed to this player.

			// Transmit this packet to the client.
			if ( client->queuedVoipPackets >= ARRAY_LEN( client->voipPacket ) ) {
				Com_Printf( "Too many VoIP packets queued for client #%d\n", i );
				continue;  // no room for another packet right now.
			}

			packet = Z_Malloc( sizeof( *packet ) );
			packet->sender = sender;
			packet->frames = frames;
			packet->len = packetsize;
			packet->generation = generation;
			packet->sequence = sequence;
			packet->flags = pflags;
			memcpy( packet->data, encoded, packetsize );

			client->voipPacket[( client->queuedVoipIndex + client->queuedVoipPackets ) % ARRAY_LEN( client->voipPacket )] = packet;
			client->queuedVoipPackets++;
		}
	}
}


/*
==================
SV_WriteVoipToClient

Check to see if there is any VoIP queued for a client, and send if there is.
Limits output to roughly 50% of remaining message space to avoid overflow.
==================
*/
void SV_WriteVoipToClient( client_t *cl, msg_t *msg )
{
	int totalbytes = 0;
	int i;
	voipServerPacket_t *packet;

	if ( cl->queuedVoipPackets ) {
		// Write as many VoIP packets as we reasonably can...
		for ( i = 0; i < cl->queuedVoipPackets; i++ ) {
			packet = cl->voipPacket[( i + cl->queuedVoipIndex ) % ARRAY_LEN( cl->voipPacket )];

			if ( !*cl->downloadName ) {
				totalbytes += packet->len;
				if ( totalbytes > ( msg->maxsize - msg->cursize ) / 2 )
					break;

				MSG_WriteByte( msg, svc_voipOpus );
				MSG_WriteShort( msg, packet->sender );
				MSG_WriteByte( msg, (byte)packet->generation );
				MSG_WriteLong( msg, packet->sequence );
				MSG_WriteByte( msg, packet->frames );
				MSG_WriteShort( msg, packet->len );
				if ( cl->voipVersion >= 2 ) {
					// v2 clients handle channel flags (VOIP_ALL, VOIP_TEAM)
					// natively, so don't send the server-added VOIP_DIRECT.
					int wflags = packet->flags & ~VOIP_DIRECT;
					// For targeted packets (no channel flag), set VOIP_DIRECT
					// so the client knows to play it.
					if ( !( wflags & ( VOIP_SPATIAL | VOIP_TEAM | VOIP_ALL ) ) )
						wflags |= VOIP_DIRECT;
					MSG_WriteBits( msg, wflags, VOIP_FLAGCNT );
				} else {
					MSG_WriteBits( msg, packet->flags & 0x03, VOIP_FLAGCNT_V1 );
				}
				MSG_WriteData( msg, packet->data, packet->len );
			}

			Z_Free( packet );
		}

		cl->queuedVoipPackets -= i;
		cl->queuedVoipIndex += i;
		cl->queuedVoipIndex %= ARRAY_LEN( cl->voipPacket );
	}
}


/*
==================
SV_UpdateVoipIgnore

Helper to set/clear per-client ignore flag.
==================
*/
static void SV_UpdateVoipIgnore( client_t *cl, const char *idstr, qboolean ignore )
{
	if ( ( *idstr >= '0' ) && ( *idstr <= '9' ) ) {
		const int id = atoi( idstr );
		if ( ( id >= 0 ) && ( id < MAX_CLIENTS ) ) {
			cl->ignoreVoipFromClient[id] = ignore;
		}
	}
}


/*
==================
SV_Voip_f

Handle the "voip" client command.
Subcommands: ignore <id>, unignore <id>, muteall, unmuteall
==================
*/
void SV_Voip_f( client_t *cl )
{
	const char *cmd = Cmd_Argv( 1 );

	if ( strcmp( cmd, "ignore" ) == 0 ) {
		SV_UpdateVoipIgnore( cl, Cmd_Argv( 2 ), qtrue );
	} else if ( strcmp( cmd, "unignore" ) == 0 ) {
		SV_UpdateVoipIgnore( cl, Cmd_Argv( 2 ), qfalse );
	} else if ( strcmp( cmd, "muteall" ) == 0 ) {
		cl->muteAllVoip = qtrue;
	} else if ( strcmp( cmd, "unmuteall" ) == 0 ) {
		cl->muteAllVoip = qfalse;
	}
}


/*
==================
SV_FreeVoipPackets

Free all queued VoIP packets for a client (called on disconnect).
==================
*/
void SV_FreeVoipPackets( client_t *cl )
{
	int index;

	for ( index = cl->queuedVoipIndex; index < cl->queuedVoipIndex + cl->queuedVoipPackets; index++ ) {
		Z_Free( cl->voipPacket[index % ARRAY_LEN( cl->voipPacket )] );
	}

	cl->queuedVoipPackets = 0;
}

#endif // USE_VOIP
