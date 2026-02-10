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

#include "server.h"

tvState_t tv;
cvar_t *sv_tvauto;
cvar_t *sv_tvpath;


/*
===============
SV_TV_Init
===============
*/
void SV_TV_Init( void ) {
	sv_tvauto = Cvar_Get( "sv_tvauto", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( sv_tvauto, "Automatically start TV recording on map load." );

	sv_tvpath = Cvar_Get( "sv_tvpath", "demos", CVAR_ARCHIVE );
	Cvar_SetDescription( sv_tvpath, "Directory for TV recordings." );
}


/*
===============
SV_TV_FileWrite

Write data to TV file and track file offset.
===============
*/
static void SV_TV_FileWrite( const void *data, int len, fileHandle_t f ) {
	unsigned int newOffset;

	FS_Write( data, len, f );

	newOffset = tv.fileOffset + (unsigned int)len;
	if ( newOffset < tv.fileOffset ) {
		tv.fileOffsetHi++;
	}
	tv.fileOffset = newOffset;
}


/*
===============
SV_TV_StartRecord
===============
*/
static void SV_TV_StartRecord( const char *filename ) {
	char path[MAX_QPATH];
	int i;
	int val;
	char timestamp[64];
	time_t now;
	struct tm *tm_info;

	if ( sv.state != SS_GAME ) {
		Com_Printf( "TV: Not recording, server not running.\n" );
		return;
	}

	if ( tv.recording ) {
		Com_Printf( "TV: Already recording.\n" );
		return;
	}

	Com_Memset( &tv, 0, sizeof( tv ) );

	// Store base path (without extension) for later rename
	Com_sprintf( tv.recordingPath, sizeof( tv.recordingPath ), "%s/%s", sv_tvpath->string, filename );

	// Open as .tvd.tmp (renamed to .tvd on successful finalization)
	Com_sprintf( path, sizeof( path ), "%s.tvd.tmp", tv.recordingPath );
	tv.file = FS_FOpenFileWrite( path );
	if ( !tv.file ) {
		Com_Printf( "TV: Could not open %s for writing.\n", path );
		return;
	}

	tv.fileOffset = 0;
	tv.fileOffsetHi = 0;

	// Write header: magic
	SV_TV_FileWrite( "TVD1", 4, tv.file );

	// Protocol version
	val = 1;
	SV_TV_FileWrite( &val, 4, tv.file );

	// sv_fps
	val = sv_fps->integer;
	SV_TV_FileWrite( &val, 4, tv.file );

	// maxclients
	val = sv.maxclients;
	SV_TV_FileWrite( &val, 4, tv.file );

	// Duration in msec (placeholder, patched on close)
	val = 0;
	SV_TV_FileWrite( &val, 4, tv.file );

	// Map name (null-terminated)
	SV_TV_FileWrite( sv_mapname->string, (int)strlen( sv_mapname->string ) + 1, tv.file );

	// Timestamp (null-terminated ISO 8601)
	now = time( NULL );
	tm_info = localtime( &now );
	strftime( timestamp, sizeof( timestamp ), "%Y-%m-%dT%H:%M:%S", tm_info );
	SV_TV_FileWrite( timestamp, (int)strlen( timestamp ) + 1, tv.file );

	// Write configstrings
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		int len;
		unsigned short idx, slen;

		if ( !sv.configstrings[i] || sv.configstrings[i][0] == '\0' ) {
			continue;
		}

		len = (int)strlen( sv.configstrings[i] );
		idx = (unsigned short)i;
		slen = (unsigned short)len;
		SV_TV_FileWrite( &idx, 2, tv.file );
		SV_TV_FileWrite( &slen, 2, tv.file );
		SV_TV_FileWrite( sv.configstrings[i], len, tv.file );
	}

	// Configstring terminator
	{
		unsigned short term = 0xFFFF;
		SV_TV_FileWrite( &term, 2, tv.file );
	}

	// Zero baselines
	Com_Memset( tv.prevEntities, 0, sizeof( tv.prevEntities ) );
	Com_Memset( tv.prevEntityBitmask, 0, sizeof( tv.prevEntityBitmask ) );
	Com_Memset( tv.prevPlayers, 0, sizeof( tv.prevPlayers ) );
	Com_Memset( tv.prevPlayerBitmask, 0, sizeof( tv.prevPlayerBitmask ) );

	// Clear per-frame state
	Com_Memset( tv.csChanged, 0, sizeof( tv.csChanged ) );
	tv.cmdCount = 0;
	tv.cmdBufUsed = 0;

	tv.recording = qtrue;
	tv.frameCount = 0;

	Com_Printf( "TV: Recording to %s\n", path );
}


