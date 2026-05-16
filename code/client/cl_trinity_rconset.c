// cl_trinity_rconset.c -- Engine-side rconset handler. See header for
// protocol overview.

#include "client.h"
#include "cl_trinity_rconset.h"

// ---------------------------------------------------------------------------
// SipHash-2-4, 128-bit output, raw byte form. Byte-identical to the Go
// implementation in internal/crypto/siphash.go (which itself is
// byte-identical to the QVM-side BG_HashKeyed in trinity's bg_hash.c —
// the difference here is we operate on byte buffers with explicit
// lengths so payloads with embedded NULs are safe).
// ---------------------------------------------------------------------------

#define SIPROUND \
	v0 += v1; v2 += v3; \
	v1 = (v1 << 13) | (v1 >> (64 - 13)); \
	v3 = (v3 << 16) | (v3 >> (64 - 16)); \
	v1 ^= v0; v3 ^= v2; \
	v0 = (v0 << 32) | (v0 >> (64 - 32)); \
	v2 += v1; v0 += v3; \
	v1 = (v1 << 17) | (v1 >> (64 - 17)); \
	v3 = (v3 << 21) | (v3 >> (64 - 21)); \
	v1 ^= v2; v3 ^= v0; \
	v2 = (v2 << 32) | (v2 >> (64 - 32))

// DeriveKey folds an arbitrary-length byte slice into two uint64 key
// halves. Byte-identical to deriveKey() in siphash.go.
static void DeriveKey( const byte *key, int keyLen, uint64_t *k0, uint64_t *k1 ) {
	unsigned int h[4];
	int i;
	h[0] = 0x736f6d65;
	h[1] = 0x646f7261;
	h[2] = 0x6c796765;
	h[3] = 0x74656462;
	for ( i = 0; i < keyLen; i++ ) {
		h[i & 3] ^= (unsigned char)key[i];
		h[i & 3] *= 0x01000193u;
	}
	*k0 = ( (uint64_t)h[0] << 32 ) | (uint64_t)h[1];
	*k1 = ( (uint64_t)h[2] << 32 ) | (uint64_t)h[3];
}

static void SipHash128Raw( const byte *key, int keyLen, const byte *msg, int msgLen, byte out[16] ) {
	uint64_t v0, v1, v2, v3, k0, k1, m;
	int blocks, i, left;
	uint64_t hash0, hash1;

	DeriveKey( key, keyLen, &k0, &k1 );

	v0 = k0 ^ 0x736f6d6570736575ULL;
	v1 = k1 ^ 0x646f72616e646f6dULL;
	v2 = k0 ^ 0x6c7967656e657261ULL;
	v3 = k1 ^ 0x7465646279746573ULL;

	v1 ^= 0xeeULL; // 128-bit output tag

	blocks = msgLen / 8;
	for ( i = 0; i < blocks; i++ ) {
		m = (uint64_t)msg[i*8]
			| ( (uint64_t)msg[i*8+1] << 8 )
			| ( (uint64_t)msg[i*8+2] << 16 )
			| ( (uint64_t)msg[i*8+3] << 24 )
			| ( (uint64_t)msg[i*8+4] << 32 )
			| ( (uint64_t)msg[i*8+5] << 40 )
			| ( (uint64_t)msg[i*8+6] << 48 )
			| ( (uint64_t)msg[i*8+7] << 56 );
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}

	m = 0;
	left = msgLen & 7;
	{
		int j;
		for ( j = left - 1; j >= 0; j-- ) {
			m <<= 8;
			m |= (uint64_t)(unsigned char)msg[blocks*8 + j];
		}
	}
	m |= (uint64_t)( msgLen & 0xff ) << 56;
	v3 ^= m;
	SIPROUND;
	SIPROUND;
	v0 ^= m;

	v2 ^= 0xeeULL;
	SIPROUND; SIPROUND; SIPROUND; SIPROUND;
	hash0 = v0 ^ v1 ^ v2 ^ v3;

	v1 ^= 0xddULL;
	SIPROUND; SIPROUND; SIPROUND; SIPROUND;
	hash1 = v0 ^ v1 ^ v2 ^ v3;

	// Byte layout matches sipHashHex's hex string when decoded:
	//   bytes 0-3:  hash0 low-32 big-endian
	//   bytes 4-7:  hash0 high-32 big-endian
	//   bytes 8-11: hash1 low-32 big-endian
	//   bytes 12-15: hash1 high-32 big-endian
	out[0]  = (byte)( hash0 >> 24 );
	out[1]  = (byte)( hash0 >> 16 );
	out[2]  = (byte)( hash0 >> 8 );
	out[3]  = (byte)( hash0 );
	out[4]  = (byte)( hash0 >> 56 );
	out[5]  = (byte)( hash0 >> 48 );
	out[6]  = (byte)( hash0 >> 40 );
	out[7]  = (byte)( hash0 >> 32 );
	out[8]  = (byte)( hash1 >> 24 );
	out[9]  = (byte)( hash1 >> 16 );
	out[10] = (byte)( hash1 >> 8 );
	out[11] = (byte)( hash1 );
	out[12] = (byte)( hash1 >> 56 );
	out[13] = (byte)( hash1 >> 48 );
	out[14] = (byte)( hash1 >> 40 );
	out[15] = (byte)( hash1 >> 32 );
}

