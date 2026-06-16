# Color management plan

This document sets the long-term direction for color handling in the Rae
UI/graphics stack. The driving constraint is that the same stack must
serve two audiences:

- **Day-to-day apps** (Royal Blush, mobile UI demo, future general-
  purpose apps) — needs *correct* sRGB / Display-P3 output on macOS
  and iOS today, with no perceptible difference from the reference
  versions of the same art viewed in a properly color-managed app
  (Chrome, Preview, Safari).
- **A color-grading / video-editing application** the project owner
  intends to build on top of the same stack — needs 10-16 bit input
  support, HDR (extended dynamic range) output, ACES-class linear-
  light working space, 1D + 3D LUTs, OCIO integration, real-time
  4K+ performance.

Both audiences are served by the same underlying pipeline; the
grading app is just the linear-light end of the same machine that
draws normal UI.

## Principles

1. **Linear-light working space, always.** All blending, all shaders,
   all color math (LUTs, matrices, grades) happens in linear-light
   radiometric values. sRGB / Display P3 / Rec.709 / log-encoded
   camera footage are *encodings*; they get decoded to linear on
   input and re-encoded on output.

2. **Every color carries a colorspace tag.** A `Color` isn't just
   RGBA — it's RGBA + a colorspace identity. A PNG is sRGB. A P3
   photo is Display P3. An ARRI LogC clip is LogC AWG3. The
   internal framebuffer is linear. Conversions are explicit and
   correct.

3. **Internal pixel format is half-float.** The working framebuffer
   is `RGBA16F`. 16 bits per channel covers 10-bit display, HDR
   headroom, log→linear conversions, and chained corrections with
   no banding. 8-bit is fine for sRGB UI assets *as inputs*, and
   for final present buffers on non-HDR displays — but never
   internally.

4. **One explicit display-transform pass at the end.** Whatever the
   working space, the final pass converts linear-light → the
   display's expected color space + EOTF. On Apple platforms we
   delegate this to the OS via `CAMetalLayer.colorspace`, which
   is the cleanest, fastest, most-correct path.

5. **Don't reinvent the color-management library.** Use OCIO
   (OpenColorImageIO) for grading-grade transforms — every
   serious tool uses it, including DaVinci Resolve, Nuke, Blender.

## Architecture (target)

Layered bottom-up:

```
┌──────────────────────────────────────┐
│  app code (royalblush, mobile_ui)    │
├──────────────────────────────────────┤
│  lib/ui — scene graph, ECS, layout,  │
│           render API (existing)      │
├──────────────────────────────────────┤
│  lib/gfx — render backend trait      │
│  + Metal impl  + GL/raylib impl      │
├──────────────────────────────────────┤
│  lib/color — colorspace types,       │
│              transforms, LUTs, OCIO  │
└──────────────────────────────────────┘
```

### `lib/color` — new module

- `Colorspace` enum: `sRGB`, `DisplayP3`, `LinearSRGB`,
  `LinearRec709`, `Rec709`, `Rec2020`, `ACEScg`, `ACES2065_1`,
  plus camera-log spaces (LogC AWG3, S-Log3 SGamut3, RedLogFilm
  RWG, …) as needed.
- `Color8 = (r,g,b,a: U8, space: Colorspace)` — for storage / API
  compatibility with current `Color`.
- `ColorF = (r,g,b,a: F32, space: Colorspace)` — for math.
- `colorspaceConvert(from, to, value) ret ColorF` — matrix multiply
  + EOTF / OETF lookup. Bradford chromatic adaptation. Hardcoded
  matrices for the standard spaces.
- `ColorTransform` trait covering 1D LUT, 3D LUT, matrix, EOTF,
  OETF, identity. Composable.
- `loadCubeLut(path)`, `loadSpi3dLut(path)` — .cube and .spi3d
  loaders for grading LUTs.
