// cl_voip.c -- client VOIP implementation
// Ported from ioq3

#include "client.h"

#ifdef USE_VOIP

// Cvar definitions
cvar_t *cl_voipUseVAD;
cvar_t *cl_voipVADThreshold;
cvar_t *cl_voipSend;
cvar_t *cl_voipSendTarget;
cvar_t *cl_voipGainDuringCapture;
cvar_t *cl_voipCaptureMult;
cvar_t *cl_voipShowMeter;
cvar_t *cl_voipVolume;
cvar_t *cl_voip;

static cvar_t *cl_voipProtocol;
static cvar_t *cl_voipMuteSpatial;
static cvar_t *cl_voipMuteDirect;
static cvar_t *cl_voipMuteTeam;
static cvar_t *cl_voipMuteAll;
static cvar_t *cl_voipVADMuted;



/*
===============
CL_VoipCvarInit

Register all cl_voip* cvars
===============
*/
void CL_VoipCvarInit( void )
{
	cl_voipSend = Cvar_Get( "cl_voipSend", "0", 0 );
	cl_voipSendTarget = Cvar_Get( "cl_voipSendTarget", "spatial", 0 );
	cl_voipGainDuringCapture = Cvar_Get( "cl_voipGainDuringCapture", "0.2", CVAR_ARCHIVE );
	cl_voipCaptureMult = Cvar_Get( "cl_voipCaptureMult", "2.0", CVAR_ARCHIVE );
	cl_voipUseVAD = Cvar_Get( "cl_voipUseVAD", "0", CVAR_ARCHIVE );
	cl_voipVADThreshold = Cvar_Get( "cl_voipVADThreshold", "0.1", CVAR_ARCHIVE );
	cl_voipShowMeter = Cvar_Get( "cl_voipShowMeter", "1", CVAR_ARCHIVE );
	cl_voipVolume = Cvar_Get( "cl_voipVolume", "1.0", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_voipVolume, "0", "2", CV_FLOAT );
	Cvar_SetDescription( cl_voipVolume, "Sets volume for incoming VOIP audio (0.0 - 2.0, allows boost)." );

	cl_voip = Cvar_Get( "cl_voip", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_voip, "0", "1", CV_INTEGER );
	cl_voipProtocol = Cvar_Get( "cl_voipProtocol", cl_voip->integer ? "opus" : "", CVAR_USERINFO | CVAR_ROM );
	Cvar_Get( "cl_voipVersion", "2", CVAR_USERINFO | CVAR_ROM );
	cl_voipMuteSpatial = Cvar_Get( "cl_voipMuteSpatial", "0", CVAR_ARCHIVE_ND );
	cl_voipMuteDirect = Cvar_Get( "cl_voipMuteDirect", "0", CVAR_ARCHIVE_ND );
	cl_voipMuteTeam = Cvar_Get( "cl_voipMuteTeam", "0", CVAR_ARCHIVE_ND );
	cl_voipMuteAll = Cvar_Get( "cl_voipMuteAll", "0", CVAR_ARCHIVE_ND );
	cl_voipVADMuted = Cvar_Get( "cl_voipVADMuted", "0", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( cl_voipVADMuted, "When set, VAD-captured audio frames are discarded. Inert in PTT mode." );
}


/*
===============
CL_InitVoip

Create Opus encoder + MAX_CLIENTS decoders.
Called on first snapshot from cl_cgame.c.
===============
*/
void CL_InitVoip( void )
{
	int i;
	int error;

	if ( clc.voipCodecInitialized )
		return;

	clc.opusEncoder = opus_encoder_create( 48000, 1, OPUS_APPLICATION_VOIP, &error );

	if ( error ) {
		Com_DPrintf( "VoIP: Error opus_encoder_create %d\n", error );
		return;
	}

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		clc.opusDecoder[i] = opus_decoder_create( 48000, 1, &error );
		if ( error ) {
			int j;
			Com_DPrintf( "VoIP: Error opus_decoder_create(%d) %d\n", i, error );
			// Clean up already-created decoders and encoder
			for ( j = 0; j < i; j++ ) {
				opus_decoder_destroy( clc.opusDecoder[j] );
				clc.opusDecoder[j] = NULL;
			}
			opus_encoder_destroy( clc.opusEncoder );
			clc.opusEncoder = NULL;
			return;
		}
		clc.voipIgnore[i] = qfalse;
		clc.voipGain[i] = 1.0f;
	}
	clc.voipCodecInitialized = qtrue;
	clc.voipMuteAll = qfalse;
	Cmd_AddCommand( "voip", CL_Voip_f );
	Cvar_Set( "cl_voipSendTarget", "spatial" );
	Com_Memset( clc.voipTargets, ~0, sizeof( clc.voipTargets ) );
}


