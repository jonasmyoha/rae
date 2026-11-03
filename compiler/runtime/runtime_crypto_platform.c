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