- OCIO interop (later phase): vendor OpenColorImageIO as a C
  library, bind through `lib/color/ocio.rae`. Loads `config.ocio`,
  resolves named colorspaces, builds transforms.

### `lib/gfx` — backend trait

Today every layer of the rae stack calls raylib directly. The
refactor introduces a thin abstraction:

```rae
type Backend {
  loadTexture: func(path: String, space: Colorspace) ret TextureHandle
  beginFrame: func(viewport: Rect, displaySpace: Colorspace)
  drawTexturedQuad: func(handle, dst: Rect, src: Rect, tint: ColorF)
  drawText: func(...)
  endFrame: func()
}
```

Two concrete implementations:

- **`lib/gfx/metal.rae`** — for macOS + iOS. Uses `CAMetalLayer`
  directly. `MTLPixelFormatRGBA16Float` working buffer.
  `colorspace = kCGColorSpaceExtendedLinearDisplayP3` (or sRGB on
  non-wide-gamut hosts). `wantsExtendedDynamicRange = true` when
  the display supports HDR (queried via `NSScreen`'s
  `maximumExtendedDynamicRangeColorComponentValue`). The OS
  handles the final EOTF + display-profile conversion correctly,
  for free.
- **`lib/gfx/gl.rae`** — wraps raylib for non-Apple platforms.
  Limited to sRGB output via the `NSWindow.colorSpace` trick on
  macOS until the Metal backend is ready. Keeps existing apps
  shipping during the transition.

`lib/ui/render.rae` stops calling raylib directly and dispatches
through `Backend`. Existing scenes need no changes — `tint: Color`
becomes `tint: ColorF` at the API boundary (auto-converted from
8-bit `Color8` for backwards compat).

## Why Metal specifically on Apple

For the grading goal this is the right backend:

- `CAMetalLayer.pixelFormat = .rgba16Float` — the working buffer.
- `CAMetalLayer.colorspace` is a `CGColorSpace`. Setting it to
  `kCGColorSpaceExtendedLinearDisplayP3` tells macOS *the buffer
  contains extended-linear P3*, and the WindowServer applies the
  correct EOTF + display-profile conversion to whatever physical
  display the window is on. This is the *only* path that handles
  multi-display setups (sRGB monitor + P3 laptop + HDR external)
  correctly without per-display work in the app.
- `CAMetalLayer.wantsExtendedDynamicRange = true` on HDR-capable
  displays. Values > 1.0 in the shader → real HDR output. The
  RGBA16F framebuffer is HDR-ready as-is.
- 10-bit non-HDR path: `pixelFormat = .bgra10_xr` and a P3
  colorspace. Or stay on RGBA16F — that works too, slightly more
  bandwidth.
- iOS uses the same APIs. `wantsExtendedDynamicRange` matters on
  iPad Pro M1+ XDR displays.

Metal is also the only way to ship to iOS at all (OpenGL ES has
been deprecated for years). Building this once gets both macOS
and iOS.

## Performance for grading

- **3D LUT lookup**: `texture3D(samplerLUT, color.rgb)` is one tex
  fetch per pixel. A 33³ float-16 LUT is ~140 KB. Trilinear is
  free on the GPU. Trivially real-time at 4K60.
- **Per-pixel matrix transforms**: 9 multiplies + 6 adds. Negligible.
- **EOTF / OETF**: a 1D LUT (1024-4096 samples is overkill for
  visual, right for math precision) → one `texture1D` fetch. Faster
  than `pow(x, 2.4)` in the shader.
- **Internal float framebuffer**: RGBA16F at 4K is ~64 MB. Not a
  problem on any current Apple silicon. RGBA32F if a specific op
  genuinely needs it; 16F's ~10-bit precision per stop across 30
  stops is essentially perceptually lossless.
- **Compute shaders > fragment shaders** for heavy ops (debayer,
  optical flow, motion estimation). Metal compute is excellent.
- The genuinely expensive thing is **source clip decode** (ProRes,
  RED RAW, BRAW). That's a video decoder, not a color pipeline,
  and lives in a separate subsystem.

