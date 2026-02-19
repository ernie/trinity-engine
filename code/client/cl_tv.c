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
// cl_tv.c -- TV demo playback

#include "client.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

tvPlayback_t tvPlay;

cvar_t *cl_tvViewpoint;
cvar_t *cl_tvTime;
cvar_t *cl_tvDuration;

static void CL_TV_View_f( void );
static void CL_TV_ViewNext_f( void );
static void CL_TV_ViewPrev_f( void );
static void CL_TV_Seek_f( void );

// Read team from configstring rather than persistant[], which is unreliable
// for spectators in follow mode.
static int CL_TV_GetPlayerTeam( int clientNum ) {
	const char *cs = cl.gameState.stringData + cl.gameState.stringOffsets[CS_PLAYERS + clientNum];
	return atoi( Info_ValueForKey( cs, "t" ) );
}


/*
===============
CL_TV_Init
===============
*/
void CL_TV_Init( void ) {
	cl_tvViewpoint = Cvar_Get( "cl_tvViewpoint", "0", CVAR_ROM );
	cl_tvTime = Cvar_Get( "cl_tvTime", "0", CVAR_ROM );
	cl_tvDuration = Cvar_Get( "cl_tvDuration", "0", CVAR_ROM );
}


/*
===============
CL_TV_DecompressRead

Read decompressed data from the zstd stream.
Returns number of bytes actually read (< len at stream end).
===============
*/
static int CL_TV_DecompressRead( void *buf, int len ) {
	byte *dst = (byte *)buf;
	int total = 0;

	while ( total < len ) {
		// Consume from decompressed output buffer first
		if ( tvPlay.zstdOutPos < tvPlay.zstdOutSize ) {
			size_t avail = tvPlay.zstdOutSize - tvPlay.zstdOutPos;
			size_t want = (size_t)( len - total );
			size_t copy = ( want < avail ) ? want : avail;
			Com_Memcpy( dst + total, tvPlay.zstdOutBuf + tvPlay.zstdOutPos, copy );
			tvPlay.zstdOutPos += copy;
			total += (int)copy;
			continue;
		}

		if ( tvPlay.zstdStreamEnded ) {
			break;
		}

		// Read more compressed data from file if input buffer exhausted
		if ( tvPlay.zstdInPos >= tvPlay.zstdInSize ) {
			int bytesRead = FS_Read( tvPlay.zstdInBuf, TVD_ZSTD_IN_BUF_SIZE, tvPlay.file );
			if ( bytesRead <= 0 ) {
				tvPlay.zstdStreamEnded = qtrue;
				break;
			}
			tvPlay.zstdInSize = (size_t)bytesRead;
			tvPlay.zstdInPos = 0;
		}

		// Decompress
		{
			ZSTD_inBuffer in = { tvPlay.zstdInBuf, tvPlay.zstdInSize, tvPlay.zstdInPos };
			ZSTD_outBuffer out = { tvPlay.zstdOutBuf, TVD_ZSTD_OUT_BUF_SIZE, 0 };
			size_t ret = ZSTD_decompressStream( tvPlay.dstream, &out, &in );
			tvPlay.zstdInPos = in.pos;
			tvPlay.zstdOutSize = out.pos;
			tvPlay.zstdOutPos = 0;

			if ( ret == 0 ) {
				// Zstd frame complete
				tvPlay.zstdStreamEnded = qtrue;
			}
			if ( ZSTD_isError( ret ) ) {
				tvPlay.zstdStreamEnded = qtrue;
			}
		}
	}

	return total;
}


/*
===============
CL_TV_ReadTrailer

Read k/v trailer from the end of the file.
Format: "TVDt" + repeated(key\0 + valueLen:2 + valueData) + \0 + trailerSize:4
Returns qtrue on success, qfalse on failure (sets totalDuration=0).
===============
*/
static qboolean CL_TV_ReadTrailer( void ) {
	long savedPos;
	int trailerSize;
	char magic[4];
	long fileLen;
	char key[64];
	unsigned short vlen;
	byte vbuf[256];

	tvPlay.totalDuration = 0;

	savedPos = FS_FTell( tvPlay.file );

	// Get file length by seeking to end
	FS_Seek( tvPlay.file, 0, FS_SEEK_END );
	fileLen = FS_FTell( tvPlay.file );

	// Minimum trailer: "TVDt"(4) + \0(1) + size(4) = 9
	if ( fileLen < 9 ) {
		FS_Seek( tvPlay.file, savedPos, FS_SEEK_SET );
		return qfalse;
	}

	// Read trailerSize from EOF-4
	FS_Seek( tvPlay.file, fileLen - 4, FS_SEEK_SET );
	if ( FS_Read( &trailerSize, 4, tvPlay.file ) != 4 || trailerSize < 9 || trailerSize > fileLen ) {
		FS_Seek( tvPlay.file, savedPos, FS_SEEK_SET );
		return qfalse;
	}

	// Seek to trailer start
	FS_Seek( tvPlay.file, fileLen - trailerSize, FS_SEEK_SET );

	// Read and validate magic "TVDt"
	if ( FS_Read( magic, 4, tvPlay.file ) != 4 ||
		 magic[0] != 'T' || magic[1] != 'V' || magic[2] != 'D' || magic[3] != 't' ) {
		FS_Seek( tvPlay.file, savedPos, FS_SEEK_SET );
		return qfalse;
	}

	// Read k/v pairs until empty key (terminator)
	while ( 1 ) {
		int i;

		// Read key (null-terminated)
		for ( i = 0; i < (int)sizeof( key ) - 1; i++ ) {
			if ( FS_Read( &key[i], 1, tvPlay.file ) != 1 ) {
				FS_Seek( tvPlay.file, savedPos, FS_SEEK_SET );
				return qfalse;
			}
			if ( key[i] == '\0' ) {
				break;
			}
		}
		key[sizeof( key ) - 1] = '\0';

		// Empty key = terminator
		if ( key[0] == '\0' ) {
			break;
		}

		// Read value length
		if ( FS_Read( &vlen, 2, tvPlay.file ) != 2 ) {
			FS_Seek( tvPlay.file, savedPos, FS_SEEK_SET );
			return qfalse;
		}

		// Read value data
		if ( vlen > sizeof( vbuf ) ) {
			// Skip unknown large value
			FS_Seek( tvPlay.file, vlen, FS_SEEK_CUR );
			continue;
		}
		if ( vlen > 0 && FS_Read( vbuf, vlen, tvPlay.file ) != vlen ) {
			FS_Seek( tvPlay.file, savedPos, FS_SEEK_SET );
			return qfalse;
		}

		// Match known keys
		if ( !Q_stricmp( key, "dur" ) && vlen == 4 ) {
			Com_Memcpy( &tvPlay.totalDuration, vbuf, 4 );
		}
	}

	// Restore file position
	FS_Seek( tvPlay.file, savedPos, FS_SEEK_SET );
	return qtrue;
}


