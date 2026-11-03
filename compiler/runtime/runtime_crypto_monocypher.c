/* lib/crypto over the vendored Monocypher 4.0.2 (monocypher.VENDOR.md) and the
 * operating system's secure random source.
 *
 * Each rae_ext_Crypto_* function makes ONE library call and holds no policy:
 * the sizes, the checks, the chunking of random bytes and every choice of
 * parameters are Rae's (the lib/crypto modules). Byte arguments are a buffer and an
 * offset, as lib/crypto/Sha1 passes them. The only C-side work besides the
 * call is filling Monocypher's argon2 parameter structs (its ABI).
 *
 * Platform reasons: Monocypher is a vendored C library used as a library
 * (AGENTS.md, the lodepng rule); the random source is an OS call
 * (arc4random_buf on Apple, getrandom on Linux, getentropy on the web, which
 * Emscripten backs with crypto.getRandomValues). Included by rae_runtime.c. */
/* Monocypher defines its own MIN and MAX (the same as the system headers') */
#undef MIN
#undef MAX
#include "monocypher.c"
/* The two files are written as separate translation units, and the runtime is
 * one: the add-on redefines the same utility macros (identically) and has its
 * own static `hash_reduce`. Undefine the macros and rename that function for
 * the add-on, so both compile unedited. */
#undef FOR
#undef COPY
#undef ZERO
#undef WIPE_CTX
#undef WIPE_BUFFER
#undef MIN
#define hash_reduce rae_monocypher_ed25519_hash_reduce
#include "monocypher-ed25519.c"
#undef hash_reduce

/* BLAKE2b of `count` bytes of `message` from `offset` into `hashSize` bytes
 * (1..64) at `hash`, keyed by the `keySize` (0..64) bytes at `key` (0: the
 * plain hash) */
void rae_ext_Crypto_blake2b(uint8_t* hash, int64_t hashSize, uint8_t* key, int64_t keySize,
                            uint8_t* message, int64_t offset, int64_t count) {
  crypto_blake2b_keyed(hash, (size_t)hashSize, key, (size_t)keySize, message + offset, (size_t)count);
}

/* An Ed25519 key pair from a 32-byte seed. Monocypher wipes the seed, so the
 * caller passes a copy. */
void rae_ext_Crypto_ed25519KeyPair(uint8_t* secretKey, uint8_t* publicKey, uint8_t* seed) {
  crypto_ed25519_key_pair(secretKey, publicKey, seed);
}

/* The 64-byte Ed25519 signature of `count` bytes of `message` from `offset` */
void rae_ext_Crypto_ed25519Sign(uint8_t* signature, uint8_t* secretKey, uint8_t* message,
                                int64_t offset, int64_t count) {
  crypto_ed25519_sign(signature, secretKey, message + offset, (size_t)count);
}

/* 0 when `signature` is `publicKey`'s signature of the message, -1 if not */
int64_t rae_ext_Crypto_ed25519Check(uint8_t* signature, uint8_t* publicKey, uint8_t* message,
                                    int64_t offset, int64_t count) {
  return crypto_ed25519_check(signature, publicKey, message + offset, (size_t)count);
}

/* The X25519 public key of a 32-byte secret key */
void rae_ext_Crypto_x25519PublicKey(uint8_t* publicKey, uint8_t* secretKey) {
  crypto_x25519_public_key(publicKey, secretKey);
}

/* The raw X25519 shared secret of our secret key and their public key */
void rae_ext_Crypto_x25519(uint8_t* sharedSecret, uint8_t* secretKey, uint8_t* theirPublicKey) {
  crypto_x25519(sharedSecret, secretKey, theirPublicKey);
}

/* Argon2 (`algorithm` 0 Argon2d, 1 Argon2i, 2 Argon2id) of the password with
 * the salt and the optional key and associated data, into `hashSize` bytes;
 * `workArea` is `blocks` KiB */