/*
===============
CL_ShutdownVoip

Destroy encoder/decoders and clean up VoIP state.
Called from CL_Disconnect.
===============
*/
void CL_ShutdownVoip( void )
{
	if ( cl_voipSend->integer ) {
		int tmp = cl_voipUseVAD->integer;
		cl_voipUseVAD->integer = 0;  // disable this for a moment.
		clc.voipOutgoingDataSize = 0;  // dump any pending VoIP transmission.
		Cvar_Set( "cl_voipSend", "0" );
		CL_CaptureVoip();  // clean up any state...
		cl_voipUseVAD->integer = tmp;
	}

	if ( clc.voipCodecInitialized ) {
		int i;
		opus_encoder_destroy( clc.opusEncoder );
		for ( i = 0; i < MAX_CLIENTS; i++ ) {
			opus_decoder_destroy( clc.opusDecoder[i] );
		}
		clc.voipCodecInitialized = qfalse;
	}
	Cmd_RemoveCommand( "voip" );
}


/*
===============
CL_VoipNewGeneration

Reset encoder for new transmission
===============
*/
static void CL_VoipNewGeneration( void )
{
	// don't have a zero generation so new clients won't match, and don't
	//  wrap to negative so MSG_ReadLong() doesn't "fail."
	clc.voipOutgoingGeneration++;
	if ( clc.voipOutgoingGeneration <= 0 )
		clc.voipOutgoingGeneration = 1;
	clc.voipPower = 0.0f;
	clc.voipOutgoingSequence = 0;

	opus_encoder_ctl( clc.opusEncoder, OPUS_RESET_STATE );
}


/*
===============
CL_VoipParseTargets

Sets clc.voipTargets according to cl_voipSendTarget.
Generally we don't want who's listening to change during a transmission,
so this is only called when the key is first pressed.
===============
*/
static void CL_VoipParseTargets( void )
{
	const char *target = cl_voipSendTarget->string;
	char *end;
	int val;

	Com_Memset( clc.voipTargets, 0, sizeof( clc.voipTargets ) );
	clc.voipFlags = 0;

	while ( target ) {
		while ( *target == ',' || *target == ' ' )
			target++;

		if ( !*target )
			break;

		if ( isdigit( *target ) ) {
			val = strtol( target, &end, 10 );
			target = end;
		} else {
			if ( !Q_stricmpn( target, "all", 3 ) ) {
				clc.voipFlags |= VOIP_ALL;
				Com_Memset( clc.voipTargets, ~0, sizeof( clc.voipTargets ) );
				target += 3;
				continue;
			}
			if ( !Q_stricmpn( target, "spatial", 7 ) ) {
				clc.voipFlags |= VOIP_SPATIAL;
				target += 7;
				continue;
			} else {
				if ( !Q_stricmpn( target, "team", 4 ) ) {
					clc.voipFlags |= VOIP_TEAM;
					// fallback: populate recips bitmask for old servers
					if ( VM_Call( cgvm, 0, CG_VOIP_TEAM ) == 0 ) {
						char teamIds[256];
						const char *p;
						char *e;
						Cvar_VariableStringBuffer( "cl_voipTeamTargets", teamIds, sizeof( teamIds ) );
						p = teamIds;
						while ( *p ) {
							while ( *p == ',' || *p == ' ' ) p++;
							if ( !*p ) break;
							val = strtol( p, &e, 10 );
							p = e;
							if ( val >= 0 && val < MAX_CLIENTS ) {
								clc.voipTargets[val / 8] |= 1 << (val % 8);
							}
						}
					}
					target += 4;
					continue;
				} else if ( !Q_stricmpn( target, "attacker", 8 ) ) {
					val = VM_Call( cgvm, 0, CG_LAST_ATTACKER );
					target += 8;
				} else if ( !Q_stricmpn( target, "crosshair", 9 ) ) {
					val = VM_Call( cgvm, 0, CG_CROSSHAIR_PLAYER );
					target += 9;
				} else {
					while ( *target && *target != ',' && *target != ' ' )
						target++;

					continue;
				}

				if ( val < 0 )
					continue;
			}
		}

		if ( val < 0 || val >= MAX_CLIENTS ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: VoIP "
				   "target %d is not a valid client "
				   "number\n", val );

			continue;
		}

		clc.voipTargets[val / 8] |= 1 << (val % 8);
		clc.voipFlags |= VOIP_DIRECT;
	}
}


