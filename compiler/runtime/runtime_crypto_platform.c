/* Platform hashes: ONE call each into the operating system's crypto library,
 * which uses the CPU's hash instructions (ARMv8 SHA1 / x86 SHA-NI). On an M1
 * Max that hashes 16 MB with SHA-1 in 7 ms, against 100 ms for the portable
 * Rae code and 77 ms for the same algorithm in plain C, so where the
 * platform has it, it is the one used (AGENTS.md: the faster native
 * implementation wins; Rae is the portable fallback).
 *
 * Platform reason: an OS library call (CommonCrypto on Apple). The choice of
 * path is Rae's: lib/crypto/Sha1.rae declares this binding only under
 * `when hasCommonCrypto` (docs/platform-conditional-code.md), so other
 * platforms never reference it and need no stub. Included by
 * rae_runtime.c. */

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <CommonCrypto/CommonDigest.h>

/* SHA-1 of `count` bytes of `bytes` from `offset` into the 20 bytes at
 * `digest`: 1, or -1 for an input CommonCrypto cannot take (over 4 GB) */
int64_t rae_ext_Sha1_commonCrypto(uint8_t* bytes, int64_t offset, int64_t count, uint8_t* digest) {
  if (count < 0 || count > (int64_t)UINT32_MAX) return -1;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CC_SHA1(count > 0 ? bytes + offset : (const void*)"", (CC_LONG)count, digest);
#pragma clang diagnostic pop
  return 1;
}
#endif

/* Random bytes for keys (lib/crypto/SecureRandom.rae): ONE getentropy call,
 * the OS's cryptographic random source (arc4random's kernel source on Apple,
 * getrandom on Linux, crypto.getRandomValues under Emscripten). At most 256
 * bytes per call; the Rae side loops. Answers 0 or -errno.
 *
 * Platform reason: a system call. Declared in Rae under `when
 * hasGetEntropy` (Unix and the web), so nothing else references it. */
#if !defined(_WIN32)
#include <errno.h>
#include <sys/types.h>
#include <sys/random.h>
#include <unistd.h>

int64_t rae_ext_SecureRandom_getEntropy(uint8_t* buffer, int64_t offset, int64_t count) {
  if (count < 0 || count > 256) return -EINVAL;
  return getentropy(buffer + offset, (size_t)count) == 0 ? 0 : -errno;
}
#endif

/* ChaCha20-Poly1305 and X25519 through the vendored Monocypher
 * (compiler/runtime/monocypher.c, included by rae_runtime.c in every build:
 * native and the browser's). lib/crypto's Rae code does the same work and
 * is the reference checked against these (compiler/tests/cases/
 * 1285_fast_crypto_matches_reference); these are the ones used, being
 * about twice as fast for a 1200-byte packet and twenty times for X25519
 * (AGENTS.md: the faster implementation wins).
 *
 * Reason for the C: a faster established library (Monocypher, CC0). Each
 * function is one library operation over buffers Rae sized; no policy. */

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

/* X25519 of the 32-byte scalar and point into the 32 bytes at `out` */
void rae_ext_Crypto_x25519(uint8_t* out, int64_t outOffset, uint8_t* scalar, int64_t scalarOffset,
                           uint8_t* point, int64_t pointOffset) {
  crypto_x25519(out + outOffset, scalar + scalarOffset, point + pointOffset);
}