/*
===============
SV_TV_StartRecord_f
===============
*/
void SV_TV_StartRecord_f( void ) {
	const char *filename;
	char defaultName[MAX_QPATH];

	if ( Cmd_Argc() >= 2 ) {
		filename = Cmd_Argv( 1 );
	} else {
		time_t now = time( NULL );
		struct tm *tm_info = localtime( &now );
		strftime( defaultName, sizeof( defaultName ), "tv_%Y%m%d_%H%M%S", tm_info );
		filename = defaultName;
	}

	SV_TV_StartRecord( filename );
}


/*
===============
SV_TV_WriteFrame
===============
*/
void SV_TV_WriteFrame( void ) {
	msg_t msg;
	int i;
	byte curEntityBitmask[MAX_GENTITIES/8];
	byte curPlayerBitmask[MAX_CLIENTS/8];
	int csCount;
	unsigned int frameSize;

	// Check for deferred auto-start
	if ( tv.autoPending ) {
		const char *matchState = Cvar_VariableString( "g_matchState" );

		if ( matchState[0] != '\0' ) {
			// Match-state-aware mod (e.g. Trinity): start on "active"
			if ( !Q_stricmp( matchState, "active" ) ) {
				char name[MAX_QPATH];
				const char *uuid;

				tv.autoPending = qfalse;

				uuid = Cvar_VariableString( "g_matchUUID" );
				if ( uuid[0] != '\0' ) {
					Com_sprintf( name, sizeof( name ), "%s", uuid );
				} else {
					time_t now = time( NULL );
					struct tm *tm_info = localtime( &now );
					strftime( name, sizeof( name ), "tv_%Y%m%d_%H%M%S", tm_info );
				}

				SV_TV_StartRecord( name );
			}
		} else {
			// Fallback: start when first human client connects
			for ( i = 0; i < sv.maxclients; i++ ) {
				if ( svs.clients[i].state == CS_ACTIVE &&
					 svs.clients[i].netchan.remoteAddress.type != NA_BOT ) {
					char name[MAX_QPATH];
					const char *uuid;

					tv.autoPending = qfalse;

					uuid = Cvar_VariableString( "g_matchUUID" );
					if ( uuid[0] != '\0' ) {
						Com_sprintf( name, sizeof( name ), "%s", uuid );
					} else {
						time_t now = time( NULL );
						struct tm *tm_info = localtime( &now );
						strftime( name, sizeof( name ), "tv_%Y%m%d_%H%M%S", tm_info );
					}

					SV_TV_StartRecord( name );
					break;
				}
			}
		}
		return;
	}

	if ( !tv.recording ) {
		return;
	}

	// Track whether a human was present during recording
	if ( !tv.hadHuman ) {
		for ( i = 0; i < sv.maxclients; i++ ) {
			if ( svs.clients[i].state == CS_ACTIVE &&
				 svs.clients[i].netchan.remoteAddress.type != NA_BOT ) {
				tv.hadHuman = qtrue;
				break;
			}
		}
	}

	// Track server time range for duration
	if ( tv.frameCount == 0 ) {
		tv.firstServerTime = sv.time;
	}
	tv.lastServerTime = sv.time;

	// Init message buffer
	MSG_Init( &msg, tv.msgBuf, MAX_TV_MSGLEN );
	MSG_Bitstream( &msg );

	// Write server time
	MSG_WriteLong( &msg, sv.time );

	// --- Entity encoding ---
	Com_Memset( curEntityBitmask, 0, sizeof( curEntityBitmask ) );

	// Build current entity bitmask
	for ( i = 0; i < sv.num_entities; i++ ) {
		sharedEntity_t *ent = SV_GentityNum( i );
		if ( ent->r.linked && !( ent->r.svFlags & SVF_NOCLIENT ) ) {
			curEntityBitmask[i >> 3] |= ( 1 << ( i & 7 ) );
		}
	}

	// Write entity bitmask
	MSG_WriteData( &msg, curEntityBitmask, sizeof( curEntityBitmask ) );

	// Write delta-encoded entities
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		entityState_t *es;

		if ( !( curEntityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		es = &SV_GentityNum( i )->s;
		MSG_WriteDeltaEntity( &msg, &tv.prevEntities[i], es, qfalse );
	}

	// Entity end marker
	MSG_WriteBits( &msg, MAX_GENTITIES - 1, GENTITYNUM_BITS );

	// --- Player encoding ---
	Com_Memset( curPlayerBitmask, 0, sizeof( curPlayerBitmask ) );

	for ( i = 0; i < sv.maxclients; i++ ) {
		if ( svs.clients[i].state == CS_ACTIVE ) {
			curPlayerBitmask[i >> 3] |= ( 1 << ( i & 7 ) );
		}
	}

	// Write player bitmask
	MSG_WriteData( &msg, curPlayerBitmask, sizeof( curPlayerBitmask ) );

	// Write delta-encoded players
	for ( i = 0; i < sv.maxclients; i++ ) {
		playerState_t *ps;

		if ( !( curPlayerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		ps = SV_GameClientNum( i );
		MSG_WriteByte( &msg, i );
		MSG_WriteDeltaPlayerstate( &msg, &tv.prevPlayers[i], ps );
	}

	// --- Configstring changes ---
	csCount = 0;
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( tv.csChanged[i] ) {
			csCount++;
		}
	}

	MSG_WriteShort( &msg, csCount );

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		int len;

		if ( !tv.csChanged[i] ) {
			continue;
		}

		len = sv.configstrings[i] ? (int)strlen( sv.configstrings[i] ) : 0;
		MSG_WriteShort( &msg, i );
		MSG_WriteShort( &msg, len );
		if ( len > 0 ) {
			MSG_WriteData( &msg, sv.configstrings[i], len );
		}
	}

	Com_Memset( tv.csChanged, 0, sizeof( tv.csChanged ) );

	// --- Server commands ---
	MSG_WriteShort( &msg, tv.cmdCount );

	for ( i = 0; i < tv.cmdCount; i++ ) {
		MSG_WriteByte( &msg, tv.cmds[i].target == -1 ? 255 : (byte)tv.cmds[i].target );
		MSG_WriteShort( &msg, tv.cmds[i].len );
		MSG_WriteData( &msg, tv.cmdBuf + tv.cmds[i].offset, tv.cmds[i].len );
	}

	tv.cmdCount = 0;
	tv.cmdBufUsed = 0;

	// --- Flush to file ---
	if ( msg.overflowed ) {
		Com_Printf( "TV: Frame %i overflowed message buffer, stopping recording.\n", tv.frameCount );
		SV_TV_StopRecord( qtrue );
		return;
	}

	frameSize = (unsigned int)msg.cursize;
	SV_TV_FileWrite( &frameSize, 4, tv.file );
	SV_TV_FileWrite( msg.data, msg.cursize, tv.file );

	// Save current state as previous for next delta.
	// Zero removed entities/players so reappearing ones get a full delta.
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		if ( curEntityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) {
			tv.prevEntities[i] = SV_GentityNum( i )->s;
		} else {
			Com_Memset( &tv.prevEntities[i], 0, sizeof( entityState_t ) );
		}
	}
	Com_Memcpy( tv.prevEntityBitmask, curEntityBitmask, sizeof( curEntityBitmask ) );

	for ( i = 0; i < sv.maxclients; i++ ) {
		if ( curPlayerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) {
			tv.prevPlayers[i] = *SV_GameClientNum( i );
		} else {
			Com_Memset( &tv.prevPlayers[i], 0, sizeof( playerState_t ) );
		}
	}
	Com_Memcpy( tv.prevPlayerBitmask, curPlayerBitmask, sizeof( curPlayerBitmask ) );

	tv.frameCount++;
}