/*
===============
CL_CaptureVoip

Record more audio from the hardware if required and encode it into Opus
data for later transmission.
===============
*/
void CL_CaptureVoip( void )
{
	const float audioMult = cl_voipCaptureMult->value;
	const qboolean useVad = (cl_voipUseVAD->integer != 0);
	qboolean initialFrame = qfalse;
	qboolean finalFrame = qfalse;

	// If your data rate is too low, you'll get Connection Interrupted warnings
	//  when VoIP packets arrive, even if you have a broadband connection.
	//  This might work on rates lower than 25000, but for safety's sake, we'll
	//  just demand it. Who doesn't have at least a DSL line now, anyhow? If
	//  you don't, you don't need VoIP.  :)
	if ( cl_voip->modified ) {
		if ( (cl_voip->integer) && (Cvar_VariableIntegerValue( "rate" ) < 25000) ) {
			Com_Printf( S_COLOR_YELLOW "Your network rate is too slow for VoIP.\n" );
			Com_Printf( "Set 'Data Rate' to 'LAN/Cable/xDSL' in 'Setup/System/Network'.\n" );
			Com_Printf( "Until then, VoIP is disabled.\n" );
			Cvar_Set( "cl_voip", "0" );
		}
		Cvar_Set( "cl_voipProtocol", cl_voip->integer ? "opus" : "" );
		cl_voip->modified = qfalse;
	}

	if ( !clc.voipCodecInitialized )
		return;  // just in case this gets called at a bad time.

	if ( clc.voipOutgoingDataSize > 0 )
		return;  // packet is pending transmission, don't record more yet.

	if ( cl_voipUseVAD->modified ) {
		Cvar_Set( "cl_voipSend", (useVad) ? "1" : "0" );
		cl_voipUseVAD->modified = qfalse;
	}

	if ( (useVad) && (!cl_voipSend->integer) )
		Cvar_Set( "cl_voipSend", "1" );  // lots of things reset this.

	if ( cl_voipSend->modified ) {
		qboolean dontCapture = qfalse;
		if ( cls.state != CA_ACTIVE )
			dontCapture = qtrue;  // not connected to a server.
		else if ( !clc.voipEnabled )
			dontCapture = qtrue;  // server doesn't support VoIP.
		else if ( clc.demoplaying )
			dontCapture = qtrue;  // playing back a demo.
		else if ( cl_voip->integer == 0 )
			dontCapture = qtrue;  // client has VoIP support disabled.
		else if ( audioMult == 0.0f )
			dontCapture = qtrue;  // basically silenced incoming audio.

		cl_voipSend->modified = qfalse;

		if ( dontCapture ) {
			Cvar_Set( "cl_voipSend", "0" );
			return;
		}

		if ( cl_voipSend->integer ) {
			initialFrame = qtrue;
		} else {
			finalFrame = qtrue;
		}
	}

	// try to get more audio data from the sound card...

	if ( initialFrame ) {
		// Check if capture is available before starting
		if ( S_AvailableCaptureSamples() < 0 ) {
			Cvar_Set( "cl_voipSend", "0" );
			return;
		}
		S_MasterGain( Com_Clamp( 0.0f, 1.0f, cl_voipGainDuringCapture->value ) );

		S_StartCapture();

		// Drain stale samples that arrived between stop and start
		{
			int stale = S_AvailableCaptureSamples();
			if ( stale > 0 ) {
				static int16_t devnull[VOIP_MAX_PACKET_SAMPLES];
				while ( stale > 0 ) {
					int chunk = (stale > VOIP_MAX_PACKET_SAMPLES) ? VOIP_MAX_PACKET_SAMPLES : stale;
					S_Capture( chunk, (byte *) devnull );
					stale -= chunk;
				}
			}
		}

		CL_VoipNewGeneration();
		CL_VoipParseTargets();
	}

	if ( (cl_voipSend->integer) || (finalFrame) ) { // user wants to capture audio?
		int samples = S_AvailableCaptureSamples();
		const int packetSamples = (finalFrame) ? VOIP_MAX_FRAME_SAMPLES : VOIP_MAX_PACKET_SAMPLES;

		// If no capture device available, skip gracefully
		if ( samples <= 0 && !cl_voipSend->integer ) {
			return;
		}

		// enough data buffered in audio hardware to process yet?
		// On finalFrame, accept any samples > 0 and zero-pad to frame boundary
		if ( samples >= packetSamples || (finalFrame && samples > 0) ) {
			// audio capture is always MONO16.
			static int16_t sampbuffer[VOIP_MAX_PACKET_SAMPLES];
			float voipPower = 0.0f;
			int voipFrames;
			int i, bytes;
			int actualSamples;

			if ( samples > VOIP_MAX_PACKET_SAMPLES )
				samples = VOIP_MAX_PACKET_SAMPLES;

			// Capture the real samples first, then handle rounding
			actualSamples = samples;

			if ( finalFrame ) {
				// Round UP to next frame boundary so we don't lose the tail
				samples = ((samples + VOIP_MAX_FRAME_SAMPLES - 1) / VOIP_MAX_FRAME_SAMPLES) * VOIP_MAX_FRAME_SAMPLES;
				if ( samples > VOIP_MAX_PACKET_SAMPLES )
					samples = VOIP_MAX_PACKET_SAMPLES;
			} else {
				// Normal path: round down to frame boundary
				samples -= samples % VOIP_MAX_FRAME_SAMPLES;
				actualSamples = samples;  // only capture the rounded amount
			}

			if ( samples <= 0 ) {
				Com_DPrintf( "VoIP: no complete frames to encode\n" );
				return;
			}
			voipFrames = samples / VOIP_MAX_FRAME_SAMPLES;

			S_Capture( actualSamples, (byte *) sampbuffer );  // grab from audio card.

			// Zero-pad any remainder on the final frame
			if ( actualSamples < samples ) {
				Com_Memset( sampbuffer + actualSamples, 0, (samples - actualSamples) * sizeof( int16_t ) );
			}

			// check the "power" of this packet...
			for ( i = 0; i < actualSamples; i++ ) {
				const float flsamp = (float) sampbuffer[i];
				const float s = fabs( flsamp );
				voipPower += s * s;
				sampbuffer[i] = (int16_t) Com_Clamp( -32768.0f, 32767.0f, flsamp * audioMult );
			}

			// encode raw audio samples into Opus data...
			bytes = opus_encode( clc.opusEncoder, sampbuffer, samples,
									(unsigned char *) clc.voipOutgoingData,
									sizeof( clc.voipOutgoingData ) );
			if ( bytes <= 0 ) {
				Com_DPrintf( "VoIP: Error encoding %d samples\n", samples );
				bytes = 0;
			}

			clc.voipPower = (voipPower / (32768.0f * 32768.0f *
			                 ((float) (actualSamples ? actualSamples : 1)))) * 100.0f;

			{
				qboolean discard = qfalse;
				qboolean hangover = qfalse;

				if ( useVad ) {
					if ( cl_voipVADMuted->integer ) {
						discard = qtrue;
					} else if ( clc.voipPower < cl_voipVADThreshold->value ) {
						// Below threshold — bridge brief dips so the wire
						// behavior matches the HUD icon's decay window.
						if ( clc.voipLastSelfSendTime > 0
							&& cls.realtime - clc.voipLastSelfSendTime < VOIP_TALKING_TIMEOUT ) {
							hangover = qtrue;
						} else {
							discard = qtrue;
						}
					}
				}

				if ( discard ) {
					CL_VoipNewGeneration();  // no "talk" for at least 1/4 second.
				} else {
					clc.voipOutgoingDataSize = bytes;
					clc.voipOutgoingDataFrames = voipFrames;
					// Only advance the timestamp on frames that genuinely cross
					// the threshold; hang-over frames ride the existing window
					// so sustained silence still terminates the stream.
					if ( !hangover ) {
						clc.voipLastSelfSendTime = cls.realtime;
					}

					Com_DPrintf( "VoIP: Send %d frames, %d bytes, %f power%s\n",
					            voipFrames, bytes, clc.voipPower,
					            hangover ? " (hangover)" : "" );
				}
			}
		}
	}

	// User requested we stop recording, and we've now processed the last of
	//  any previously-buffered data. Pause the capture device, etc.
	if ( finalFrame ) {
		S_StopCapture();

		// Drain any remaining samples so they don't leak into the next
		// generation — alcCaptureStop does not discard buffered data.
		{
			int remaining = S_AvailableCaptureSamples();
			if ( remaining > 0 ) {
				static int16_t devnull[VOIP_MAX_PACKET_SAMPLES];
				while ( remaining > 0 ) {
					int chunk = (remaining > VOIP_MAX_PACKET_SAMPLES) ? VOIP_MAX_PACKET_SAMPLES : remaining;
					S_Capture( chunk, (byte *) devnull );
					remaining -= chunk;
				}
			}
		}

		S_MasterGain( 1.0f );
		clc.voipPower = 0.0f;  // force this value so it doesn't linger.
	}
}


