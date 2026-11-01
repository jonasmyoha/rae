# Vendored: Monocypher

The cryptography of compiled Rae programs (`lib/crypto`, `Sys.encrypt` /
`decrypt`): BLAKE2b, Ed25519, X25519, Argon2, XChaCha20-Poly1305.

- **Upstream:** https://monocypher.org (https://github.com/LoupVaillant/Monocypher)
- **Version:** 4.0.2 (`monocypher-4.0.2.tar.gz`, sha256
  `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`)
- **License:** BSD-2-Clause OR CC0-1.0, at your choice (each file's header).
- **Author:** Loup Vaillant and the Monocypher contributors.

## Files

- `monocypher.c` / `monocypher.h` — `src/` of the release, verbatim.
- `monocypher-ed25519.c` / `monocypher-ed25519.h` — `src/optional/` of the
  release, verbatim: SHA-512 and standard Ed25519 (RFC 8032). Monocypher's own
  `crypto_eddsa_*` signs with BLAKE2b, which no other Ed25519 verifies.

**Do not edit these**; re-vendor from a release to update.

## How it is consumed

`runtime_crypto_monocypher.c` `#include`s both `.c` files, and `rae_runtime.c`
includes that, so every compiled program (macOS, Linux, iOS, WASM) has them in
its one runtime translation unit, the way it has lodepng. An emitted project
(`--emit-c`) gets the four files copied beside `rae_runtime.c`. Never compile
them as separate translation units: the symbols would be defined twice.