/*
===============
SV_TV_StopRecord
===============
*/
void SV_TV_StopRecord( qboolean discard ) {
	char tmpPath[MAX_QPATH];
	float duration;

	tv.autoPending = qfalse;

	if ( !tv.recording ) {
		return;
	}

	Com_sprintf( tmpPath, sizeof( tmpPath ), "%s.tvd.tmp", tv.recordingPath );

	if ( discard ) {
		// Close and delete the file without finalizing
		FS_FCloseFile( tv.file );
		FS_HomeRemove( tmpPath );
		Com_Printf( "TV: Recording discarded, file deleted.\n" );
	} else {
		char finalPath[MAX_QPATH];
		int durationMsec;

		// Patch duration in header at offset 16
		// (magic[4] + protocol[4] + sv_fps[4] + maxclients[4])
		durationMsec = tv.lastServerTime - tv.firstServerTime;
		FS_Seek( tv.file, 16, FS_SEEK_SET );
		FS_Write( &durationMsec, 4, tv.file );

		FS_FCloseFile( tv.file );

		// Rename .tvd.tmp to .tvd
		Com_sprintf( finalPath, sizeof( finalPath ), "%s.tvd", tv.recordingPath );
		FS_Rename( tmpPath, finalPath );

		duration = (float)durationMsec / 1000.0f;

		Com_Printf( "TV: Recording stopped. %i frames (%.1f seconds), %u bytes.\n",
			tv.frameCount, duration, tv.fileOffset );
	}

	tv.recording = qfalse;
	tv.file = 0;
}