/*
===============
CL_TV_ReadString

Read a null-terminated string from file, byte at a time.
===============
*/
static int CL_TV_ReadString( char *buf, int bufSize ) {
	int i;
	for ( i = 0; i < bufSize - 1; i++ ) {
		if ( FS_Read( &buf[i], 1, tvPlay.file ) != 1 ) {
			buf[i] = '\0';
			return -1;
		}
		if ( buf[i] == '\0' ) {
			return i;
		}
	}
	buf[bufSize - 1] = '\0';
	return i;
}


/*
===============
CL_TV_FindFirstActivePlayer

Return the first active player clientNum from the player bitmask.
Returns -1 if none found.
===============
*/
static int CL_TV_FindFirstActivePlayer( void ) {
	int i;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( ( tvPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) )
				&& CL_TV_GetPlayerTeam( i ) != TEAM_SPECTATOR ) {
			return i;
		}
	}
	return -1;
}


/*
===============
CL_TV_UpdateConfigstring

Apply a configstring change to cl.gameState, rebuilding the string table.
===============
*/
static void CL_TV_UpdateConfigstring( int index, const char *data, int dataLen ) {
	gameState_t oldGs;
	int i, len;
	const char *dup;
	char modifiedInfo[BIG_INFO_STRING];

	// Ensure \tv\1 is always present in CS_SERVERINFO
	if ( index == CS_SERVERINFO && dataLen > 0 ) {
		Q_strncpyz( modifiedInfo, data, sizeof( modifiedInfo ) );
		Info_SetValueForKey( modifiedInfo, "tv", "1" );
		data = modifiedInfo;
		dataLen = (int)strlen( modifiedInfo );
	}

	oldGs = cl.gameState;
	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );
	cl.gameState.dataCount = 1;

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( i == index ) {
			// use new data (may be empty string if dataLen == 0)
			if ( dataLen <= 0 ) {
				continue;
			}
			if ( dataLen + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "CL_TV_UpdateConfigstring: MAX_GAMESTATE_CHARS exceeded" );
			}
			cl.gameState.stringOffsets[i] = cl.gameState.dataCount;
			Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, data, dataLen );
			cl.gameState.stringData[cl.gameState.dataCount + dataLen] = '\0';
			cl.gameState.dataCount += dataLen + 1;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[i];
			if ( !dup[0] ) {
				continue;
			}
			len = (int)strlen( dup );
			if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "CL_TV_UpdateConfigstring: MAX_GAMESTATE_CHARS exceeded" );
			}
			cl.gameState.stringOffsets[i] = cl.gameState.dataCount;
			Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1 );
			cl.gameState.dataCount += len + 1;
		}
	}
}


