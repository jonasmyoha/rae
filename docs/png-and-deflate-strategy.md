# PNG & DEFLATE strategy

Status: **decision record / living reference**, 2026-06-26. Companion to
`tech-stack-and-dependencies.md` (dependency policy, preference ordering, the
"small and controllable?" litmus, and the Rae-owned-seam principle).

This records how Rae handles PNG (and, underneath it, DEFLATE) today and where we
intend to take it. It follows directly from the dependency philosophy: own the
small/controllable, buy the genuinely hard, keep all foreign code behind a
Rae-owned seam.

---

## The reframe: PNG is two problems, not one

1. **The container** — signature, `IHDR`/`IDAT`/`IEND` chunks, CRC-32, the five
   row filters. A *trivially* bounded spec; passes the litmus easily.
2. **DEFLATE/zlib compression** — the only real work. It is a pure, bounded
   algorithm (RFC 1951, frozen since 1996): no driver, no hardware, no Unicode
   minefield, so it *also* passes the litmus — and it is **reused far beyond
   PNG**: `.raepack` compression, gzip, asset packing, possibly network framing.

So "our PNG strategy" is really "where does our DEFLATE come from," with a thin
container on top. That reframe is why **libpng is the wrong default** (see below).

---

## Decisions

### 1. Today: lodepng behind the Rae Image API

- **Backend:** lodepng (C, zlib-licensed, single file, no zlib dependency, does
  both encode and decode). Vendored at `compiler/runtime/lodepng.{c,h}` — see
  `runtime/lodepng.VENDOR.md`. It sits at the top of the consumption spectrum: a
  single-file C library, no foreign toolchain, no extra link step.
- **Seam:** `lib/image.rae` exposes the **Rae Image API**; application code
  targets it and never touches lodepng. Today: `imageSavePng(path, pixels,
  width, height)` encoding a packed-0xRRGGBB framebuffer. Decode + load land when
  the texture pipeline needs them.
- **Implementation note:** `rae_runtime.c` does `#include "lodepng.c"`, so it
  compiles into that one translation unit — every build path already compiles
  `rae_runtime.c`, so no build-flag or link plumbing changes.

### 2. Web: do not ship a PNG library

Behind the same seam, the web target routes to the browser's native decode
(`createImageBitmap`/canvas), mirroring the "the browser provides WebGPU"
pattern. A PNG decoder compiled into WASM would be wasted bytes.

### 3. Long-term: own DEFLATE (and the PNG container) in Rae

This is the high-value dogfooding move the dependency philosophy explicitly
wants — "each piece we pull in-house hardens Rae's stdlib" — *and* DEFLATE is the
shared dependency for `.raepack`/gzip, so the payoff compounds. **Gated**, not an
early requirement. lodepng stays as the **reference oracle** for correctness and
speed comparison (round-trip + interop tests) even after the Rae path lands, and
is dropped only once the Rae path passes.

Why this is a *comfortable* thing to own (and DEFLATE specifically, not PNG-the-
format): the spec is frozen, it is pure compute, and it is **golden-file
testable** — `deflate` → must `inflate` back byte-identical, and must interop
both directions with zlib/the browser. Correctness is mechanically verifiable.

---

## Why not the alternatives

- **libpng** — *not* the default. Heavier, **requires zlib**, awkward
  `setjmp/longjmp` API, and full generality (interlacing, every bit depth,
  palettes, gamma) we don't need for assets we control. Wins only when robustly
  decoding *exotic/hostile* PNGs — and even then a modern decoder beats it.
- **stb_image + stb_image_write** — great zero-friction default *except* two
  things that matter here: `stb_image_write`'s compression is weak (fat files),
  and `stb_image` has a CVE history (poor for **untrusted** input). Fine for a
  controlled pipeline; lodepng is the better all-rounder (real compression, one
  file, both directions).
- **libspng** — excellent *decoder* (fast, SIMD, sane API) but needs zlib/miniz
  and is decode-focused. A candidate if decode robustness/speed on untrusted
  input ever becomes the priority; overkill as the general default.

---

## Effort & line-count estimate (the Rae rewrite)

| Piece | Difficulty | Rough Rae LOC |
|---|---|---|
| Bit reader/writer, CRC-32, Adler-32 | easy | ~150–250 |
| **Inflate** (decode: stored/fixed/dynamic Huffman, 32 KB window) | moderate | ~400–600 |
| **Deflate** (encode: LZ77 hash-chain match-finder + Huffman trees) | the hard part | ~600–900 |
| PNG container (chunks + the 5 row filters, both directions) | easy–moderate | ~300–500 |

End-to-end ≈ **1,500–2,500 lines of Rae**, split into ~5–6 modules to respect the
<1000 LOC/file rule (`bitio`, `huffman`, `inflate`, `deflate`, `checksums`,
`png`). The encoder's **match-finder is the only genuinely fiddly part**; build
it test-first against zlib vectors. A **fixed-Huffman-only deflate is trivially
correct** and already beats a stored-block writer, so a correct-but-modest
encoder can ship early and improve later behind the same API.

**Maintenance:** lower than almost anything else in the tree — the logic never
changes, only Rae does, and the round-trip + zlib-interop suite pins correctness
so language churn surfaces as a test failure pointing exactly at the break. The
one ongoing cost is **performance tracking** (codegen shifts can swing
throughput while correctness stays pinned), so keep a perf check in the suite.

---

## Staging

1. **Done:** lodepng vendored + Rae Image API (`imageSavePng`); raytracer F2 save
   routes through it; the old hand-written stored-deflate C writer is removed.
2. **Next (image input):** `imageLoadPng` decode in the Image API (lodepng), for
   the texture/atlas pipeline. Web routes to browser-native decode.
3. **Mid (gated dogfooding):** inflate in Rae first (simpler half; decode is what
   texture loading needs), validated against lodepng output. Then deflate
   (fixed-Huffman first, match-finder later). lodepng becomes the test oracle.
4. **Later:** share the Rae DEFLATE codec with `.raepack`/gzip; drop lodepng once
   the Rae path passes correctness + acceptable-speed gates.
