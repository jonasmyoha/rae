# Cross-platform image decoding for gpu2d (#217)

Research + decision record, 2026-07-08. Updated after implementation:
stb JPEG is present, but macOS keeps ImageIO as the default because valid
Spotify baseline JPEGs were observed failing in stb with "can't merge dc and ac".

## 1. Status quo

`rae_ext_gpu2d_loadImage` (compiler/runtime/rae_runtime.c) decodes a
file to RGBA8 and uploads a WebGPU texture. Today's dispatch:

| Format | Decoder | Platforms | Notes |
|---|---|---|---|
| PNG | lodepng (vendored, runtime/lodepng.{c,h}) | all | Also the encoder + the DEFLATE/zlib oracle for the pure-Rae codec (#110). |
| JPEG | ImageIO (`CGImageSource`) | **macOS only** (`#ifdef __APPLE__`) | Magic-byte sniffed (`FF D8 FF`). Premultiplied-alpha bitmap context (moot for JPEG — no alpha channel). 16384² dimension cap. |
| anything else | raylib `LoadImage` | only when `RAE_HAS_RAYLIB` | Incidental fallback; drags raylib's bundled stb_image in through the back door and produces decoder-dependent pixels. |
| WebP | — | none | Unsupported. |

The cross-platform gap is exactly JPEG: on Linux/Windows (the next
SDL3+WebGPU targets per docs/graphics research and the GPU
architecture decision) every album cover and every Spotify CDN
artwork fetch — all JPEG — fails to the raylib fallback or to
nothing.

Inputs in practice:
- **local assets**: `examples/*/assets/**` covers (`cover.jpg`), UI
  PNGs — trusted, author-controlled;
- **network**: Spotify CDN artwork cached under `app_cache/album_art/
  *.jpg` — well-formed in practice but *semi-untrusted*: treat the
  decoder as a security boundary.

## 2. Requirements

1. JPEG (baseline + progressive) and PNG decode on macOS, Linux,
   Windows — the SDL3/WebGPU platform set. Mobile later.
2. Deterministic pixels across platforms. The test/verification
   workflow diffs headless screenshots; two platforms decoding the
   same JPEG through different system codecs produce off-by-one
   pixels and break byte-comparisons. One decoder everywhere beats
   per-platform "best" codecs.
3. Project ethos: small vendored C with a `VENDOR.md` (the lodepng
   precedent), no new build systems, no dynamic system deps; thin
   seam so decoders can be swapped (long-term direction is pure-Rae
   codecs — the #110 PNG codec is the proof of shape).
4. Decode cost is not hot: ~24 covers once at boot plus occasional
   CDN fetches. Correctness, size, and licence outrank throughput.

## 3. Candidates evaluated

| Option | Formats | Licence | Integration | Verdict |
|---|---|---|---|---|
| **stb_image.h** (v2.30) | JPEG (baseline+progressive), PNG, BMP, GIF, … each `STBI_ONLY_*`-selectable | public-domain / MIT dual | one vendored header compiled into rae_runtime.c | **Chosen for non-macOS JPEG, but not yet trusted as the macOS default.** Smallest possible integration; deterministic everywhere once its Spotify JPEG failure is resolved. Known CVE history on malformed inputs → mitigate with the existing dimension cap, `STBI_ONLY_JPEG` (shrinks attack surface to one parser), and no-stdio (we hand it bytes we read ourselves). |
| libjpeg-turbo | JPEG | BSD-3 + IJG + zlib mix | CMake + SIMD asm; system package or submodule | Rejected: 2–6× faster decode we don't need, at the cost of a real build dependency on three platforms. Revisit only if decode ever becomes hot (e.g. hundreds of covers per second). |
| libwebp | WebP | BSD-3 (Google) | CMake/autotools; sizeable | Deferred with WebP itself (§5). |
| Wuffs | PNG, JPEG, GIF, … | Apache-2.0 | single-file C amalgamation; memory-safe by construction | Strong safety story and a plausible future swap-in behind the same seam, but larger, less battle-tested in this exact stack, and its transpiled style is harder to patch. Not now; noted as the fallback if stb's CVE cadence becomes a problem. |
| Platform APIs (ImageIO / Windows WIC / gdk-pixbuf, NDK) | varies | OS-provided | per-platform glue ×3+ | Rejected as the primary path: three different codebases, three different pixel outputs (kills requirement 2), and Linux has no universal one. ImageIO keeps a temporary role during migration only (§6). |
| Pure-Rae decoders | PNG done (#110); JPEG feasible | ours | none | The end state the language wants, but a Rae JPEG decoder is a multi-week project (DCT, Huffman, chroma upsampling) and #110's codec is compiled-target-only today. The seam keeps this open; not the answer for "run 106 on Linux next month". |

## 4. Decision

1. **Vendor `stb_image.h`** next to lodepng (`runtime/stb_image.h` +
   `stb_image.VENDOR.md`), compiled with:
   - `STBI_ONLY_JPEG` — one format, one parser, smallest surface;
   - `STBI_NO_STDIO` — the runtime reads the bytes, stb only parses;
   - `STBI_MAX_DIMENSIONS 16384` — matches the existing ImageIO cap.
2. **PNG stays on lodepng** (decoder, encoder, and pure-Rae oracle
   roles are already load-bearing; no reason to churn).
3. **Dispatch by magic bytes, one decoder per format, no cascade**:
   sniff → `FF D8 FF` = ImageIO on macOS and stb JPEG elsewhere,
   `89 50 4E 47` = lodepng, anything else = unsupported. The raylib `LoadImage` fallback is REMOVED —
   a decode either succeeds through the designated decoder or fails
   loudly; silent decoder-roulette is how platform-dependent pixels
   sneak in.
4. **Supported format set: JPEG + PNG.** That is 100 % of current
   assets and the Spotify pipeline.

## 5. WebP (and everything else): out of scope, with a trigger

No current asset or data source produces WebP. Adding a decoder for
a format nothing feeds is pure attack surface. Trigger to revisit:
the first real source of WebP/AVIF (e.g. a CDN that stops offering
JPEG). At that point the decision defaults to vendoring libwebp
(BSD-3) behind the same sniff-dispatch seam, with Wuffs as the
alternative if we want its safety properties.

## 6. Fallback & error policy

- Decode failure (unsupported magic, parse error, dimension cap,
  alloc failure) → one stderr line, existing format:
  `[gpu2d] image decode failed (<path>): <reason>` → return texture
  handle `0`. Callers already treat 0 as "no texture" and the UI
  shows its placeholder tile; no new error channel needed.
- No second-chance decoding inside one platform's chosen decoder. A JPEG
  that the selected JPEG decoder rejects is a failed image.
- Transition plan for determinism: stb JPEG is compiled in and can be tested
  on macOS with `RAE_G2D_STB_JPEG=1`, but ImageIO remains the default on
  macOS until the observed Spotify JPEG failure is understood or the decoder
  choice changes. Screenshot baselines should not move to stb while it rejects
  valid cached artwork.
- Colour/alpha semantics: RGBA8, straight (non-premultiplied) alpha,
  no colour management (ICC profiles ignored) — which is what both
  lodepng and stb produce natively, and what the JPEG path
  effectively produced before (premultiplication is identity without
  alpha). Documented as the contract of `gpu2d.loadImage`.

## 7. Deferred implementation sketch (for the follow-up task)

1. Vendor stb_image.h + VENDOR.md; defines as in §4.
2. Rewrite `rae_ext_gpu2d_loadImage`'s decode half as
   `rae_g2d_decode_rgba(path)` with the sniff-dispatch; delete the
   raylib branch; keep ImageIO as the macOS default and gate stb testing
   behind `RAE_G2D_STB_JPEG=1`.
3. Also route `rae_ext_image_loadPng`'s sibling (if a
   `image.loadJpeg` Rae API is wanted) through the same helper —
   optional, not required for gpu2d parity.
4. Verify: 106 headless screenshots on macOS with stb vs ImageIO
   (expect near-identical; regenerate baselines), suite green, and a
   corrupt-file test asserting the log line + placeholder behaviour.

End of design.