/*
===============
CL_WriteVoip

Write clc_voipOpus to outgoing message. Also writes fake svc_voipOpus
messages into demos if recording.
===============
*/
void CL_WriteVoip( msg_t *msg )
{
	if ( clc.voipOutgoingDataSize <= 0 )
		return;

	if ( (clc.voipFlags & VOIP_SPATIAL) || Com_IsVoipTarget( clc.voipTargets, sizeof( clc.voipTargets ), -1 ) ) {
		MSG_WriteByte( msg, clc_voipOpus );
		MSG_WriteByte( msg, clc.voipOutgoingGeneration );
		MSG_WriteLong( msg, clc.voipOutgoingSequence );
		MSG_WriteByte( msg, clc.voipOutgoingDataFrames );
		MSG_WriteData( msg, clc.voipTargets, sizeof( clc.voipTargets ) );
		MSG_WriteByte( msg, clc.voipFlags );
		MSG_WriteShort( msg, clc.voipOutgoingDataSize );
		MSG_WriteData( msg, clc.voipOutgoingData, clc.voipOutgoingDataSize );

		// If we're recording a demo, we have to fake a server packet with
		//  this VoIP data so it gets to disk; the server doesn't send it
		//  back to us, and we might as well eliminate concerns about dropped
		//  and misordered packets here.
		if ( clc.demorecording && !clc.demowaiting ) {
			const int voipSize = clc.voipOutgoingDataSize;
			msg_t fakemsg;
			byte fakedata[MAX_MSGLEN];

			MSG_Init( &fakemsg, fakedata, sizeof( fakedata ) );
			MSG_Bitstream( &fakemsg );
			MSG_WriteLong( &fakemsg, clc.reliableAcknowledge );
			MSG_WriteByte( &fakemsg, svc_voipOpus );
			MSG_WriteShort( &fakemsg, clc.clientNum );
			MSG_WriteByte( &fakemsg, clc.voipOutgoingGeneration );
			MSG_WriteLong( &fakemsg, clc.voipOutgoingSequence );
			MSG_WriteByte( &fakemsg, clc.voipOutgoingDataFrames );
			MSG_WriteShort( &fakemsg, clc.voipOutgoingDataSize );
			MSG_WriteBits( &fakemsg, clc.voipFlags, VOIP_FLAGCNT );
			MSG_WriteData( &fakemsg, clc.voipOutgoingData, voipSize );
			MSG_WriteByte( &fakemsg, svc_EOF );

			CL_WriteDemoMessage( &fakemsg, 0 );
		}

		clc.voipOutgoingSequence += clc.voipOutgoingDataFrames;
		clc.voipOutgoingDataSize = 0;
		clc.voipOutgoingDataFrames = 0;
	} else {
		// We have data, but no targets. Silently discard all data
		clc.voipOutgoingDataSize = 0;
		clc.voipOutgoingDataFrames = 0;
	}
}