#undef SIPROUND

// ---------------------------------------------------------------------------
// Rconset protocol constants. Must match internal/crypto/rconset.go.
// ---------------------------------------------------------------------------

#define RCONSET_VERSION         1
#define RCONSET_NONCE_LEN       16
#define RCONSET_MAC_LEN         8
#define RCONSET_MAX_CT          64
#define RCONSET_HEADER_LEN      ( 2 + RCONSET_NONCE_LEN )
#define RCONSET_MAX_BLOB_BYTES  ( RCONSET_HEADER_LEN + RCONSET_MAX_CT + RCONSET_MAC_LEN )

static const char LABEL_KEY[] = "trinity-rconset-key-v1";
static const char LABEL_KS[]  = "trinity-rconset-ks-v1";
static const char LABEL_MAC[] = "trinity-rconset-mac-v1";

// HexDecode parses up to outCap bytes from src (NUL-terminated) into out.
// Returns the number of bytes decoded on success, -1 on bad input.
static int HexDecode( const char *src, byte *out, int outCap ) {
	int srcLen = (int)strlen( src );
	int i;
	if ( srcLen & 1 ) return -1;
	if ( srcLen / 2 > outCap ) return -1;
	for ( i = 0; i < srcLen / 2; i++ ) {
		int hi, lo;
		char a = src[i*2], b = src[i*2 + 1];
		if      ( a >= '0' && a <= '9' ) hi = a - '0';
		else if ( a >= 'a' && a <= 'f' ) hi = 10 + a - 'a';
		else if ( a >= 'A' && a <= 'F' ) hi = 10 + a - 'A';
		else return -1;
		if      ( b >= '0' && b <= '9' ) lo = b - '0';
		else if ( b >= 'a' && b <= 'f' ) lo = 10 + b - 'a';
		else if ( b >= 'A' && b <= 'F' ) lo = 10 + b - 'A';
		else return -1;
		out[i] = (byte)( ( hi << 4 ) | lo );
	}
	return srcLen / 2;
}

// CTEqual is constant-time byte comparison. Returns 0 on equal.
static int CTEqual( const byte *a, const byte *b, int n ) {
	int i, diff = 0;
	for ( i = 0; i < n; i++ ) diff |= a[i] ^ b[i];
	return diff;
}