/*
===============
CL_TV_Open
===============
*/
qboolean CL_TV_Open( const char *filename ) {
	char magic[4];
	int protocol;
	char mapname[MAX_QPATH];
	char timestamp[64];
	char csData[BIG_INFO_STRING];
	unsigned short csIdx, csLen;

	Com_Memset( &tvPlay, 0, sizeof( tvPlay ) );

	if ( FS_FOpenFileRead( filename, &tvPlay.file, qtrue ) == -1 ) {
		return qfalse;
	}

	// Read and validate magic
	if ( FS_Read( magic, 4, tvPlay.file ) != 4 ||
		 magic[0] != 'T' || magic[1] != 'V' || magic[2] != 'D' || magic[3] != '1' ) {
		Com_Printf( S_COLOR_YELLOW "TV: Invalid magic\n" );
		FS_FCloseFile( tvPlay.file );
		return qfalse;
	}

	// Protocol version
	if ( FS_Read( &protocol, 4, tvPlay.file ) != 4 || protocol != 1 ) {
		Com_Printf( S_COLOR_YELLOW "TV: Unsupported protocol %i\n", protocol );
		FS_FCloseFile( tvPlay.file );
		return qfalse;
	}

	// sv_fps
	if ( FS_Read( &tvPlay.svFps, 4, tvPlay.file ) != 4 ) {
		FS_FCloseFile( tvPlay.file );
		return qfalse;
	}

	// maxclients
	if ( FS_Read( &tvPlay.maxclients, 4, tvPlay.file ) != 4 ) {
		FS_FCloseFile( tvPlay.file );
		return qfalse;
	}

	// Map name
	if ( CL_TV_ReadString( mapname, sizeof( mapname ) ) < 0 ) {
		FS_FCloseFile( tvPlay.file );
		return qfalse;
	}

	// Timestamp
	if ( CL_TV_ReadString( timestamp, sizeof( timestamp ) ) < 0 ) {
		FS_FCloseFile( tvPlay.file );
		return qfalse;
	}

	// Populate cl.gameState with configstrings
	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );
	cl.gameState.dataCount = 1;

	while ( 1 ) {
		if ( FS_Read( &csIdx, 2, tvPlay.file ) != 2 ) {
			FS_FCloseFile( tvPlay.file );
			return qfalse;
		}

		if ( csIdx == 0xFFFF ) {
			break;  // terminator
		}

		if ( FS_Read( &csLen, 2, tvPlay.file ) != 2 ) {
			FS_FCloseFile( tvPlay.file );
			return qfalse;
		}

		if ( csLen >= sizeof( csData ) ) {
			Com_Printf( S_COLOR_YELLOW "TV: Configstring %i too long (%i)\n", csIdx, csLen );
			FS_FCloseFile( tvPlay.file );
			return qfalse;
		}

		if ( csLen > 0 ) {
			if ( FS_Read( csData, csLen, tvPlay.file ) != csLen ) {
				FS_FCloseFile( tvPlay.file );
				return qfalse;
			}
		}
		csData[csLen] = '\0';

		if ( (unsigned)csIdx >= MAX_CONFIGSTRINGS ) {
			continue;
		}

		// Store in gameState
		if ( csLen + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Printf( S_COLOR_YELLOW "TV: MAX_GAMESTATE_CHARS exceeded loading configstrings\n" );
			FS_FCloseFile( tvPlay.file );
			return qfalse;
		}

		cl.gameState.stringOffsets[csIdx] = cl.gameState.dataCount;
		Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, csData, csLen + 1 );
		cl.gameState.dataCount += csLen + 1;
	}

	// Inject \tv\1 into CS_SERVERINFO (CL_TV_UpdateConfigstring auto-injects for CS_SERVERINFO)
	{
		const char *si = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
		CL_TV_UpdateConfigstring( CS_SERVERINFO, si, (int)strlen( si ) );
	}

	// Read trailer for duration (before saving frame offset)
	CL_TV_ReadTrailer();

	// Print header info
	{
		if ( tvPlay.totalDuration > 0 ) {
			int secs = tvPlay.totalDuration / 1000;
			int h = secs / 3600;
			int m = ( secs % 3600 ) / 60;
			int s = secs % 60;
			Com_Printf( "TV: %s recorded %s, %i fps, %i maxclients, %02i:%02i:%02i\n",
				mapname, timestamp, tvPlay.svFps, tvPlay.maxclients, h, m, s );
		} else {
			Com_Printf( "TV: %s recorded %s, %i fps, %i maxclients, unknown duration\n",
				mapname, timestamp, tvPlay.svFps, tvPlay.maxclients );
		}
	}

	// Save initial gameState and first frame offset for seeking
	tvPlay.initialGameState = cl.gameState;
	tvPlay.firstFrameOffset = FS_FTell( tvPlay.file );

	// Init zstd decompressor
	tvPlay.dstream = ZSTD_createDStream();
	ZSTD_initDStream( tvPlay.dstream );
	tvPlay.zstdInSize = 0;
	tvPlay.zstdInPos = 0;
	tvPlay.zstdOutSize = 0;
	tvPlay.zstdOutPos = 0;
	tvPlay.zstdStreamEnded = qfalse;

	// Zero entity/player buffers
	Com_Memset( tvPlay.entities, 0, sizeof( tvPlay.entities ) );
	Com_Memset( tvPlay.entityBitmask, 0, sizeof( tvPlay.entityBitmask ) );
	Com_Memset( tvPlay.players, 0, sizeof( tvPlay.players ) );
	Com_Memset( tvPlay.playerBitmask, 0, sizeof( tvPlay.playerBitmask ) );

	// Set initial clientNum
	clc.clientNum = 0;

	// Read first frame and build snapshots[0]
	CL_TV_ReadFrame();
	if ( tvPlay.atEnd ) {
		Com_Printf( S_COLOR_YELLOW "TV: No frames in file\n" );
		FS_FCloseFile( tvPlay.file );
		return qfalse;
	}

	tvPlay.firstServerTime = tvPlay.serverTime;

	// Set viewpoint to first active player
	tvPlay.viewpoint = CL_TV_FindFirstActivePlayer();
	if ( tvPlay.viewpoint < 0 ) {
		tvPlay.viewpoint = 0;
	}
	clc.clientNum = tvPlay.viewpoint;
	VectorCopy( tvPlay.players[tvPlay.viewpoint].origin, tvPlay.viewOrigin );

	CL_TV_BuildSnapshot( 0 );

	// Read second frame and build snapshots[1]
	CL_TV_ReadFrame();
	if ( tvPlay.atEnd ) {
		// Only one frame - duplicate
		tvPlay.snapshots[1] = tvPlay.snapshots[0];
		tvPlay.snapshots[1].messageNum = tvPlay.snapCount++;
		Com_Memcpy( tvPlay.snapEntities[1], tvPlay.snapEntities[0],
			sizeof( tvPlay.snapEntities[0] ) );
	} else {
		CL_TV_BuildSnapshot( 1 );
	}

	tvPlay.active = qtrue;

	// Set cl.snap so CA_PRIMED -> CA_ACTIVE transition works
	cl.snap = tvPlay.snapshots[1];
	cl.newSnapshots = qtrue;

	// Set up server message/command sequences for cgame init
	clc.serverMessageSequence = tvPlay.snapshots[1].messageNum;
	clc.lastExecutedServerCommand = tvPlay.cmdSequence;
	clc.serverCommandSequence = tvPlay.cmdSequence;

	// Register commands
	Cmd_AddCommand( "tv_view", CL_TV_View_f );
	Cmd_AddCommand( "tv_view_next", CL_TV_ViewNext_f );
	Cmd_AddCommand( "tv_view_prev", CL_TV_ViewPrev_f );
	Cmd_AddCommand( "tv_seek", CL_TV_Seek_f );

	// Set duration cvar from header (written by server on recording close)
	Cvar_SetIntegerValue( "cl_tvDuration", tvPlay.totalDuration );
	Cvar_SetIntegerValue( "cl_tvTime", 0 );
	Cvar_SetIntegerValue( "cl_tvViewpoint", tvPlay.viewpoint );

	return qtrue;
}


