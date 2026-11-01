# lib/crypto

Cryptography for compiled Rae programs, on every target (macOS, Linux, iOS,
WASM), over the vendored Monocypher 4.0.2
(`compiler/runtime/monocypher.VENDOR.md`) and the operating system's secure
random source. Every function takes and returns `List(UInt8)` values; a
wrong-sized key or nonce gives an empty result (or `none` / `false`), never a
crash.

| module | functions | sizes |
|---|---|---|
| `crypto/CryptoRandom` | `cryptoRandomBytes(count)` | any count (asked for 256 at a time) |
| `crypto/Blake2b` | `blake2b(message, hashSize)`, `blake2bKeyed(message, key, hashSize)` | digest 1..64, key 0..64 |
| `crypto/Ed25519` | `ed25519KeyPairFromSeed(seed)`, `ed25519NewKeyPair()`, `ed25519Sign(secretKey, message)`, `ed25519Verify(signature, publicKey, message)` | seed 32, secret key 64, public key 32, signature 64 |
| `crypto/X25519` | `x25519NewSecretKey()`, `x25519PublicKey(secretKey)`, `x25519SharedSecret(secretKey, theirPublicKey)` | 32 each |
| `crypto/Argon2` | `argon2Hash(password, salt, settings, hashSize, key, associatedData)`, `argon2DefaultSettings()` | salt at least 8 (16 random is usual) |
| `crypto/Aead` | `aeadLock(key, nonce, plainText, associatedData) -> AeadSealed`, `aeadUnlock(key, nonce, sealed, associatedData) -> opt List(UInt8)` | key 32, nonce 24, mac 16 |
| `crypto/CryptoBytes` | `hexOfBytes`, `bytesOfHex`, `bytesOfText`, `bytesEqual` (constant time), `copyBytes`, `zeroBytes` | |

- **Ed25519 is the standard one** (RFC 8032, SHA-512, Monocypher's optional
  `monocypher-ed25519`), so another implementation verifies its signatures.
  Monocypher's own `crypto_eddsa_*` (BLAKE2b) is not exposed.
- **X25519's shared secret is raw**: hash it with both public keys before
  using it as a key, e.g. `blake2b(shared ++ ourPublic ++ theirPublic, 32)`.
- **Aead is XChaCha20-Poly1305**: a random 24-byte nonce per message
  (`cryptoRandomBytes(count: 24)`) is safe; never lock two messages with one
  key and one nonce.
- **Argon2**: store the salt and the settings with the hash. RFC 9106's
  second recommendation, `argon2DefaultSettings()`, is Argon2id with 64 MiB,
  3 passes and 4 lanes (Monocypher computes the lanes one after the other).
- **Secure random**: `arc4random_buf` on Apple, `getrandom` on Linux,
  `getentropy` (Emscripten's `crypto.getRandomValues`) in the browser. Never
  `lib/Random`, which is a seeded game sequence.

The C side (`compiler/runtime/runtime_crypto_monocypher.c`) is one library
call per function; the checks and choices are the Rae modules'. Case 1287
holds the published vectors: RFC 7693 and the BLAKE2 keyed KAT, RFC 8032
§7.1, RFC 7748 §6.1, RFC 9106 §5, draft-irtf-cfrg-xchacha-03 A.3.1. The older
raw-pointer `Sys.encrypt` / `decrypt` / `rae_crypto_argon2i` are the same
Monocypher calls (XChaCha20-Poly1305 with no associated data, Argon2i).