// rconsetDecrypt is the protocol-bearing core. Takes a token (raw bytes
// + length), the decoded blob, and writes the plaintext into pt with
// at most ptCap bytes. Returns the plaintext length on success, -1 on
// any failure (bad version, bad ct_len, MAC mismatch, length mismatch).
// Plaintext is NOT validated for printability — caller decides.
static int rconsetDecrypt(
	const byte *token, int tokenLen,
	const byte *blob, int blobLen,
	byte *pt, int ptCap )
{
	byte K[16];
	byte epochNonce[RCONSET_NONCE_LEN];
	int  ctLen;
	int  i, blocks;
	byte macInput[ /* label + header + ct */ 256 ];
	int  macInputLen;
	byte macFull[16];
	byte keystream[RCONSET_MAX_CT + 16]; // 16 bytes headroom for final partial block

	if ( blobLen < RCONSET_HEADER_LEN + RCONSET_MAC_LEN ) return -1;
	if ( blob[0] != RCONSET_VERSION )                     return -1;
	ctLen = (int)blob[1];
	if ( ctLen < 1 || ctLen > RCONSET_MAX_CT )            return -1;
	if ( blobLen != RCONSET_HEADER_LEN + ctLen + RCONSET_MAC_LEN ) return -1;
	if ( ctLen > ptCap )                                  return -1;

	Com_Memcpy( epochNonce, blob + 2, RCONSET_NONCE_LEN );

	// K = SipHash128( token, LABEL_KEY || epochNonce )
	{
		byte msg[ /* label + nonce */ 64 ];
		int  msgLen = (int)strlen( LABEL_KEY );
		if ( msgLen + RCONSET_NONCE_LEN > (int)sizeof( msg ) ) return -1;
		Com_Memcpy( msg, LABEL_KEY, msgLen );
		Com_Memcpy( msg + msgLen, epochNonce, RCONSET_NONCE_LEN );
		msgLen += RCONSET_NONCE_LEN;
		SipHash128Raw( token, tokenLen, msg, msgLen, K );
	}

	// MAC verify
	{
		int labelLen = (int)strlen( LABEL_MAC );
		macInputLen = labelLen + RCONSET_HEADER_LEN + ctLen;
		if ( macInputLen > (int)sizeof( macInput ) ) return -1;
		Com_Memcpy( macInput, LABEL_MAC, labelLen );
		Com_Memcpy( macInput + labelLen, blob, RCONSET_HEADER_LEN + ctLen );
		SipHash128Raw( K, 16, macInput, macInputLen, macFull );
		if ( CTEqual( macFull, blob + RCONSET_HEADER_LEN + ctLen, RCONSET_MAC_LEN ) != 0 ) {
			return -1;
		}
	}

	// Keystream: KS_i = SipHash128( K, LABEL_KS || epochNonce || u32be(i) )
	blocks = ( ctLen + 15 ) / 16;
	for ( i = 0; i < blocks; i++ ) {
		byte msg[ /* label + nonce + counter */ 64 ];
		int  labelLen = (int)strlen( LABEL_KS );
		int  msgLen   = labelLen + RCONSET_NONCE_LEN + 4;
		if ( msgLen > (int)sizeof( msg ) ) return -1;
		Com_Memcpy( msg, LABEL_KS, labelLen );
		Com_Memcpy( msg + labelLen, epochNonce, RCONSET_NONCE_LEN );
		msg[ labelLen + RCONSET_NONCE_LEN + 0 ] = (byte)( i >> 24 );
		msg[ labelLen + RCONSET_NONCE_LEN + 1 ] = (byte)( i >> 16 );
		msg[ labelLen + RCONSET_NONCE_LEN + 2 ] = (byte)( i >> 8 );
		msg[ labelLen + RCONSET_NONCE_LEN + 3 ] = (byte)i;
		SipHash128Raw( K, 16, msg, msgLen, keystream + i * 16 );
	}
	for ( i = 0; i < ctLen; i++ ) {
		pt[i] = blob[ RCONSET_HEADER_LEN + i ] ^ keystream[i];
	}
	return ctLen;
}

// ---------------------------------------------------------------------------
// Public entry points.
// ---------------------------------------------------------------------------

