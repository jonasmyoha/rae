# Vendored: stb_image

JPEG decoder for `gpu2d.loadImage` (#228; decision record in
`docs/image-decoding-design.md`, #217). Compiled with `STBI_ONLY_JPEG`
— PNG stays on the vendored lodepng, and no other format is enabled,
keeping the attack surface to a single parser (Spotify CDN artwork is
semi-untrusted input).

- **Upstream:** https://github.com/nothings/stb
- **Version:** v2.30
- **Pinned commit:** `013ac3beddff3dbffafd5177e7972067cd2b5083`
- **License:** public domain / MIT dual (see the header of
  `stb_image.h`).
- **Author:** Sean Barrett and contributors.

## Files

- `stb_image.h` — copied verbatim from upstream. **Do not edit** —
  re-vendor from a pinned commit to update, so provenance stays clean.

## How it is consumed

`rae_runtime.c` defines `STB_IMAGE_IMPLEMENTATION` with
`STBI_ONLY_JPEG`, `STBI_NO_STDIO` (the runtime reads file bytes
itself; stb only parses memory), and `STBI_MAX_DIMENSIONS 16384`
(matching the previous ImageIO cap), then `#include "stb_image.h"` —
same single-translation-unit pattern as lodepng.
`copy_runtime_assets()` in the compiler copies `stb_image.h` alongside
`rae_runtime.c` for emitted standalone projects.

## Re-vendoring

```sh
COMMIT=<new-commit-sha>
curl -sSL -o stb_image.h "https://raw.githubusercontent.com/nothings/stb/$COMMIT/stb_image.h"
# then update the commit/version above and run the examples suite.
```