/*
===============
CL_TV_Close
===============
*/
void CL_TV_Close( void ) {
	if ( tvPlay.dstream ) {
		ZSTD_freeDStream( tvPlay.dstream );
		tvPlay.dstream = NULL;
	}

	if ( tvPlay.file ) {
		FS_FCloseFile( tvPlay.file );
	}

	Cmd_RemoveCommand( "tv_view" );
	Cmd_RemoveCommand( "tv_view_next" );
	Cmd_RemoveCommand( "tv_view_prev" );
	Cmd_RemoveCommand( "tv_seek" );

	Com_Memset( &tvPlay, 0, sizeof( tvPlay ) );
}


/*
===============
CL_TV_ReadFrame

Read one frame from the current file position.
===============
*/
void CL_TV_ReadFrame( void ) {
	unsigned int frameSize;
	msg_t msg;
	int serverTime;
	int num;
	entityState_t tempEntity;
	playerState_t tempPlayer;
	byte oldEntityBitmask[MAX_GENTITIES/8];
	byte oldPlayerBitmask[MAX_CLIENTS/8];
	int csCount, cmdCount;
	int i;
	char csData[BIG_INFO_STRING];

	// Read frame size (4 bytes from compressed stream)
	if ( CL_TV_DecompressRead( &frameSize, 4 ) != 4 || frameSize == 0 ) {
		tvPlay.atEnd = qtrue;
		return;
	}

	if ( frameSize > sizeof( tvPlay.msgBuf ) ) {
		Com_Printf( S_COLOR_YELLOW "TV: Frame too large (%u)\n", frameSize );
		tvPlay.atEnd = qtrue;
		return;
	}

	// Read Huffman-encoded payload from compressed stream
	if ( CL_TV_DecompressRead( tvPlay.msgBuf, frameSize ) != (int)frameSize ) {
		tvPlay.atEnd = qtrue;
		return;
	}

	// Set up message for reading
	MSG_Init( &msg, tvPlay.msgBuf, sizeof( tvPlay.msgBuf ) );
	msg.cursize = frameSize;
	MSG_BeginReading( &msg );

	// Server time
	serverTime = MSG_ReadLong( &msg );

	// --- Entity section ---

	// Save old bitmask for cleanup
	Com_Memcpy( oldEntityBitmask, tvPlay.entityBitmask, sizeof( oldEntityBitmask ) );

	// Read new entity bitmask
	MSG_ReadData( &msg, tvPlay.entityBitmask, MAX_GENTITIES / 8 );

	// Read delta-encoded entities from the bitstream
	while ( 1 ) {
		num = MSG_ReadEntitynum( &msg );
		if ( num == MAX_GENTITIES - 1 ) {
			break;  // end marker
		}

		if ( num < 0 ) {
			// MSG_ReadEntitynum returns -1 when the message buffer is
			// exhausted.  This is normal at the end of a demo file where
			// the final frame may be truncated.
			tvPlay.atEnd = qtrue;
			return;
		}

		if ( num >= MAX_GENTITIES - 1 ) {
			Com_Printf( S_COLOR_YELLOW "TV: Bad entity number %i\n", num );
			tvPlay.atEnd = qtrue;
			return;
		}

		// Delta frame: read into temp, then copy back
		MSG_ReadDeltaEntity( &msg, &tvPlay.entities[num], &tempEntity, num );
		if ( tempEntity.number == MAX_GENTITIES - 1 ) {
			// Entity removed
			Com_Memset( &tvPlay.entities[num], 0, sizeof( entityState_t ) );
		} else {
			tvPlay.entities[num] = tempEntity;
		}
	}

	// Zero entities that left the bitmask to match the writer's baseline.
	// The writer zeroes prevEntities for removed entities (sv_tv.c),
	// so our running state must also be zeroed for correct delta decoding.
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		if ( ( oldEntityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) &&
			 !( tvPlay.entityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			Com_Memset( &tvPlay.entities[i], 0, sizeof( entityState_t ) );
		}
	}

	// --- Player section ---
	Com_Memcpy( oldPlayerBitmask, tvPlay.playerBitmask, sizeof( oldPlayerBitmask ) );
	MSG_ReadData( &msg, tvPlay.playerBitmask, MAX_CLIENTS / 8 );

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		int clientNum;

		if ( !( tvPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		clientNum = MSG_ReadByte( &msg );
		if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
			Com_Printf( S_COLOR_YELLOW "TV: Bad player clientNum %i\n", clientNum );
			tvPlay.atEnd = qtrue;
			return;
		}

		MSG_ReadDeltaPlayerstate( &msg, &tvPlay.players[clientNum], &tempPlayer );
		tvPlay.players[clientNum] = tempPlayer;
	}

	// Zero players that left the bitmask to match the writer's baseline.
	// The writer zeroes prevPlayers for removed players (sv_tv.c),
	// so our running state must also be zeroed for correct delta decoding.
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( ( oldPlayerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) &&
			 !( tvPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			Com_Memset( &tvPlay.players[i], 0, sizeof( playerState_t ) );
		}
	}

	// Auto-switch viewpoint if current player disconnected or became spectator
	// Skip during seek: early replay frames may not yet contain the followed player
	if ( !tvPlay.seeking &&
		 ( !( tvPlay.playerBitmask[tvPlay.viewpoint >> 3] & ( 1 << ( tvPlay.viewpoint & 7 ) ) ) ||
		   CL_TV_GetPlayerTeam( tvPlay.viewpoint ) == TEAM_SPECTATOR ) ) {
		int newVp = CL_TV_FindFirstActivePlayer();
		if ( newVp >= 0 ) {
			tvPlay.viewpoint = newVp;
			clc.clientNum = newVp;
			Cvar_SetIntegerValue( "cl_tvViewpoint", newVp );
		}
	}

	// --- Configstring changes ---
	csCount = MSG_ReadShort( &msg );
	for ( i = 0; i < csCount; i++ ) {
		int csIndex = MSG_ReadShort( &msg );
		int csLen = MSG_ReadShort( &msg );

		if ( csLen > 0 && csLen < (int)sizeof( csData ) ) {
			MSG_ReadData( &msg, csData, csLen );
			csData[csLen] = '\0';
		} else {
			csData[0] = '\0';
		}

		if ( (unsigned)csIndex < MAX_CONFIGSTRINGS ) {
			CL_TV_UpdateConfigstring( csIndex, csData, csLen );

			// Synthesize "cs" command for cgame so it registers new models/sounds/etc.
			// Skip during seek (tv_seek_sync handles bulk re-registration)
			if ( !tvPlay.seeking ) {
				char csCmd[MAX_STRING_CHARS];
				int cmdIdx;
				Com_sprintf( csCmd, sizeof( csCmd ), "cs %i \"%s\"", csIndex, csData );
				cmdIdx = tvPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
				Q_strncpyz( tvPlay.cmds[cmdIdx], csCmd, MAX_STRING_CHARS );
				tvPlay.cmdSequence++;
			}
		}
	}

	// --- Server commands ---
	cmdCount = MSG_ReadShort( &msg );
	for ( i = 0; i < cmdCount; i++ ) {
		int target = MSG_ReadByte( &msg );
		int cmdLen = MSG_ReadShort( &msg );

		if ( cmdLen > 0 && cmdLen < (int)sizeof( csData ) ) {
			MSG_ReadData( &msg, csData, cmdLen );
			csData[cmdLen] = '\0';
		} else {
			csData[0] = '\0';
			cmdLen = 0;
		}

		// Queue if broadcast (255) or targeted at our viewpoint
		// Skip during seek to avoid overflowing the 64-command buffer
		if ( !tvPlay.seeking && ( target == 255 || target == tvPlay.viewpoint ) ) {
			int idx = tvPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
			Q_strncpyz( tvPlay.cmds[idx], csData, MAX_STRING_CHARS );
			tvPlay.cmdSequence++;
		}
	}

	tvPlay.serverTime = serverTime;

	// Track last server time for seek clamping
	if ( serverTime > tvPlay.lastServerTime ) {
		tvPlay.lastServerTime = serverTime;
	}
}