void rae_ext_Crypto_argon2(uint8_t* hash, int64_t hashSize, uint8_t* workArea, int64_t algorithm,
                           int64_t blocks, int64_t passes, int64_t lanes,
                           uint8_t* password, int64_t passwordSize, uint8_t* salt, int64_t saltSize,
                           uint8_t* key, int64_t keySize, uint8_t* associatedData,
                           int64_t associatedDataSize) {
  crypto_argon2_config config = { (uint32_t)algorithm, (uint32_t)blocks, (uint32_t)passes, (uint32_t)lanes };
  crypto_argon2_inputs inputs = { password, salt, (uint32_t)passwordSize, (uint32_t)saltSize };
  crypto_argon2_extras extras = { key, associatedData, (uint32_t)keySize, (uint32_t)associatedDataSize };
  crypto_argon2(hash, (uint32_t)hashSize, workArea, config, inputs, extras);
}

/* XChaCha20-Poly1305: encrypt `count` bytes of `plain` from `offset` into
 * `cipher` and the 16-byte `mac`, with a 32-byte key and a 24-byte nonce,
 * authenticating the `associatedDataSize` bytes at `associatedData` too */
void rae_ext_Crypto_aeadLock(uint8_t* cipher, uint8_t* mac, uint8_t* key, uint8_t* nonce,
                             uint8_t* associatedData, int64_t associatedDataSize,
                             uint8_t* plain, int64_t offset, int64_t count) {
  crypto_aead_lock(cipher, mac, key, nonce, associatedData, (size_t)associatedDataSize,
                   plain + offset, (size_t)count);
}

/* Decrypt `count` bytes of `cipher` into `plain`: 0, or -1 when the mac does
 * not match (then `plain` is left untouched) */
int64_t rae_ext_Crypto_aeadUnlock(uint8_t* plain, uint8_t* mac, uint8_t* key, uint8_t* nonce,
                                  uint8_t* associatedData, int64_t associatedDataSize,
                                  uint8_t* cipher, int64_t offset, int64_t count) {
  return crypto_aead_unlock(plain, mac, key, nonce, associatedData, (size_t)associatedDataSize,
                            cipher + offset, (size_t)count);
}

/* ChaCha20-Poly1305 as RFC 8439 has it (the IETF form, a 12-byte nonce): the
 * packets of lib/net (lib/crypto/ChaCha20Poly1305.rae, whose Rae code is the
 * reference these are checked against in compiler/tests/cases/
 * 1285_fast_crypto_matches_reference). The ciphertext and its tag are
 * contiguous, as on the wire. */
/* Seal `count` bytes of `plain` from `plainOffset` under the 32-byte key at
 * `key + keyOffset` and the 12-byte `nonce` (RFC 8439), covering `adCount`
 * bytes of `ad` from `adOffset`: the ciphertext then the 16-byte tag at
 * `out + outOffset` */
void rae_ext_Crypto_aeadSeal(uint8_t* key, int64_t keyOffset, uint8_t* nonce, uint8_t* ad,
                             int64_t adOffset, int64_t adCount, uint8_t* plain,
                             int64_t plainOffset, int64_t count, uint8_t* out, int64_t outOffset) {
  crypto_aead_ctx context;
  crypto_aead_init_ietf(&context, key + keyOffset, nonce);
  crypto_aead_write(&context, out + outOffset, out + outOffset + count,
                    adCount > 0 ? ad + adOffset : NULL, (size_t)adCount,
                    count > 0 ? plain + plainOffset : NULL, (size_t)count);
  crypto_wipe(&context, sizeof(context));
}

/* Open `count` bytes of ciphertext at `sealed + sealedOffset`, its tag right
 * after them: 0 and the plaintext at `out + outOffset`, or -1 when the tag
 * does not match (nothing written) */