/*
===============
CL_ShouldIgnoreVoipSender

Check if sender is ignored/muted
===============
*/
qboolean CL_ShouldIgnoreVoipSender( int sender )
{
	if ( !cl_voip->integer )
		return qtrue;  // VoIP is disabled.
	else if ( (sender == clc.clientNum) && (!clc.demoplaying) && (!tvPlay.active) )
		return qtrue;  // ignore own voice (unless playing back a demo or TVD).
	else if ( clc.voipMuteAll )
		return qtrue;  // all channels are muted with extreme prejudice.
	else if ( clc.voipIgnore[sender] )
		return qtrue;  // just ignoring this guy.
	else if ( clc.voipGain[sender] == 0.0f )
		return qtrue;  // too quiet to play.

	return qfalse;
}


/*
=====================
CL_PlayVoip

Play raw decoded VoIP data through the appropriate audio streams.
Direct VOIP goes to a per-sender stream; spatial VOIP is spatialized
relative to the sender's entity.
=====================
*/
static void CL_PlayVoip( int sender, int samplecnt, const byte *data, int flags )
{
	float vol = cl_voipVolume->value;

	// Filter out muted channels
	if ( cl_voipMuteSpatial->integer ) flags &= ~VOIP_SPATIAL;
	if ( cl_voipMuteDirect->integer )  flags &= ~VOIP_DIRECT;
	if ( cl_voipMuteTeam->integer )    flags &= ~VOIP_TEAM;
	if ( cl_voipMuteAll->integer )     flags &= ~VOIP_ALL;

	// Play as non-spatialized (direct) audio if any non-spatial flag is set.
	// VOIP_DIRECT, VOIP_TEAM, and VOIP_ALL all play the same way — the
	// distinction is routing metadata, not a playback mode.
	if ( flags & ( VOIP_DIRECT | VOIP_TEAM | VOIP_ALL ) ) {
		S_RawSamples( sender + 1, samplecnt, 48000, 2, 1,
			data, clc.voipGain[sender] * vol, -1 );
	}

	if ( flags & VOIP_SPATIAL ) {
		S_RawSamples( sender + MAX_CLIENTS + 1, samplecnt, 48000, 2, 1,
			data, clc.voipGain[sender] * vol, sender );
	}
}