/*
===============
SV_TV_StopRecord_f
===============
*/
void SV_TV_StopRecord_f( void ) {
	if ( !tv.recording ) {
		Com_Printf( "TV: Not recording.\n" );
		return;
	}

	SV_TV_StopRecord( qfalse );
}


/*
===============
SV_TV_ConfigstringChanged
===============
*/
void SV_TV_ConfigstringChanged( int index ) {
	if ( index >= 0 && index < MAX_CONFIGSTRINGS ) {
		tv.csChanged[index] = qtrue;
	}
}


/*
===============
SV_TV_CaptureServerCommand
===============
*/
void SV_TV_CaptureServerCommand( int target, const char *cmd ) {
	int len;

	if ( tv.cmdCount >= MAX_TV_CMDS ) {
		return;
	}

	len = (int)strlen( cmd );
	if ( tv.cmdBufUsed + len > MAX_TV_CMDBUF ) {
		return;
	}

	tv.cmds[tv.cmdCount].target = target;
	tv.cmds[tv.cmdCount].offset = tv.cmdBufUsed;
	tv.cmds[tv.cmdCount].len = len;
	Com_Memcpy( tv.cmdBuf + tv.cmdBufUsed, cmd, len );
	tv.cmdBufUsed += len;
	tv.cmdCount++;
}


/*
===============
SV_TV_AutoStart
===============
*/
void SV_TV_AutoStart( void ) {
	if ( !sv_tvauto->integer || tv.recording || tv.autoPending ) {
		return;
	}

	tv.autoPending = qtrue;
	tv.hadHuman = qfalse;
	Com_Printf( "TV: Auto-record pending, waiting for first client.\n" );
}