int64_t rae_ext_Crypto_aeadOpen(uint8_t* key, int64_t keyOffset, uint8_t* nonce, uint8_t* ad,
                                int64_t adOffset, int64_t adCount, uint8_t* sealed,
                                int64_t sealedOffset, int64_t count, uint8_t* out,
                                int64_t outOffset) {
  crypto_aead_ctx context;
  crypto_aead_init_ietf(&context, key + keyOffset, nonce);
  int mismatch = crypto_aead_read(&context, out + outOffset, sealed + sealedOffset + count,
                                  adCount > 0 ? ad + adOffset : NULL, (size_t)adCount,
                                  count > 0 ? sealed + sealedOffset : NULL, (size_t)count);
  crypto_wipe(&context, sizeof(context));
  return mismatch ? -1 : 0;
}

/* `count` (at most 256) secure random bytes into `bytes` from `offset`: the
 * number written, or a negative error. One call to the platform's source. */
#if defined(__EMSCRIPTEN__) || defined(__wasm__)
#include <unistd.h>
int64_t rae_ext_Crypto_randomFill(uint8_t* bytes, int64_t offset, int64_t count) {
  return getentropy(bytes + offset, (size_t)count) == 0 ? count : -1;
}
#elif defined(__APPLE__)
#include <stdlib.h>
int64_t rae_ext_Crypto_randomFill(uint8_t* bytes, int64_t offset, int64_t count) {
  arc4random_buf(bytes + offset, (size_t)count);
  return count;
}
#elif defined(__linux__)
#include <sys/random.h>
#include <errno.h>
int64_t rae_ext_Crypto_randomFill(uint8_t* bytes, int64_t offset, int64_t count) {
  ssize_t got = getrandom(bytes + offset, (size_t)count, 0);
  return got < 0 ? -(int64_t)errno : (int64_t)got;
}
#else
#include <unistd.h>
int64_t rae_ext_Crypto_randomFill(uint8_t* bytes, int64_t offset, int64_t count) {
  return getentropy(bytes + offset, (size_t)count) == 0 ? count : -1;
}
#endif

/* Sys.encrypt / decrypt / argon2i (lib/Sys.rae's older raw-pointer API, kept
 * for its callers): XChaCha20-Poly1305 with no associated data, and Argon2i
 * into a work area this call allocates */
void rae_ext_rae_crypto_lock(RaeAny key, RaeAny nonce, RaeAny plain, int64_t plain_len, RaeAny mac, RaeAny cipher) {
  crypto_aead_lock((uint8_t*)cipher.as.ptr, (uint8_t*)mac.as.ptr, (const uint8_t*)key.as.ptr,
                   (const uint8_t*)nonce.as.ptr, NULL, 0, (const uint8_t*)plain.as.ptr, (size_t)plain_len);
}

int64_t rae_ext_rae_crypto_unlock(RaeAny key, RaeAny nonce, RaeAny mac, RaeAny cipher, int64_t cipher_len, RaeAny plain) {
  return crypto_aead_unlock((uint8_t*)plain.as.ptr, (const uint8_t*)mac.as.ptr, (const uint8_t*)key.as.ptr,
                            (const uint8_t*)nonce.as.ptr, NULL, 0, (const uint8_t*)cipher.as.ptr,
                            (size_t)cipher_len);
}

void rae_ext_rae_crypto_argon2i(rae_String password, rae_String salt, int64_t nb_blocks, int64_t nb_iterations, RaeAny hash_buf, int64_t hash_len) {
  /* Monocypher's precondition: at least 8 blocks per lane (one lane here) */
  if (nb_blocks < 8 || nb_iterations < 1) return;
  void* work = malloc((size_t)nb_blocks * 1024);
  if (!work) return;
  crypto_argon2_config config = { CRYPTO_ARGON2_I, (uint32_t)nb_blocks, (uint32_t)nb_iterations, 1 };
  crypto_argon2_inputs inputs = { password.data, salt.data, (uint32_t)password.len, (uint32_t)salt.len };
  crypto_argon2((uint8_t*)hash_buf.as.ptr, (uint32_t)hash_len, work, config, inputs, crypto_argon2_no_extras);
  free(work);
}