void CL_HandleTrinityRconset( const char *hexArg ) {
	byte blob[RCONSET_MAX_BLOB_BYTES];
	int  blobLen;
	char token[256];
	byte pt[RCONSET_MAX_CT + 1];
	int  ptLen;
	int  i;

	// Refuse outside a real multiplayer session (#7 in the design):
	// no demos, no local games. com_sv_running is set when we're the
	// listening server; clc.demoplaying covers demo replay.
	if ( clc.demoplaying ) {
		Com_DPrintf( "trinity_rconset: demo playback, ignoring\n" );
		return;
	}
	if ( com_sv_running && com_sv_running->integer ) {
		Com_DPrintf( "trinity_rconset: local-game context, ignoring\n" );
		return;
	}

	if ( !hexArg || !hexArg[0] ) {
		Com_DPrintf( "trinity_rconset: empty argument\n" );
		return;
	}

	blobLen = HexDecode( hexArg, blob, RCONSET_MAX_BLOB_BYTES );
	if ( blobLen < 0 ) {
		Com_DPrintf( "trinity_rconset: hex decode failed\n" );
		return;
	}

	Cvar_VariableStringBuffer( "cl_trinityToken", token, sizeof( token ) );
	if ( !token[0] ) {
		Com_DPrintf( "trinity_rconset: cl_trinityToken empty, ignoring\n" );
		return;
	}

	ptLen = rconsetDecrypt( (const byte *)token, (int)strlen( token ),
	                        blob, blobLen, pt, sizeof( pt ) - 1 );
	if ( ptLen < 0 ) {
		Com_DPrintf( "trinity_rconset: decrypt/MAC failed\n" );
		return;
	}

	// Validate plaintext: printable ASCII, no space, NUL-terminate.
	for ( i = 0; i < ptLen; i++ ) {
		if ( pt[i] < 0x21 || pt[i] > 0x7E ) {
			Com_DPrintf( "trinity_rconset: non-printable byte at %d\n", i );
			return;
		}
	}
	pt[ptLen] = '\0';

	Cvar_Set( "rconPassword", (const char *)pt );
	Com_Printf( "^5Trinity: rcon access enabled for this server\n" );
}

// CL_TrinityRconsetSelfTest replays the committed first test vector to
// catch byte-layout regressions before they reach a live server.
void CL_TrinityRconsetSelfTest( void ) {
	// Vector "min_1byte_plaintext" from
	// trinity-tracker/internal/crypto/testdata/rconset_vectors.json
	static const char token[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	static const char blobHex[] = "01010000000000000000000000000000000130c8942a31b2e16d34";
	static const char expectedKey[] = "ea00d837ba1fe7d2a52b78d38f69eb23";
	static const char expectedPt[] = "x";

	byte blob[RCONSET_MAX_BLOB_BYTES];
	int  blobLen;
	byte pt[RCONSET_MAX_CT + 1];
	int  ptLen;
	byte K[16];
	char keyHex[33];
	int  i;
	static const char hexDigits[] = "0123456789abcdef";

	blobLen = HexDecode( blobHex, blob, sizeof( blob ) );
	if ( blobLen < 0 ) {
		Com_Error( ERR_FATAL, "trinity_rconset selftest: hex decode failed" );
	}

	// Spot-check K derivation in isolation.
	{
		byte msg[64];
		int  msgLen = (int)strlen( LABEL_KEY );
		Com_Memcpy( msg, LABEL_KEY, msgLen );
		Com_Memcpy( msg + msgLen, blob + 2, RCONSET_NONCE_LEN );
		msgLen += RCONSET_NONCE_LEN;
		SipHash128Raw( (const byte *)token, (int)strlen( token ), msg, msgLen, K );
	}
	for ( i = 0; i < 16; i++ ) {
		keyHex[i*2]     = hexDigits[ (K[i] >> 4) & 0xf ];
		keyHex[i*2 + 1] = hexDigits[  K[i]       & 0xf ];
	}
	keyHex[32] = '\0';
	if ( strcmp( keyHex, expectedKey ) != 0 ) {
		Com_Error( ERR_FATAL,
			"trinity_rconset selftest: K mismatch (got %s, want %s)",
			keyHex, expectedKey );
	}

	// Full decrypt round-trip.
	ptLen = rconsetDecrypt( (const byte *)token, (int)strlen( token ),
	                        blob, blobLen, pt, sizeof( pt ) - 1 );
	if ( ptLen != (int)strlen( expectedPt ) ) {
		Com_Error( ERR_FATAL,
			"trinity_rconset selftest: ptLen = %d, want %d",
			ptLen, (int)strlen( expectedPt ) );
	}
	pt[ptLen] = '\0';
	if ( strcmp( (const char *)pt, expectedPt ) != 0 ) {
		Com_Error( ERR_FATAL,
			"trinity_rconset selftest: plaintext mismatch (got %s, want %s)",
			(const char *)pt, expectedPt );
	}

	Com_DPrintf( "trinity_rconset selftest passed\n" );
}
