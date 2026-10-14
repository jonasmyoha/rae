# Forward + 4x MSAA next to the deferred renderer: a study (render-perf-14)

Question: should Rae add a Rae-first forward main pass (a depth pre-pass, one
forward pass, 4x MSAA resolved on store, memoryless attachments) next to the
deferred one? What does it cost, what does it change, and how much of the old C
forward code would it replace? This document answers with measurements. It
ships nothing; the prototype is `benchmarks/forwardMsaa/` (its README explains
how to run it and how it times frames).

**Recommendation, in short: do not build a second main pass now.** On the iPhone
15 Pro, forward + 4x MSAA is 0.62-0.94x the time of the deferred G-buffer +
lighting passes and keeps 15-50 MB less in targets. But those two passes are a
small part of a Rae frame, every effect the deferred renderer has (SSAO, TAA,
decals, water, the render ceiling) would need a second version, and the C forward
path can be retired without a new renderer. Details and numbers below.

## 1. The prototype

`benchmarks/forwardMsaa/` draws one synthetic scene over the raw WebGPU bindings:
a grid of instanced UV spheres (1,536 triangles each) over a ground, seen from a
low camera so that near spheres hide far ones, the overdraw a walker's street
has. Two weights: **heavy** (901 instances, 1.38 M triangles: geometry-bound)
and **light** (145 instances, 0.22 M triangles: pixel-bound). Six variants run
the SAME lighting function (one sun with GGX specular plus a hemisphere ambient)
and end in the same 8-bit image:

| variant | what it does | target bytes / pixel |
|---|---|---|
| deferred | G-buffer pass (rgb10a2 normal, rgba8 albedo, rgba8 material, depth32, all stored), then a fullscreen lighting pass that rebuilds the position from depth | 20 |
| forward 1x + pre-pass | one pass: every object drawn depth-only, then again with shading and depth Equal | 8 |
| forward 1x | one pass, shading with depth Less | 8 |
| forward 4x + pre-pass | the pre-pass design at 4x MSAA, resolved into the 1x output on store, samples discarded, memoryless | 4 |
| forward 4x | one 4x pass, resolved on store, memoryless | 4 |
| forward 4x stored | forward 4x with ordinary (not memoryless) 4x targets | 36 |

The deferred variant's G-buffer is the layout the Rae deferred renderer uses;
its lighting writes 8-bit here where the renderer writes rg11b10 (also 4 bytes).

