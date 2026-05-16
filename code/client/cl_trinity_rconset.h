// cl_trinity_rconset.h -- Engine-side handler for the trinity_rconset
// server command. The hub computes an encryption key from the user's
// long-lived Trinity token + a per-handshake nonce; the collector
// encrypts the local rcon password under that key; the engine
// (here) decrypts and sets the rconPassword cvar so the connected
// player can rcon the server they're admin of without ever typing
// the password.
//
// Wire format: trinity_rconset <hexblob>
// Decoded blob:
//   byte 0    : version (must be 1)
//   byte 1    : ct_len (1..64)
//   byte 2-17 : epoch_nonce
//   byte 18.. : ciphertext (ct_len bytes)
//   trailing 8 bytes : SipHash MAC over (header || ciphertext) keyed by K
//
// K = SipHash128(token_bytes, "trinity-rconset-key-v1" || epoch_nonce)
// ciphertext = plaintext XOR keystream
// keystream block i = SipHash128(K, "trinity-rconset-ks-v1" || nonce || u32be(i))
// MAC = SipHash128(K, "trinity-rconset-mac-v1" || header || ct)[:8]
//
// Mirrors internal/crypto/rconset.go byte-for-byte. Test
// vectors live in internal/crypto/testdata/rconset_vectors.json.
#ifndef CL_TRINITY_RCONSET_H
#define CL_TRINITY_RCONSET_H

// CL_HandleTrinityRconset is called from the cl_cgame.c server-command
// dispatch when the server sends "trinity_rconset <hex>". hexArg is
// the single hex-encoded payload argument. Silently drops on any
// failure (token missing, MAC mismatch, version skew, etc.).
void CL_HandleTrinityRconset( const char *hexArg );

// CL_TrinityRconsetSelfTest runs the committed first test vector
// through the decoder using a known-good token + ciphertext, and
// Com_Errors if the result doesn't match the expected plaintext.
// Called once at client init so a crypto regression refuses to boot.
void CL_TrinityRconsetSelfTest( void );

#endif