/*
=====================
CL_ParseVoip

A VoIP message has been received from the server
=====================
*/
void CL_ParseVoip( msg_t *msg, qboolean ignoreData )
{
	static short decoded[VOIP_MAX_PACKET_SAMPLES * 4];

	const int sender = MSG_ReadShort( msg );
	const int generation = MSG_ReadByte( msg );
	const int sequence = MSG_ReadLong( msg );
	const int frames = MSG_ReadByte( msg );
	const int packetsize = MSG_ReadShort( msg );
	const int flagBits = ( clc.svVoipVersion >= 2 ) ? VOIP_FLAGCNT : VOIP_FLAGCNT_V1;
	const int flags = MSG_ReadBits( msg, flagBits );
	unsigned char encoded[4000];
	int numSamples;
	int seqdiff;
	int written = 0;
	int i;

	Com_DPrintf( "VoIP: %d-byte packet from client %d\n", packetsize, sender );

	if ( sender < 0 )
		return;   // short/invalid packet, bail.
	else if ( generation < 0 )
		return;   // short/invalid packet, bail.
	else if ( sequence < 0 )
		return;   // short/invalid packet, bail.
	else if ( frames < 0 )
		return;   // short/invalid packet, bail.
	else if ( packetsize < 0 )
		return;   // short/invalid packet, bail.

	if ( packetsize > (int) sizeof( encoded ) ) {  // overlarge packet?
		int bytesleft = packetsize;
		while ( bytesleft ) {
			int br = bytesleft;
			if ( br > (int) sizeof( encoded ) )
				br = sizeof( encoded );
			MSG_ReadData( msg, encoded, br );
			bytesleft -= br;
		}
		return;   // overlarge packet, bail.
	}

	MSG_ReadData( msg, encoded, packetsize );

	if ( ignoreData ) {
		return; // just ignore legacy speex voip data
	} else if ( !clc.voipCodecInitialized ) {
		return;   // can't handle VoIP without libopus!
	} else if ( sender >= MAX_CLIENTS ) {
		return;   // bogus sender.
	} else if ( CL_ShouldIgnoreVoipSender( sender ) ) {
		return;   // Channel is muted, bail.
	}

	Com_DPrintf( "VoIP: packet accepted!\n" );

	clc.voipLastPacketTime[sender] = cls.realtime;
	clc.voipLastChannel[sender] = flags & ( VOIP_TEAM | VOIP_ALL | VOIP_SPATIAL | VOIP_DIRECT );

	seqdiff = sequence - clc.voipIncomingSequence[sender];

	// This is a new "generation" ... a new recording started, reset the bits.
	if ( generation != clc.voipIncomingGeneration[sender] ) {
		Com_DPrintf( "VoIP: new generation %d!\n", generation );
		opus_decoder_ctl( clc.opusDecoder[sender], OPUS_RESET_STATE );
		clc.voipIncomingGeneration[sender] = generation;
		seqdiff = 0;
	} else if ( seqdiff < 0 ) {   // we're ahead of the sequence?!
		// This shouldn't happen unless the packet is corrupted or something.
		Com_DPrintf( "VoIP: misordered sequence! %d < %d!\n",
		            sequence, clc.voipIncomingSequence[sender] );
		// reset the decoder just in case.
		opus_decoder_ctl( clc.opusDecoder[sender], OPUS_RESET_STATE );
		seqdiff = 0;
	} else if ( seqdiff * VOIP_MAX_PACKET_SAMPLES * 2 >= (int) sizeof( decoded ) ) { // dropped more than we can handle?
		// just start over.
		Com_DPrintf( "VoIP: Dropped way too many (%d) frames from client #%d\n",
		            seqdiff, sender );
		opus_decoder_ctl( clc.opusDecoder[sender], OPUS_RESET_STATE );
		seqdiff = 0;
	}

	if ( seqdiff != 0 ) {
		Com_DPrintf( "VoIP: Dropped %d frames from client #%d\n",
		            seqdiff, sender );
		// tell opus that we're missing frames...
		for ( i = 0; i < seqdiff; i++ ) {
			if ( (written + VOIP_MAX_PACKET_SAMPLES) * 2 >= (int) sizeof( decoded ) )
				break;
			numSamples = opus_decode( clc.opusDecoder[sender], NULL, 0, decoded + written, VOIP_MAX_PACKET_SAMPLES, 0 );
			if ( numSamples <= 0 ) {
				Com_DPrintf( "VoIP: Error decoding frame %d from client #%d\n", i, sender );
				continue;
			}
			written += numSamples;
		}
	}

	numSamples = opus_decode( clc.opusDecoder[sender], encoded, packetsize, decoded + written, ARRAY_LEN( decoded ) - written, 0 );

	if ( numSamples <= 0 ) {
		Com_DPrintf( "VoIP: Error decoding voip data from client #%d\n", sender );
		numSamples = 0;
	}

	written += numSamples;

	Com_DPrintf( "VoIP: playback %d bytes, %d samples, %d frames\n",
	            written * 2, written, frames );

	if ( written > 0 )
		CL_PlayVoip( sender, written, (const byte *) decoded, flags );

	clc.voipIncomingSequence[sender] = sequence + frames;
}