/*
===============
CL_TV_InjectScores

Synthesize a "scores" server command from playerState data
so cgame's scoreboard always has up-to-date information.
===============
*/

// playerState_t.persistant[] indices (from game/bg_public.h)
#define TV_PERS_SCORE					0
#define TV_PERS_RANK					2
#define TV_PERS_KILLED					8
#define TV_PERS_IMPRESSIVE_COUNT		9
#define TV_PERS_EXCELLENT_COUNT		10
#define TV_PERS_DEFEND_COUNT			11
#define TV_PERS_ASSIST_COUNT			12
#define TV_PERS_GAUNTLET_FRAG_COUNT	13
#define TV_PERS_CAPTURES				14

static void CL_TV_InjectScores( void ) {
	char buf[MAX_STRING_CHARS];
	int len, count, i, idx;
	playerState_t *ps;
	int perfect, powerups;

	// Count active players
	count = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( tvPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) {
			count++;
		}
	}

	// "scores <count> <redScore> <blueScore>"
	len = Com_sprintf( buf, sizeof( buf ), "scores %i 0 0", count );

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( !( tvPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		ps = &tvPlay.players[i];
		perfect = ( ps->persistant[TV_PERS_RANK] == 0 &&
					ps->persistant[TV_PERS_KILLED] == 0 ) ? 1 : 0;
		powerups = tvPlay.entities[i].powerups;

		len += Com_sprintf( buf + len, sizeof( buf ) - len,
			" %i %i %i %i %i %i %i %i %i %i %i %i %i %i",
			i,
			ps->persistant[TV_PERS_SCORE],
			0,		// ping
			0,		// time
			0,		// scoreFlags
			powerups,
			0,		// accuracy
			ps->persistant[TV_PERS_IMPRESSIVE_COUNT],
			ps->persistant[TV_PERS_EXCELLENT_COUNT],
			ps->persistant[TV_PERS_GAUNTLET_FRAG_COUNT],
			ps->persistant[TV_PERS_DEFEND_COUNT],
			ps->persistant[TV_PERS_ASSIST_COUNT],
			perfect,
			ps->persistant[TV_PERS_CAPTURES] );
	}

	idx = tvPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	Q_strncpyz( tvPlay.cmds[idx], buf, MAX_STRING_CHARS );
	tvPlay.cmdSequence++;
}


