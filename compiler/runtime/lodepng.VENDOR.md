# Vendored: lodepng

PNG encoder/decoder used as the current backend of the **Rae Image API**
(`lib/image.rae`). See `docs/png-and-deflate-strategy.md` for why it is here and
the long-term plan to replace it with a Rae-owned DEFLATE codec.

- **Upstream:** https://github.com/lvandeve/lodepng
- **Version:** 20260119
- **Pinned commit:** `ed6fe5825c6a4fbb7f58ab35a4231c7543cd452a`
- **License:** zlib (permissive; see the header of `lodepng.c`).
- **Author:** Lode Vandevenne.

## Files

- `lodepng.h` / `lodepng.c` — copied verbatim from upstream (`lodepng.cpp`
  renamed to `lodepng.c`; the file is written to compile as both C and C++, and
  we compile it as C11). **Do not edit these** — re-vendor from the pinned commit
  to update, so provenance stays clean.

## How it is consumed

`rae_runtime.c` does `#include "lodepng.c"`, so lodepng is compiled into that one
translation unit. Every build path already compiles `rae_runtime.c`, so no build
flag or link step changes are needed; `copy_runtime_assets()` in the compiler
copies `lodepng.{c,h}` alongside `rae_runtime.c` for emitted standalone projects.

This is the lightest point on the dependency-consumption spectrum
(`docs/tech-stack-and-dependencies.md`): a single-file C library, no foreign
toolchain, no extra link dependency.

## Re-vendoring

```sh
COMMIT=<new-commit-sha>
curl -sSL -o lodepng.c "https://raw.githubusercontent.com/lvandeve/lodepng/$COMMIT/lodepng.cpp"
curl -sSL -o lodepng.h "https://raw.githubusercontent.com/lvandeve/lodepng/$COMMIT/lodepng.h"
# then update the commit/version above and run the examples suite.
```