/*
===============
CL_UpdateVoipIgnore

Helper for CL_Voip_f to add/remove ignore for a player
===============
*/
static void CL_UpdateVoipIgnore( const char *idstr, qboolean ignore )
{
	if ( (*idstr >= '0') && (*idstr <= '9') ) {
		const int id = atoi( idstr );
		if ( (id >= 0) && (id < MAX_CLIENTS) ) {
			clc.voipIgnore[id] = ignore;
			CL_AddReliableCommand( va( "voip %s %d",
			                         ignore ? "ignore" : "unignore", id ), qfalse );
			Com_Printf( "VoIP: %s ignoring player #%d\n",
			            ignore ? "Now" : "No longer", id );
			return;
		}
	}
	Com_Printf( "VoIP: invalid player ID#\n" );
}


/*
===============
CL_UpdateVoipGain

Helper for CL_Voip_f to set gain for a player
===============
*/
static void CL_UpdateVoipGain( const char *idstr, float gain )
{
	if ( (*idstr >= '0') && (*idstr <= '9') ) {
		const int id = atoi( idstr );
		if ( gain < 0.0f )
			gain = 0.0f;
		if ( (id >= 0) && (id < MAX_CLIENTS) ) {
			clc.voipGain[id] = gain;
			Com_Printf( "VoIP: player #%d gain now set to %f\n", id, gain );
		}
	}
}