/*
===============
CL_TV_BuildSnapshot

Build tvPlay.snapshots[which] from current running state.
When more than MAX_ENTITIES_IN_SNAPSHOT entities are active,
keeps the nearest ones by distance from the current view origin.
===============
*/

/*
===============
CL_TV_SkipEventEntity

Returns qtrue if a freestanding event entity should be excluded from the
snapshot because it targets a player other than the one being followed.
Events like score plums and voice chats are only meaningful for the
player they belong to.
===============
*/
static qboolean CL_TV_SkipEventEntity( const entityState_t *es ) {
	if ( es->eType == ET_EVENTS + EV_SCOREPLUM &&
		 es->otherEntityNum != tvPlay.viewpoint ) {
		return qtrue;
	}

	return qfalse;
}


typedef struct {
	int		entityNum;
	float	distSq;
} tvEntDist_t;

static int TV_EntDistCompare( const void *a, const void *b ) {
	float da = ((const tvEntDist_t *)a)->distSq;
	float db = ((const tvEntDist_t *)b)->distSq;
	if ( da < db ) return -1;
	if ( da > db ) return 1;
	return 0;
}

void CL_TV_BuildSnapshot( int which ) {
	clSnapshot_t *snap;
	int count, i, total;

	// Inject synthetic scores so cgame scoreboard is always current
	CL_TV_InjectScores();

	snap = &tvPlay.snapshots[which];
	Com_Memset( snap, 0, sizeof( *snap ) );

	snap->valid = qtrue;
	snap->serverTime = tvPlay.serverTime;
	snap->messageNum = tvPlay.snapCount++;
	snap->deltaNum = snap->messageNum - 1;
	snap->snapFlags = 0;
	snap->ping = 0;
	snap->serverCommandNum = tvPlay.cmdSequence;

	// All areas visible (0 = visible, 1 = blocked)
	snap->areabytes = MAX_MAP_AREA_BYTES;
	Com_Memset( snap->areamask, 0x00, sizeof( snap->areamask ) );

	// Player state from followed viewpoint
	snap->ps = tvPlay.players[tvPlay.viewpoint];
	snap->ps.clientNum = tvPlay.viewpoint;

	// Count active entities (excluding viewpoint and filtered events)
	total = 0;
	for ( i = 0; i < MAX_GENTITIES - 1; i++ ) {
		if ( !( tvPlay.entityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) )
			continue;
		if ( i == tvPlay.viewpoint )
			continue;
		if ( CL_TV_SkipEventEntity( &tvPlay.entities[i] ) )
			continue;
		total++;
	}

	if ( total <= MAX_ENTITIES_IN_SNAPSHOT ) {
		// All fit — simple copy, no sorting needed
		count = 0;
		for ( i = 0; i < MAX_GENTITIES - 1; i++ ) {
			if ( !( tvPlay.entityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) )
				continue;
			if ( i == tvPlay.viewpoint )
				continue;
			if ( CL_TV_SkipEventEntity( &tvPlay.entities[i] ) )
				continue;
			tvPlay.snapEntities[which][count] = tvPlay.entities[i];
			count++;
		}
	} else {
		// Too many entities — keep the nearest MAX_ENTITIES_IN_SNAPSHOT
		tvEntDist_t candidates[MAX_GENTITIES];
		int n = 0;

		for ( i = 0; i < MAX_GENTITIES - 1; i++ ) {
			if ( !( tvPlay.entityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) )
				continue;
			if ( i == tvPlay.viewpoint )
				continue;
			if ( CL_TV_SkipEventEntity( &tvPlay.entities[i] ) )
				continue;
			candidates[n].entityNum = i;
			candidates[n].distSq = DistanceSquared(
				tvPlay.viewOrigin, tvPlay.entities[i].pos.trBase );
			n++;
		}

		qsort( candidates, n, sizeof( candidates[0] ), TV_EntDistCompare );

		count = ( n < MAX_ENTITIES_IN_SNAPSHOT ) ? n : MAX_ENTITIES_IN_SNAPSHOT;
		for ( i = 0; i < count; i++ ) {
			tvPlay.snapEntities[which][i] =
				tvPlay.entities[candidates[i].entityNum];
		}
	}

	snap->numEntities = count;
	snap->parseEntitiesNum = which * MAX_ENTITIES_IN_SNAPSHOT;
}


/*
===============
CL_TV_GetSnapshot
===============
*/
qboolean CL_TV_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t *clSnap;
	int idx, i;

	if ( snapshotNumber == tvPlay.snapshots[0].messageNum ) {
		idx = 0;
	} else if ( snapshotNumber == tvPlay.snapshots[1].messageNum ) {
		idx = 1;
	} else {
		return qfalse;
	}

	clSnap = &tvPlay.snapshots[idx];
	if ( !clSnap->valid ) {
		return qfalse;
	}

	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	Com_Memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;

	snapshot->numEntities = clSnap->numEntities;
	for ( i = 0; i < clSnap->numEntities; i++ ) {
		snapshot->entities[i] = tvPlay.snapEntities[idx][i];
	}

	return qtrue;
}


/*
===============
CL_TV_GetCurrentSnapshotNumber
===============
*/
void CL_TV_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	*snapshotNumber = tvPlay.snapshots[1].messageNum;
	*serverTime = tvPlay.snapshots[1].serverTime;
}