Timing is the clock time of one submit of many frames (64 on the Mac, 32 on the
iPhone), from recording to the GPU finishing, divided by the frames: the
throughput, with consecutive frames overlapping as they do in a running app. The
variants take turns over 5 rounds so a GPU clock change hits all of them. GPU
timestamps were not usable: on the Mac they include the overlap with the frame
before, and on the iPhone they come back with the end before the beginning (the
same Metal timestamp problem as #528).

## 2. Measurements

Microseconds per frame (median). iPhone 15 Pro (A17 Pro):

| scene, size | deferred | fwd 1x + pre-pass | fwd 1x | fwd 4x + pre-pass | **fwd 4x** | fwd 4x stored |
|---|---|---|---|---|---|---|
| light, 1600x720 (the phone's render ceiling) | 444 | 525 | 278 | 544 | **344** | 343 |
| heavy, 1600x720 | 1456 | 2388 | 1306 | 2454 | **1364** | 1366 |
| light, 2556x1179 (native) | 923 | 617 | 517 | 655 | **574** | 576 |

Apple M1 Max:

| scene, size | deferred | fwd 1x + pre-pass | fwd 1x | fwd 4x + pre-pass | **fwd 4x** | fwd 4x stored |
|---|---|---|---|---|---|---|
| heavy, 1920x1080 | 281 | 391 | 218 | 389 | **221** | 220 |
| light, 2560x1440 | 260 | 145 | 128 | 160 | **141** | 141 |
| light, 3840x2160 | 450 | 228 | 216 | 261 | **222** | 238 |
| heavy, 3840x2160 | 556 | 448 | 331 | 534 | **376** | 376 |

Target memory, from the sizes above (the deferred renderer's real set is larger:
TAA history, AO, the depth pyramid, motion vectors, bloom):

| size | deferred (20 B/px) | fwd 4x memoryless (4 B/px) | fwd 4x stored (36 B/px) |
|---|---|---|---|
| 1600x720 | 21 MB | 4 MB | 39 MB |
| 2556x1179 | 57 MB | 11 MB | 103 MB |
| 3840x2160 | 158 MB | 31 MB | 284 MB |

What the numbers say:

- **Forward + 4x MSAA is cheaper than deferred's main pass**: 0.77x (light) and
  0.94x (heavy) at the phone's render ceiling, 0.62x at the phone's native size,
  0.5-0.8x on the Mac. The deferred cost is writing and reading the G-buffer; it
  grows with pixels, so the gap grows with resolution.
- **4x MSAA is almost free on these GPUs**: +1-24% over forward 1x. Apple GPUs
  shade once per pixel and keep the 4 samples in tile memory.
- **Memoryless 4x targets cost no time, only memory saved**: "stored" and
  "memoryless" time the same (both discard the samples), but memoryless keeps
  4 B/pixel instead of 36.
- **A depth pre-pass is harmful on Apple GPUs**: +6% to +89%, and with heavy
  geometry it nearly doubles the frame. The GPU already removes hidden pixels
  before shading (hidden surface removal), so drawing every object twice only
  adds vertex work. A pre-pass is useful only for what reads its depth (SSAO).
- **The images match** (compared at full size): the same colours everywhere; the
  deferred and 1x edges are stair-stepped, the 4x edges are smooth.

### 111 (C forward) vs 112 (deferred)

The two metaball examples draw a similar scene through the two existing
renderers. They are not the same work: 112 adds shadow cascades, SSAO, a depth
pyramid, water (FFT, underwater fog), TAA, SMAA, bloom and the render ceiling.

| | 111 forward (C) | 112 deferred (Rae) |
|---|---|---|
| Mac, headless, native 2560x1600 | 9.2 ms | 9.0 ms |
| Mac, headless, default render ceiling | (no ceiling) | 6.8 ms |
| iPhone 15 Pro, frame time | 26 ms (GPU wait 19-22) | 33 ms (paced at 30 fps by its resolution controller) |

On the Mac (per-pass timestamps of 112, rough because of the overlap described
above) the G-buffer and lighting passes are about 0.55 ms of a 9.3 ms GPU
total: **6%**. The rest is the effects. So even a main pass twice as fast would
change 112's frame by about 0.3 ms on the Mac; on the phone, where the
prototype's gain at the ceiling is about 0.1 ms per frame in both scenes, it is
within the noise of a 33 ms frame.

## 3. What a forward main pass changes

- **SSAO.** Rae's SSAO reads depth (and the depth pyramid). WebGPU has no depth
  resolve, so a 4x pass cannot hand its depth on. Options: a stored 1x depth
  pre-pass (measured above: +6-89% time, and the 1x depth does not match
  the 4x edges), writing linear depth to a second colour target and resolving it
  (averages depth at edges: halos), or storing the 4x depth and resolving it in a
  compute pass (gives up the memoryless saving). The deferred G-buffer has the
  depth and normals for free.
- **Decals.** Deferred decals draw into the G-buffer. Forward needs every
  material's shader to loop over a per-tile decal list (clustered decals).
- **Many lights.** Deferred lighting runs once per pixel over all lights. Forward
  needs a clustered or tiled light list in every material shader. Rae's scenes
  are sun-lit today, so this is not a cost yet, but it is where the design goes.
- **Transparents and water.** These are already forward in the deferred renderer
  (the "transparent forward" pass); in a forward renderer they are more draws in
  the same pass, and MSAA smooths their edges too. Refraction needs a copy of the
  opaque image, so the 4x pass must resolve before water, which ends the
  single-pass, memoryless benefit there.
- **Antialiasing.** MSAA smooths geometry edges only. Specular shimmer and thin
  sub-pixel detail (grass) still need TAA, which needs motion vectors: another
  target the forward pass must write (and resolve). The deferred renderer already
  has TAA, SMAA and FXAA.
- **Render ceiling and dynamic resolution.** Both apply unchanged (MSAA renders
  at the scene size and the composite upscales). 4x MSAA at the phone's native
  size (574 µs, light) costs 1.3x deferred at the ceiling (444 µs).
- **Pipelines.** `createRenderPipeline` (lib/gpu/GpuRender.rae) hardcodes
  `multisample: { count: 1 }`; a forward path needs a sample count in
  `RenderPipelineSpec` and a second pipeline for every material at 4x.
- **wgpu-native bug.** `WGPUTextureUsage_TransientAttachment` (0x20) panics in
  wgpu-native 29: it passes usage bits to wgpu-core unchanged, where `TRANSIENT`
  is `1 << 17`. The prototype uses wgpu-core's bit. A shipping path would need
  the binding fixed upstream (or this workaround in lib/).

## 4. The C forward code it would replace

The forward renderer is mostly C: `runtime_gpu3d.c` 748 lines, `_shaders` 341,
`_sdf` 324, `_sky` 206, `_skin` 364, `_ssao` 568, `_shadow` 51, `_stubs` 99:
**about 2,700 lines of C**, plus the Rae wrappers `lib/Gpu3d.rae` (886),
`lib/Gpu3dNative.rae` (385) and `lib/Renderer3d.rae` (858). It is not a lean
forward pass: since #333 it is single-sampled and writes colour, normals and
motion vectors with a stored depth, a thin G-buffer for its SSAO and TAA.

Its one remaining user is 111 (96, 97 and 109 already draw through the
deferred renderer; 112 is 111's deferred twin). The Rae wrappers are partly
shared: `RendererDeferred`, the water system, `Particles` and the physics scenes
still open `Gpu3d` for its common types and helpers, so only their forward-only
parts would go.

A Rae-first forward path would reuse the deferred renderer's Rae for shadows,
sky, SDF, skinning, bloom and the composite, and add the 4x targets, a sample
count in the pipeline spec, a forward entry per material around the shared
lighting WGSL, and the render-graph wiring: an estimated 800-1,200 lines of Rae
and WGSL. 111 could then move onto it and the C could be deleted.

But a new forward renderer is not needed to delete the C: 112 already shows the
same scene on the deferred renderer, which is Rae over the bindings and has the
effects 111 has in C (SSAO, TAA, sky, SDF metaballs, skinning). Retiring 111 (or
porting it to the deferred renderer) retires the same ~2,700 lines of C and the
forward-only part of the wrappers without adding a second main pass to maintain.

## 5. Recommendation

1. **Do not add a forward + MSAA main pass now.** The measured gain is real but
   small where it matters: about 0.1 ms per frame at the phone's render ceiling,
   in a frame where the main pass is around 6% of the GPU time. Its cost is a
   second version of SSAO input, decals, motion vectors, refraction and every
   material pipeline, and a wgpu-native workaround.
2. **Retire the C forward path through its last user, 111**: drop it in favour
   of its deferred twin 112, or port it to the deferred renderer, rather than
   write a new forward renderer (this also settles runtime C audit row 18). This
   removes about 2,700 lines of C and needs no new renderer design. It is the
   maintainer's call whether 111 should survive as a forward demo.
3. **Keep the prototype** (`benchmarks/forwardMsaa`) to repeat the comparison
   if the deferred main pass grows: many lights, a fatter G-buffer, or a target
   device with a slower memory bus would move the numbers toward forward.
4. **Do not add a depth pre-pass** to any Rae pass on Apple GPUs: it was slower
   in every case measured here.
5. **What would change the answer:** a game whose frame is dominated by the main
   pass (few effects, many pixels), a VR-like need for sharp geometric edges
   without TAA blur, or memory pressure on a smaller phone where 50 MB of
   targets matters. Then forward + 4x MSAA with memoryless targets is the
   design, and these numbers are its starting point.

## 6. The forward main pass in lib/ (forward-rae-1)

The first step of the Rae-first forward renderer has landed. It changes nothing for
deferred apps, and no example uses it yet.

- **Using it:** `createForwardRenderer()` (or `…WithUi()` / `…WithUiSky(sky:)`,
  `lib/RendererForward.rae`) returns the same `DeferredRenderer`, with the same
  frame calls (`compileDeferred`, `renderDeferredFrameWorld` or the per-tag
  `renderDeferredPassWorld`, the shadow pass, bloom, the render ceiling, the
  dynamic scale, direct present). Its graph is `shadow -> forward -> bloom ->
  composite -> [ui] -> present`.
- **The pass:** `lib/ForwardMain.rae`. It runs one render pass at 4x MSAA with
  memoryless (TRANSIENT) 4x colour and depth, and resolves into the lit image
  when the pass ends. There is no depth pre-pass. The sky is drawn last with
  depth Equal at the far plane, so only uncovered samples evaluate it.
  `RenderPipelineSpec.sampleCount`, `createTextureSampled`,
  `addResolvedColorTarget` and `RenderTargetSet.depthDiscard` are the GPU-layer
  pieces it needs.
- **The same shading:** the deferred lighting's per-pixel code is now
  `shadeSurface` in `lib/deferred_shade.wgsl`. The lighting pass
  (`deferred_light.wgsl`) calls it with what the G-buffer holds, and the forward
  material (`forward_static.wgsl`) calls it with what it just computed. Both use
  one uniform layout (`deferred_uniforms.wgsl`) and one light upload
  (`uploadSceneLight`). The static mesh's vertex stage and its brick, roof and
  emblem patterns are shared too (`gbuffer_static_surface.wgsl`).
- **Checked by:** fixture 1130 renders a toon sphere over a lit ground with both
  renderers on the GPU and reads back their lit images. The sphere and the sky
  are equal. The ground is within 0.7%, because the deferred image has SSAO and
  G-buffer rounding.
- **The deferred image after the split:** 112 and 114 screenshots at a fixed
  frame match the previous build. About 1.6–5% of pixels differ, by at most 3
  of 255 levels (the GPU compiler orders the arithmetic differently). The
  pixels that differ by more are at the run-to-run noise level of an unchanged
  build.
- **Not in this step:**
  - No SSAO, TAA, FXAA or SMAA (MSAA is the antialiasing).
  - No transparents, water or underwater fog: they read the G-buffer depth.
  - Skinned meshes, terrain, metaballs, sprites and grass have no forward
    material yet. A forward frame skips them (`GbufferCache.forwardFrame`).
  - The renderer still allocates the G-buffer and the deferred post targets:
    the G-buffer sets the scene size, and the post passes share the target set.

  The metaballs are forward-rae-2, the C forward path's removal forward-rae-3,
  and the walker's forward mode forward-rae-4.

**forward-rae-2 (2026-10-15):**

- **What moved:** 111 draws through this pass, no longer through the C
  renderer. Its program is 112's, with `createForwardRendererWithUi` and a
  `RenderTag.forward` step (`beginForwardPass`, the scene's draws,
  `endForwardPass`).
- **Metaballs:** they have a forward material (`forward_sdf.wgsl` over the
  shared raymarch `sdf_surface.wgsl`). `GbufferSdf.drawMetaballs` uses it in a
  forward frame. Their silhouettes are not antialiased: the raymarch runs once
  per pixel.
- **Measured at the default render ceilings:**
  - Mac: 111 takes 6.1 ms a frame, 112 takes 6.7 ms.
  - iPhone 15 Pro: 111 takes 28.4 ms (35 fps), 112 takes 32.3 ms (31 fps).

  The table and every difference from 112 are in
  `examples/111_metaballs_forward/README.md`.


**forward-rae-3 (2026-10-15):**

- **Deleted:** the C forward renderer, with no user left. That is
  `runtime_gpu3d.c` and its shader, metaball, sky, skinning, SSAO, shadow and
  stub files, the shared sky table, and the Rae modules over them (`Gpu3d`,
  `Gpu3dNative`, `Gpu3dSky`, `Gpu3dWorld`, `Gpu3dTemporal`, `Renderer3d`). About
  2,900 lines of C and 2,560 lines of Rae are gone.
- **Kept:** the only piece other code used, the world shadow-caster walk. It
  moved to `ShadowMaps.renderSceneShadowWorld`.
- **Forward renderer:** `lib/RendererForward.rae` is now the only forward frame.
  Fixture 553 pins its derived pass order.