/*
===============
CL_Voip_f

"voip" console command handler
===============
*/
void CL_Voip_f( void )
{
	const char *cmd = Cmd_Argv( 1 );
	const char *reason = NULL;

	if ( cls.state != CA_ACTIVE )
		reason = "Not connected to a server";
	else if ( !clc.voipCodecInitialized )
		reason = "Voip codec not initialized";
	else if ( !clc.voipEnabled )
		reason = "Server doesn't support VoIP";
	else if ( !clc.demoplaying && (Cvar_VariableValue( "g_gametype" ) == GT_SINGLE_PLAYER
	          || Cvar_VariableValue( "ui_singlePlayerActive" )) )
		reason = "running in single-player mode";

	if ( reason != NULL ) {
		Com_Printf( "VoIP: command ignored: %s\n", reason );
		return;
	}

	if ( strcmp( cmd, "ignore" ) == 0 ) {
		CL_UpdateVoipIgnore( Cmd_Argv( 2 ), qtrue );
	} else if ( strcmp( cmd, "unignore" ) == 0 ) {
		CL_UpdateVoipIgnore( Cmd_Argv( 2 ), qfalse );
	} else if ( strcmp( cmd, "gain" ) == 0 ) {
		if ( Cmd_Argc() > 3 ) {
			CL_UpdateVoipGain( Cmd_Argv( 2 ), atof( Cmd_Argv( 3 ) ) );
		} else if ( Q_isanumber( Cmd_Argv( 2 ) ) ) {
			int id = atoi( Cmd_Argv( 2 ) );
			if ( id >= 0 && id < MAX_CLIENTS ) {
				Com_Printf( "VoIP: current gain for player #%d "
					"is %f\n", id, clc.voipGain[id] );
			} else {
				Com_Printf( "VoIP: invalid player ID#\n" );
			}
		} else {
			Com_Printf( "usage: voip gain <playerID#> [value]\n" );
		}
	} else if ( strcmp( cmd, "muteall" ) == 0 ) {
		Com_Printf( "VoIP: muting incoming voice\n" );
		CL_AddReliableCommand( "voip muteall", qfalse );
		clc.voipMuteAll = qtrue;
	} else if ( strcmp( cmd, "unmuteall" ) == 0 ) {
		Com_Printf( "VoIP: unmuting incoming voice\n" );
		CL_AddReliableCommand( "voip unmuteall", qfalse );
		clc.voipMuteAll = qfalse;
	} else {
		Com_Printf( "usage: voip [un]ignore <playerID#>\n"
		           "       voip [un]muteall\n"
		           "       voip gain <playerID#> [value]\n" );
	}
}

#endif // USE_VOIP