## Phased path

### Phase 0 — sRGB workaround (now, ~30 min)

Set the `NSWindow.colorSpace` to sRGB on macOS in the raylib boot
path so 8-bit sRGB content renders correctly on Display P3 macs.
This fixes the immediate oversaturation in Royal Blush and (less
visibly) the mobile UI demo. Doesn't help wide-gamut or HDR yet —
that needs the Metal backend — but unblocks visual iteration on
existing apps today.

Implementation: a small Objective-C block invoked from
`rae_install_crash_handler`-style constructor (or from a new
`rae_ext_initWindow` wrapper). Walks `NSApp.windows`, sets each
window's `colorSpace` property to `NSColorSpace.sRGBColorSpace`.

### Phase 1 — `lib/color/` types and transforms (~a few days)

Author `lib/color/colorspace.rae` with the Colorspace enum, the
ColorF / Color8 types, and the canonical conversions (matrices +
EOTFs for the seven core spaces). No backend changes yet — this
is API-surface work. Every texture loader in `lib/ui/textures.rae`
learns to carry a `Colorspace`. Default to sRGB on load. Add
`Color8` / `ColorF` split. Backend (raylib) still does naive sRGB
output, but the type system is now color-aware so later phases
don't require touching app code.

### Phase 2 — Metal backend (~2-4 weeks)

Build `lib/gfx/` abstraction. Refactor `lib/ui/render.rae` to
dispatch through the backend. Implement Metal backend for
macOS/iOS:

- `CAMetalLayer` setup
- RGBA16F working buffer
- Per-texture sRGB → linear upload conversion
- Per-frame `BeginFrame` clears the float target
- Sprite + text + shape draw kernels in Metal Shading Language
- `endFrame` presents through `CAMetalLayer` with appropriate
  `colorspace`

Existing apps re-target by replacing `import raylib` with `import
gfx` or similar. The raylib backend stays for Linux/Windows.

### Phase 3 — Grading app proper

On top of Phase 2's infrastructure:

- OCIO integration (vendored as a C dependency, bound through
  `lib/color/ocio.rae`)
- LUT pipeline (load + apply 1D and 3D LUTs)
- ACES configuration support
- Grading nodes (lift / gamma / gain, log offset, curves, vector
  keys) as a compute-shader graph
- Reference monitor output (Blackmagic DeckLink later, when needed)
- Scope tools (waveform, vectorscope, parade) — also Metal compute

By this point the color pipeline is industry-standard and the same
code paths that draw a Button correctly on a Display P3 laptop
also pass a tone-mapped HDR signal to a Sony BVM.

## Key insight

**Everything from Phase 1 onward is shared between UI apps and the
grading app.** Royal Blush gets accidentally-correct color management
for free once Phase 1 lands. Mobile UI does too. The video editor
later doesn't need a separate render stack — it's just the same
machine with more transforms in the chain.

The Metal backend is real engineering work but well-trodden:
references in Apple's MetalKit samples, the WWDC "Color At The End
Of The Universe" talks, FFmpeg's color-conversion sources,
OpenColorIO documentation, Blender's color management code, and
the Filmlight / Resolve community write-ups.

## Open questions deferred to each phase

- Phase 1: float vs half storage for `ColorF` (start with f32, ship
  with f32; revisit if hot path).
- Phase 2: whether to expose Metal directly to user code or hide it
  behind the `Backend` trait. Default to hiding it.
- Phase 3: bring-your-own OCIO config vs ship a default ACES one.
  Probably the latter for the demo grading app, with override.

## Non-goals (explicitly)

- We are not going to roll our own color science library. OCIO
  exists and is correct.
- We are not going to support every camera log format day one.
  Add them as we go.
- We are not going to ship a CMS that's better than the OS one.
  The OS's color management on Apple platforms is excellent; we
  use it via `CAMetalLayer.colorspace`.