/*
===============
CL_TV_GetServerCommand
===============
*/
qboolean CL_TV_GetServerCommand( int serverCommandNumber ) {
	const char *s;
	const char *cmd;
	static char bigConfigString[BIG_INFO_STRING];
	int index;

	if ( tvPlay.cmdSequence - serverCommandNumber >= MAX_RELIABLE_COMMANDS ) {
		Cmd_Clear();
		return qfalse;
	}

	if ( tvPlay.cmdSequence - serverCommandNumber < 0 ) {
		Com_Error( ERR_DROP, "CL_TV_GetServerCommand: requested a command not received" );
		return qfalse;
	}

	index = serverCommandNumber & ( MAX_RELIABLE_COMMANDS - 1 );
	s = tvPlay.cmds[index];
	clc.lastExecutedServerCommand = serverCommandNumber;

rescan:
	Cmd_TokenizeString( s );
	cmd = Cmd_Argv( 0 );

	if ( !strcmp( cmd, "disconnect" ) ) {
		// Ignore disconnect commands during TV demo playback
		Cmd_Clear();
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs0" ) ) {
		Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv(1), Cmd_Argv(2) );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs1" ) ) {
		s = Cmd_Argv(2);
		if ( strlen( bigConfigString ) + strlen( s ) >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs2" ) ) {
		s = Cmd_Argv(2);
		if ( strlen( bigConfigString ) + strlen( s ) + 1 >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		strcat( bigConfigString, "\"" );
		s = bigConfigString;
		goto rescan;
	}

	if ( !strcmp( cmd, "cs" ) ) {
		// Apply configstring change to cl.gameState
		int csIndex = atoi( Cmd_Argv(1) );
		const char *csValue = Cmd_ArgsFrom(2);

		if ( (unsigned)csIndex < MAX_CONFIGSTRINGS ) {
			CL_TV_UpdateConfigstring( csIndex, csValue, (int)strlen( csValue ) );
		}
		// Re-tokenize since UpdateConfigstring may have clobbered it
		Cmd_TokenizeString( s );
		return qtrue;
	}

	if ( !strcmp( cmd, "map_restart" ) ) {
		Con_ClearNotify();
		Cmd_TokenizeString( s );
		return qtrue;
	}

	return qtrue;
}


/*
===============
CL_TV_Seek
===============
*/
void CL_TV_Seek( int targetTime ) {
	if ( !tvPlay.active ) {
		return;
	}

	// Clamp
	if ( targetTime < tvPlay.firstServerTime ) {
		targetTime = tvPlay.firstServerTime;
	}
	if ( tvPlay.totalDuration > 0 && targetTime > tvPlay.firstServerTime + tvPlay.totalDuration ) {
		targetTime = tvPlay.firstServerTime + tvPlay.totalDuration;
	}

	if ( targetTime >= tvPlay.serverTime && !tvPlay.atEnd ) {
		// Forward seek: continue streaming from current position
		// Entity/player delta state and configstrings are already correct
		tvPlay.seeking = qtrue;

		while ( tvPlay.serverTime < targetTime && !tvPlay.atEnd ) {
			CL_TV_ReadFrame();
		}

		tvPlay.seeking = qfalse;
	} else {
		// Backward seek: full reset required

		// Restore initial gameState (configstrings are delta-encoded from header)
		cl.gameState = tvPlay.initialGameState;

		// Seek to the first frame and reset all running state
		FS_Seek( tvPlay.file, tvPlay.firstFrameOffset, FS_SEEK_SET );
		Com_Memset( tvPlay.entities, 0, sizeof( tvPlay.entities ) );
		Com_Memset( tvPlay.entityBitmask, 0, sizeof( tvPlay.entityBitmask ) );
		Com_Memset( tvPlay.players, 0, sizeof( tvPlay.players ) );
		Com_Memset( tvPlay.playerBitmask, 0, sizeof( tvPlay.playerBitmask ) );
		tvPlay.serverTime = 0;
		tvPlay.atEnd = qfalse;

		// Reset zstd decompressor session (without freeing context)
		ZSTD_DCtx_reset( tvPlay.dstream, ZSTD_reset_session_only );
		tvPlay.zstdInSize = 0;
		tvPlay.zstdInPos = 0;
		tvPlay.zstdOutSize = 0;
		tvPlay.zstdOutPos = 0;
		tvPlay.zstdStreamEnded = qfalse;

		// Skip command queueing during seek to avoid buffer overflow
		tvPlay.seeking = qtrue;

		// Read ALL frames from the beginning to ensure configstrings are correct
		while ( tvPlay.serverTime < targetTime && !tvPlay.atEnd ) {
			CL_TV_ReadFrame();
		}

		tvPlay.seeking = qfalse;
	}

	// Build both snapshots
	CL_TV_BuildSnapshot( 0 );

	if ( !tvPlay.atEnd ) {
		CL_TV_ReadFrame();
		CL_TV_BuildSnapshot( 1 );
	} else {
		tvPlay.snapshots[1] = tvPlay.snapshots[0];
		tvPlay.snapshots[1].messageNum = tvPlay.snapCount++;
		Com_Memcpy( tvPlay.snapEntities[1], tvPlay.snapEntities[0],
			sizeof( tvPlay.snapEntities[0] ) );
	}

	// Inject sync command so cgame re-fetches gamestate
	{
		int idx = tvPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
		Com_sprintf( tvPlay.cmds[idx], MAX_STRING_CHARS, "tv_seek_sync %i",
			tvPlay.viewpoint );
		tvPlay.cmdSequence++;
	}

	// Update snapshot serverCommandNum to include the sync command
	tvPlay.snapshots[1].serverCommandNum = tvPlay.cmdSequence;

	// Update client state
	cl.snap = tvPlay.snapshots[1];
	cl.newSnapshots = qtrue;
	cl.serverTimeDelta = tvPlay.snapshots[1].serverTime - cls.realtime;
	cl.oldServerTime = tvPlay.snapshots[0].serverTime;
	cl.oldFrameServerTime = tvPlay.snapshots[0].serverTime;

	Cvar_SetIntegerValue( "cl_tvTime",
		tvPlay.serverTime - tvPlay.firstServerTime );
}


/*
===============
CL_TV_RebuildSnapshots

Rebuild both snapshots after a viewpoint change.
===============
*/
static void CL_TV_RebuildSnapshots( void ) {
	int savedCount = tvPlay.snapCount;

	// Rebuild both snapshots with new viewpoint
	tvPlay.snapCount = savedCount - 2;
	if ( tvPlay.snapCount < 0 ) {
		tvPlay.snapCount = 0;
	}

	CL_TV_BuildSnapshot( 0 );
	CL_TV_BuildSnapshot( 1 );

	// Make messageNums consecutive
	tvPlay.snapshots[1].messageNum = tvPlay.snapshots[0].messageNum + 1;

	cl.snap = tvPlay.snapshots[1];
	cl.newSnapshots = qtrue;

	clc.clientNum = tvPlay.viewpoint;
	Cvar_SetIntegerValue( "cl_tvViewpoint", tvPlay.viewpoint );
}


/*
===============
CL_TV_View_f
===============
*/
static void CL_TV_View_f( void ) {
	int n;

	if ( !tvPlay.active ) {
		Com_Printf( "Not playing a TV demo.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "tv_view <clientnum>\n" );
		return;
	}

	n = atoi( Cmd_Argv( 1 ) );
	if ( n < 0 || n >= MAX_CLIENTS ) {
		Com_Printf( "Invalid client number %i\n", n );
		return;
	}

	if ( !( tvPlay.playerBitmask[n >> 3] & ( 1 << ( n & 7 ) ) ) ) {
		Com_Printf( "Client %i is not active\n", n );
		return;
	}

	if ( CL_TV_GetPlayerTeam( n ) == TEAM_SPECTATOR ) {
		Com_Printf( "Client %i is a spectator\n", n );
		return;
	}

	tvPlay.viewpoint = n;
	CL_TV_RebuildSnapshots();
}


/*
===============
CL_TV_ViewNext_f
===============
*/
static void CL_TV_ViewNext_f( void ) {
	int i, next;

	if ( !tvPlay.active ) {
		Com_Printf( "Not playing a TV demo.\n" );
		return;
	}

	next = tvPlay.viewpoint;
	for ( i = 1; i <= MAX_CLIENTS; i++ ) {
		int candidate = ( tvPlay.viewpoint + i ) % MAX_CLIENTS;
		if ( ( tvPlay.playerBitmask[candidate >> 3] & ( 1 << ( candidate & 7 ) ) )
				&& CL_TV_GetPlayerTeam( candidate ) != TEAM_SPECTATOR ) {
			next = candidate;
			break;
		}
	}

	if ( next != tvPlay.viewpoint ) {
		tvPlay.viewpoint = next;
		CL_TV_RebuildSnapshots();
	}
}


/*
===============
CL_TV_ViewPrev_f
===============
*/
static void CL_TV_ViewPrev_f( void ) {
	int i, prev;

	if ( !tvPlay.active ) {
		Com_Printf( "Not playing a TV demo.\n" );
		return;
	}

	prev = tvPlay.viewpoint;
	for ( i = 1; i <= MAX_CLIENTS; i++ ) {
		int candidate = ( tvPlay.viewpoint - i + MAX_CLIENTS ) % MAX_CLIENTS;
		if ( ( tvPlay.playerBitmask[candidate >> 3] & ( 1 << ( candidate & 7 ) ) )
				&& CL_TV_GetPlayerTeam( candidate ) != TEAM_SPECTATOR ) {
			prev = candidate;
			break;
		}
	}

	if ( prev != tvPlay.viewpoint ) {
		tvPlay.viewpoint = prev;
		CL_TV_RebuildSnapshots();
	}
}


/*
===============
CL_TV_Seek_f
===============
*/
static void CL_TV_Seek_f( void ) {
	int seconds;

	if ( !tvPlay.active ) {
		Com_Printf( "Not playing a TV demo.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "tv_seek <seconds>\n" );
		return;
	}

	seconds = atoi( Cmd_Argv( 1 ) );
	CL_TV_Seek( tvPlay.firstServerTime + seconds * 1000 );
}


/*
===============
CL_TV_GetPlayerList

Returns a tab/newline-delimited string of active players for the web UI.
Format: "<viewpoint>\n<clientnum>\t<name>\t<team>\n..."
===============
*/
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *CL_TV_GetPlayerList( void ) {
	static char buf[4096];
	int len, i;
	const char *cs;

	if ( !tvPlay.active ) {
		buf[0] = '\0';
		return buf;
	}

	// First line: current viewpoint
	len = Com_sprintf( buf, sizeof( buf ), "%i\n", tvPlay.viewpoint );

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		char nameBuf[MAX_QPATH], modelBuf[MAX_QPATH];
		int isVR;

		if ( !( tvPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}
		cs = cl.gameState.stringData + cl.gameState.stringOffsets[CS_PLAYERS + i];

		// Info_ValueForKey uses a 2-entry static buffer, so copy results
		// before making more than 2 calls
		Q_strncpyz( nameBuf, Info_ValueForKey( cs, "n" ), sizeof( nameBuf ) );
		Q_strncpyz( modelBuf, Info_ValueForKey( cs, "model" ), sizeof( modelBuf ) );
		isVR = atoi( Info_ValueForKey( cs, "vr" ) );

		len += Com_sprintf( buf + len, sizeof( buf ) - len, "%i\t%s\t%i\t%s\t%i\n",
			i, nameBuf, CL_TV_GetPlayerTeam( i ), modelBuf, isVR );
	}

	return buf;
}
