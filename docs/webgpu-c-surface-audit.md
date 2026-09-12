# WebGPU C-surface audit + gate (#505)

Close-out of the WebGPU bindings epic (#496). This enumerates every renderer
`rae_ext_*` C entry point, classifies each as **permanent ABI/platform** vs
**renderer logic that belongs in Rae**, records the zero-copy upload path, and
defines the **gate** that keeps new renderer-specific C helpers from creeping
back in.

Regenerate the raw inventory any time with:

```sh
grep -rhoE '\brae_ext_(gbuffer|gpu3d|gpu2d)_[A-Za-z0-9_]+' compiler/runtime/*.c | sort -u
```

## Result in one line

The **deferred renderer** (examples 110/112/113/114) is migrated: it drives the
GPU entirely from Rae over the generated bindings, and the only C it still
touches is the low-level `rae_gb_*` / `rae_sm_*` / `rae_g2d_*` handle/getter/
uniform-upload **ABI seam** (161 symbols) plus two frame-math helpers
(`skyHosekPush`, `pyramidMips`). That is exactly the "genuine ABI + frame-derived
math + shader source stays in C" line drawn in #496 — nothing high-level
remains. Two other tracks are explicitly *out* of the #502–#504 scope and are
recorded here as known debt: the **forward renderer** (`runtime_gpu3d.c`, still
fully C) and three **dead leftovers** superseded by the migration.

## Category A — permanent C (genuine ABI / platform / shader infra). KEEP.

These are not renderer logic; they are the seam the Rae renderer calls through.

- **Handle/getter/setter/uniform-upload ABI seam** — `rae_gb_*`, `rae_sm_*`,
  `rae_g2d_*` (161 symbols). Create/return WGPU handles, expose device globals,
  and upload frame-derived uniform math (view-proj, cascade fits, SSAO kernel,
  Hosek sky coefficients). The Rae passes own all command encoding, bind-group
  creation, and pass loops over these.
- **gpu2d platform glue** — window + input + idle-wait + cursor + clock:
  `initWindow`, `pollClose`, `waitEvents`, `closeWindow`, `window*`, `pointer*`,
  `wheelMove`, `setMouseCursor`, `nowSeconds`, `setDesignResolution`, `dpr`,
  `designWidth/Height`. Owns SDL3/Metal-layer/surface — genuinely platform.
- **gpu2d image decode + font raster** — `loadImage*`, `decodeImageProbe`,
  `hasImageKey`, `drawImage*`, `drawGlyph*`. macOS ImageIO / MSDF atlas; decode
  and glyph rasterization are platform/asset infra, not render-graph logic.
- **gpu2d primitive batch encode** — `flush`, `drawRect/RoundedRect/Box/`
  `GradientRect/Line`, `push*ClipRect`, `popClipRect`. The per-frame instance
  batching kernel; the frame *lifecycle* around it is now Rae (#504 part 8).
- **Two deferred frame-math helpers** — `rae_ext_gbuffer_skyHosekPush` (analytic
  Hosek–Wilkie sky coefficients → lighting uniform) and
  `rae_ext_gbuffer_pyramidMips` (depth-pyramid mip-count getter).
- **Shader source** — WGSL lives as C string literals returned to Rae for
  `createShaderModule`. Staying near C is fine; moving to Rae string constants
  is optional and not required by #496.
- **Resource upload + lifetime** — `meshCreate`, `meshUpdate`, `skinnedMeshCreate`,
  `setPalette` (VBO/IBO/palette upload) and the `*Shutdown` teardown calls.
  Thin `createBuffer`+`queueWriteBuffer` / release wrappers; migratable but low
  value and correctly ABI-adjacent, so they stay for now.

## Category B — migrated to Rae (#502–#504). No high-level C remains.

Deferred geometry, instanced draws, lighting, SSAO, TAA, composite,
depth-pyramid, sky pass, SDF/metaballs, cascade shadow render, the G-buffer
inspector, and the gpu2d frame lifecycle are all Rae now (`lib/gbuffer.rae`,
`lib/gbuffer_passes.rae`, `lib/gbuffer_shadow.rae`, `lib/gbuffer_inspector.rae`,
`lib/renderer_deferred.rae`, `lib/gpu2d.rae`). Render-graph orchestration lives
in `lib/renderer_deferred.rae`. The shadow **caster recording**
(`shadowBegin`/`shadowDraw`/`shadowDrawSkinned`/`shadowMetaballs`) is still a
thin C append-list that `shadowEnd` (Rae) consumes via `rae_sm_*` — a small
recording buffer, not pass logic; a minor future cleanup, not a blocker.

## Category C — dead C, superseded by the migration. REMOVE (follow-up #513).

Zero live bindings from any `lib/*.rae`; the Rae replacements exist:

- `rae_ext_gbuffer_debugView` — replaced by `gbuffer_inspector.rae` `debugView`.
- `rae_ext_gbuffer_ssao` — replaced by `gbuffer_passes.rae` `ssaoPass`.
- `rae_ext_gbuffer_present` — replaced by the Rae composite/present path.

Safe to delete after confirming no bare-`extern` (default-mangled) binding
remains; tracked as #513 so it is a scoped commit, not a drive-by.

## Category D — forward renderer (Track A). NOT migrated; known debt (#514).

`runtime_gpu3d.c` (~1221 lines) + `runtime_gpu3d_ssao.c` + `runtime_gpu3d_sky.c`
still C-encode a *complete second renderer*: `rae_ext_gpu3d_{begin, draw,`
`drawMetaballs, drawSkinned, end, submit, tonemap, taa, ssao, skyDraw,`
`skyHosekPush}`. It is reached through `lib/gpu3d.rae`'s `beginScene`/pass
wrappers and still drives the forward examples (109 PBR, 111 metaballs-forward).
This was never in the #502–#504 deferred scope. It is the last real block of
high-level graphics C. Migrating it (or retiring it in favour of the deferred
path) is its own phase, filed as #514 — not required to start grass.

## Zero-copy upload path (as required by #505)

`List(T)` lowers to `{ T* data; i64 length; i64 cap }`. `list.data` crosses the
FFI as a bare `T*` with **no copy**; the element count is passed as a separate
argument (the length is dropped at the boundary — see #499/#500). Uploads use
`wgpuQueueWriteBuffer`, which **copies synchronously into the queue**, so the
source pointer need not outlive the call and there is no realloc/lifetime hazard
on the Rae side. There is **no** `List → temp C array → memcpy → upload`
indirection anywhere in the renderer upload path — the draws SSBO, vertex, index,
and instance uploads all hand `list.data` straight to `queueWriteBuffer`.

The one legitimately-required copy is exactly that `queueWriteBuffer` internal
copy; it is unavoidable and correct (the alternative, a mapped buffer, would keep
the pointer live and reintroduce the realloc hazard). No mapped/async upload path
holds a `List.data` pointer across a potential resize.

## The gate

**Rule:** new renderer functionality is reached through the Rae WebGPU bindings
(`lib/webgpu/*.rae`, `lib/gpu*.rae`, `lib/gbuffer*.rae`), **not** by adding a new
`rae_ext_gbuffer_*` / `rae_ext_gpu3d_*` C helper. Future needs — compute
pipelines, indirect draws (`DrawIndexedIndirect`), storage buffers, texture
arrays, query sets, timestamps — go through the bindings. New C is allowed only
when it is genuine platform ABI (a new `rae_gb_*`/`rae_sm_*`/`rae_g2d_*` handle
accessor or uniform upload), and it must be added to the allowlist in the same
commit with a one-line justification.

**Enforcement:** `tools/webgpu-c-surface-gate.sh` diffs the current renderer
`rae_ext_(gbuffer|gpu3d)_*` symbol set against
`tools/webgpu-c-surface-allowlist.txt` and fails if a symbol appears that is not
on the list. Run it via `make c-surface-gate` (from `compiler/`). Removing a
symbol (e.g. finishing #513/#514) means deleting its allowlist line; that is
always allowed. Adding one is the reviewable event.

---

## #868 unsafe-boundary audit of GPU/render/water foreign consumers (#876)

Ahead of #877 (which makes the #868 pointer-operation boundary UNCONDITIONAL:
every `extern` must be `unsafe extern`, every unsafe call / raw-Ptr op must sit
in an `unsafe { ... }` block), this records the disposition of every remaining
hand-written GPU/render/water foreign consumer. No silent module exemptions.

**Migrated in #876 (done, verified):**
- `examples/zz_readback_check` and `examples/zz_gpu_timing_check` — the raw
  (pre-manager) readback and GPU-timing hardware checks: extern → `unsafe
  extern`, every webgpu call / raw-Ptr op wrapped at its source site. Both run
  green on hardware.
- Positive compiler fixtures `626_extern_symbol` and `776_ptr_c_lowering` →
  `unsafe extern` + wrapped raw-Ptr ops (776 uses the safe-factory pattern for
  its Ptr-field struct, since `let x = unsafe {}` is rejected). The negative
  cases `779`/`780`/`781`/`786`/`789` are retained unchanged — they lock in the
  order/placement diagnostics #877 turns on.

**Callback userdata / delayed callbacks — nothing to migrate.** The only
delayed-callback path is `wgpuBufferMapAsync` (already `unsafe extern`, takes
`WGPUBufferMapCallbackInfo{callback, userdata1, userdata2}`). It has NO Rae
caller: the async map, its callback and its userdata are owned entirely by the
C bridge `runtime_webgpu_readback.c` (`rae_wgpu_read_start/poll/copy/release`),
which the #887 `ReadRequest` and the manager `ReadbackId` wrap. `gpu/GpuLifetime`
is explicitly "no callbacks and no callback-reachable state in this layer." No
unsafe Rae callback-userdata consumer exists.

**Water — already clean** (#873/#874): zero non-`unsafe` extern declarations;
`water/WaterGpuBridge` is the one opted-in unsafe module and is fully wrapped.

**Recorded remaining consumer migrations (prerequisites for #877).** These
modules still declare plain `extern` and call webgpu externs unwrapped; they are
being REWRITTEN onto the manager by already-filed slices, so their #868
migration folds into those slices rather than churning code about to be
replaced. Each slice must leave its module unsafe-clean (extern → `unsafe
extern`, every unsafe call / raw-Ptr op wrapped) as part of its work:

| module | plain externs | migrates via |
|---|---|---|
| `lib/Gbuffer.rae` | 63 | #905/#906 (mesh store, frame/targets/pass/submit) |
| `lib/Gpu3d.rae` | 57 | #905/#906 |
| `lib/GbufferPasses.rae` | 52 | #904 (ao/light/composite/taa/pyramid/fog) |
| `lib/GbufferShadow.rae` | 13 | #906 (shadow maps) |
| `lib/Gpu2d.rae` | 13 | #907–#910 (2D renderer C→Rae) |
| `lib/GbufferTerrain.rae` | 10 | #904 |
| `lib/webgpu/Context.rae` | 9 | #906 (device/queue/bootstrap accessors) |
| `lib/TransparentForward.rae` | 8 | #904 |
| `lib/GbufferInspector.rae` | 8 | #904 |
| `lib/GpuTiming.rae` | 3 | #906 (legacy blocking timing folds into gpu/GpuTiming) |
| `lib/app3d/RenderScale.rae` | 2 | #906 (DRS/thermal setters) |
| `lib/Shadow3d.rae` | 1 | #906 |

Note: a bare `unsafe extern` DECLARATION compiles today without opting a file in
or forcing call-site wrapping, so these declarations could be flipped early; the
COST is the call-site wrapping, which is why it is folded into the rewrites (a
module that becomes manager-owned wraps its remaining genuine-platform calls
naturally, as water/grass did). `#877` must therefore run only after #904/#905/
#906 and #907–#910. One dead adapter noted for removal there: `setShadowAmbient`
(`rae_sm_set_shadow_ambient`) has zero call sites.

**#877 update:** the boundary is now unconditional (see `docs/unsafe-boundary.md`).
The render/2D modules listed above were made #868-clean as part of #877 itself
(public APIs became safe wrappers over renamed `native*` externs; internal
Ptr-handling functions wrap at their source site), so #904–#910 inherit
already-clean modules rather than owning the migration.

**#907 update (2D renderer slice 1 — the box pass):** the box/primitive
pipeline, its per-flush instance buffers and per-run bind groups are typed IDs
in a Rae-owned `gpu/GpuResources` manager (`lib/Gpu2dBox.rae`, `Gpu2dBoxPass`,
the WGSL is the asset `lib/gpu2d_box.wgsl`); the draws go into the C-owned
frame pass via `recordDrawInPass`, one draw per contiguous same-clip run with
the scissor and a per-run clip uniform, exactly as the C flush did. C keeps
the CPU-side batch (`rae_g2d_push` / `rae_g2d_push_gradient` behind the
`drawRect/RoundedRect/Box/GradientRect/Line` entry points, read back through
`rae_g2d_prim_count/floats/data/clip_at/reset`), the shared viewport uniform
(`rae_g2d_viewport_uniform`, adopted as an external; the image and text bind
groups still reference it) and the per-run clip uniform + scissor
(`rae_g2d_clip_frame_uniform`, `rae_g2d_scissor`); `rae_g2d_prepare_flush`
uploads the viewport transform before the Rae draw and `rae_ext_Gpu2d_flush`
now draws only images and text. Removed from C: `rae_g2d_init_pipeline`,
`rae_g2d_ensure_inst`, `rae_g2d_rebuild_bind`, `rae_g2d_box_frame_buffer`,
the box globals and `G2D_BOX_WGSL`. Ownership: `Gpu2d.flush` / `Gpu2d.endFrame`
take `box: mod Gpu2dBoxPass` — an app owns one (the deferred renderer owns one
for its UI-overlay present) and the UI render system threads it to its
per-entity flush. The batch accumulation itself stays C because the public
draw API (`Gpu2d.drawRect(...)`) is stateless; moving it is the owner-threading
decision recorded as a follow-up.

**#908 update (2D renderer slice 2 — the image pass):** `Gpu2dBoxPass` became
`Gpu2dCanvas` (`lib/Gpu2dCanvas.rae`), the ONE app-owned owner of the 2D
passes on one manager. Images: C keeps CPU decode only (`rae_g2d_decode_image`
/ `decoded_width` / `decoded_height` / `decode_free` over stb_image, plus the
device-free `decodeImageProbe`); the RGBA8 textures + views (`createTexture`,
new `writeTextureFromPtr` / `writeTexture` uploads), the image pipeline
(`lib/gpu2d_image.wgsl`), the linear clamp sampler, the per-draw 64-byte
uniform pool and the per-draw-slot bind groups (cached by image handle, as the
C flush cached them) are manager IDs; the key->handle registry and the draw
queue are Rae lists on the canvas (`ImageRegistry.rae` keeps its load / retry /
cache policy over `canvas`). Draws go into the C frame pass after the boxes
and before text, one `recordDrawInPass` per queued image with its scissor. C
lost the image texture table, `rae_g2d_init_img_pipeline`, `queue_image`,
`flush_images`, the per-slot bind cache and every `rae_ext_Gpu2d_*Image*` entry
point (`loadImage`, `loadImageKey`, `registerImageRgba`, `registerImageKeyHandle`,
`hasImageKey`, `drawImage`, `drawImageKey`, `drawImageKeyScaled`, `imageView`);
`Gpu2d.loadImage / loadImageKey / registerImageRgba / hasImageKey / drawImage*`
now take `canvas`. The manager's dependency arena is compacted by relocation
(`compactDependencies` copies the slices still owned by live / pending groups,
submissions and readbacks) so the per-frame box groups and the cached image
groups coexist without growth.

**#909 update (2D renderer slice 3 — the text pass):** the MSDF text pipeline
(`lib/gpu2d_text.wgsl`), the atlas textures + views (uploaded lazily from the
C atlas pixels through `writeTextureFromPtr`), the per-atlas per-flush glyph
instance buffers and the per-flush bind groups are manager IDs on the
`Gpu2dCanvas`; the text draw is one bind per atlas per flush and one
`recordDrawInPass` per contiguous same-clip run, after the images, exactly
the C flush's shape. C keeps glyph rasterization + the CPU atlas pixels
(`SdfText.loadAtlas`, read back via `rae_sdf_atlas_pixels/width/height`) and
the per-atlas CPU glyph batch behind the stateless `drawGlyph` /
`drawGlyphEx` (read back via `rae_g2d_text_atlas_max/floats/count/data/
clip_at/reset` — the same owner-threading question as the box batch, #913).
Removed from C: `rae_g2d_init_text_pipeline`, `rae_g2d_rebuild_text_bind`,
`rae_g2d_ensure_text_inst`, `rae_g2d_atlas_texview`, `rae_g2d_text_frame_buffer`,
the text pipeline / sampler / atlas texture globals, `G2D_TEXT_WGSL`, and
`rae_ext_Gpu2d_flush` itself — `Gpu2d.flush` is now entirely Rae (boxes,
images, text), C only uploads the viewport transform in `rae_g2d_prepare_flush`.

**#910 update (2D renderer slice 4 — the frame):** the frame's command encoder
and render pass are owned by the `Gpu2dCanvas` (`beginFrame` / `beginFrameLoad`
take the canvas and open the pass into the C offscreen target; the box / image
/ text passes draw into `canvas.framePass`; `endFrame` closes, submits and
clears it) — nothing is parked in C any more (`rae_g2d_set_frame` /
`pass_get` / `encoder_get` / `frame_active` removed). The per-run clip
uniforms are manager buffers on the canvas (a per-run-slot pool, filled by
`rae_g2d_clip_uniform_at`); the C frame-kept buffer / bind-group lists
(`rae_g2d_keep_frame_buf` / `keep_frame_bind`, `rae_g2d_clip_frame_uniform`)
are gone, and the scissor takes the pass (`rae_g2d_scissor(clip, pass)`).
Still C, deliberately: the offscreen presentable target + surface configure,
present (drawable acquire / copy / present / poll) and the headless screenshot
readback (`rae_g2d_present_and_cleanup`) — surface/platform glue whose
in-flight teardown equivalence can only be verified in a window run; the clip
stack + design→physical transform (CPU state behind the stateless
`pushClipRect` / `popClipRect` API, the same owner-threading question as the
batches, #913); and the viewport uniform (adopted). The canvas's pass is a
Rae-opened raw pass, not yet a manager `Recording` — that, the present/teardown
replacement and the clip stack are #915.

**#913 update (2D renderer slice 1b — the box batch):** the primitive
accumulation is Rae: `Gpu2dCanvas` holds `prims: List(Float)` (the same
24-float record — rect, four radii, premultiplied fill, premultiplied border,
params, premultiplied gradient end) + `primClips`, packed by
`canvasDrawRect / RoundedRect / Box / GradientRect / Line` with the same
0xAARRGGBB premultiply and the same line→capsule math; `boxFlush` uploads
`canvas.prims` directly. The owner spelling is `canvas: mod Gpu2dCanvas` on
the whole `Gpu2d.draw*` family (one owner, already carrying the image and
text passes). Removed from C: `rae_g2d_push` / `push_gradient`, `g2d_color`,
the prims array + per-prim clip index, the `rae_g2d_prim_*` accessors and the
`rae_ext_Gpu2d_drawRect / drawRoundedRect / drawBox / drawGradientRect /
drawLine` entry points; `rae_g2d_prepare_flush(pending)` now takes the canvas's
box + image count. Still C behind a stateless API: the text glyph batch
(`drawGlyph` / `drawGlyphEx`) and the clip stack — both #915.

**#915 update (2D renderer slice 4b — the frame as a Recording, clips, text
batch):** (a) the 2D frame is a manager `Recording` on the canvas
(`lib/Gpu2dCanvasFrame.rae`): `canvasOpenFrame` adopts the C offscreen view
(re-adopted when surface configure replaced it), `beginRecording`s and opens a
pass over it with the new `GpuRender.beginRenderPass(recording, resources,
target)` (raw pass held for the `recordDrawInPass` family, `endRenderPass` to
close); `canvasCloseFrame` records every per-frame bind group and the three
pipelines as uses (`recordBindGroupUse` / `recordRenderPipelineUse`) and
`trackSubmission`s the frame into `canvas.inFlight`, so `boxFrameEnd`'s retire
of the per-frame groups is deferred behind the tracked submission
(`pollSubmissions`) instead of leaning on wgpu's own tracking; `canvasShutdown`
`drainSubmissions` first. (b) The clip stack (`clipX/Y/W/H/Radius`, `clipFull`,
`clipStack`, `currentClip`), the per-run scissor (`g2dScissor` over
`setScissorInPass`, the same design→physical clamp math), the rounded-clip
uniform fill (`clipUniformFor`) and the viewport uniform (`canvas.viewport`, a
manager buffer written per flush from `rae_g2d_xform`) are Rae;
`Gpu2d.pushClipRect / pushClipRoundedRect / popClipRect` take the canvas. The
text glyph batch moved with them (`Gpu2dCanvasText.rae`, `glyphs: List(Float)`
20 floats per glyph + `glyphClips` per atlas — it needed the canvas's clip
index), so `Gpu2d.drawGlyph / drawGlyphEx` and the whole `Gpu2dText.drawText`
family take the canvas. Removed from C: the clip stack + `rae_g2d_set_scissor` /
`push_clip` / `fill_clip_uniform` / `clip_reset` / `current_clip`, the
`rae_ext_Gpu2d_pushClipRect / pushClipRoundedRect / popClipRect` entry points,
`rae_g2d_scissor` / `clip_uniform_at` / `prepare_flush`, the viewport uniform
(`rae_g2d_viewport_uniform`), the glyph batch + `rae_g2d_text_floats / count /
data / clip_at / reset` and `rae_ext_Gpu2d_drawGlyph / drawGlyphEx`. Added:
`rae_g2d_xform(out)` (the surface-derived transform; frame-derived uniform data,
the allowed class). `Gpu2dCanvas.rae` was split for the 1000-line cap into
`Gpu2dCanvas.rae` (type, box + image passes, clips, shutdown),
`Gpu2dCanvasText.rae` and `Gpu2dCanvasFrame.rae`. (c) — the offscreen
presentable target as a canvas texture and `rae_g2d_present_and_cleanup`
replaced by owner teardown — is NOT in this slice: it needs the in-flight
teardown equivalence check in a window run (RAE_WGPU_REPORT live-object counts
across 120 frames, occluded and headless), filed as #920.

**#920 update (2D renderer slice 4c — owner teardown of the presentable
target, closes #875):** the offscreen presentable target is a canvas texture +
view (`Gpu2dCanvas.offscreenTexture/offscreenView`, RenderAttachment |
CopySrc in the surface format), built and rebuilt by `canvasEnsureOffscreen`
to the configured surface size (`rae_g2d_surface_ready/width/height` — C only
configures the surface now; `rae_g2d_configure` creates no texture). The 2D
frame's pass targets `textureViewRef(id: canvas.offscreenView)` (no external
adoption), and the 3D passes that write the LDR image take the canvas:
`Gpu3d.tonemapPass(canvas:)` / `end(canvas:)` / `endFrame(canvas:)`,
`GbufferPasses.composite / compositePass / compositePassGraded(canvas:)`,
`GbufferInspector.debugView / inspect(canvas:)`, `Gbuffer.present /
presentFrame(canvas:)`, `Renderer3d.renderFrameWorld(canvas:)` —
`RendererDeferred` passes `renderer.canvas`. Present stays C but BORROWS the
texture: `rae_g2d_present(texture, w, h)` (was `rae_g2d_present_and_cleanup`),
`rae_g3d_present_frame(texture, w, h)` and `rae_ext_Gbuffer_present(texture, w,
h)` do the drawable acquire + copy (clamped to the smaller of target and
surface) + present + poll and the headless `RAE_GPU2D_SCREENSHOT` readback
(`rae_g2d_save_screenshot(path, tex, w, h)`); the occluded-window skip is
unchanged. `Gpu2d.closeWindow` releases no Rae-owned object any more (surface
+ cursors only); `canvasShutdown` retires the target after draining the
in-flight submissions. Removed from C: `g_g2d_off_tex/off_view/off_w/off_h`,
`rae_g2d_off_view`, `rae_g2d_off_view_ready`, `rae_g2d_present_and_cleanup`;
`rae_gb_offscreen_w/h`, the forward prepass target size and the deferred /
tonemap readiness checks read the surface instead. Teardown equivalence
(RAE_WGPU_REPORT live-object counts flat across 120 frames, the occluded path,
the headless screenshot path) is a window run the queue cannot do — it is the
user's confirmation the task waits on.

**#905 update (shared renderer slice 2a — the mesh store):** static meshes
are Rae: `lib/MeshStore.rae` holds one `MeshEntry { vbuf, ibuf, indexCount,
vertexCount }` of manager `BufferId`s per 1-based `MeshHandle` id, owned by
the renderer object — `DeferredRenderer.passes.gbuffer.meshes` (the
`GbufferCache` the geometry-pass draws already take) and a new manager +
`Renderer3d.meshes` for the forward path. Uploads are
`RendererDeferred.uploadMesh / updateMeshVerts(renderer:, …)` and
`Renderer3d.uploadMesh(r:, …)` (`meshStoreUpload` narrows the Rae Int indices
to u32 and `writeBufferFromPtr`s both buffers); the geometry / terrain /
transparent / forward passes resolve a handle to raw buffer handles with
`meshStoreVbufHandle / IbufHandle` (`refHandle` over `bufferRef`), the water
system BORROWS them as externals into its own manager (`adoptMeshExternals`
takes the store + the renderer's manager), and a static shadow caster is queued
by its handles (`rae_sm_queue_mesh(mesh, vbuf, ibuf, icount, model)` — the
queue is still C, #906's scope; the mesh id stays as the batching key).
`shutdownDeferredRenderer` / `shutdownRenderer3d` retire the buffers.
Removed from C: `g3d_mesh_vbuf/ibuf/icount/n`, `rae_ext_Gpu3d_meshCreate /
meshUpdate`, `rae_gb_mesh_ready/vbuf/ibuf/icount`, `rae_g3d_mesh_vbuf/ibuf/
icount`; `rae_g3d_push_draw_record` no longer validates the mesh slot (the
store did). Still C, filed as #921 (slice 2b): the SKINNED meshes + the joint
palette (`g3d_skin_vbuf/ibuf`, `rae_gb_skin_*`, `rae_ext_Gpu3d_setPalette`,
the palette storage buffer the skin bind groups and shadow casters read) —
they are one subsystem with the C skin pipeline and move together.


**#906 update (shared renderer slice 3a — the geometry FRAME):** the deferred
geometry frame is the Rae `GbufferCache`'s (`lib/Gbuffer.rae`,
`lib/GbufferResources.rae`), on the DeferredRenderer-owned manager. The four
G-buffer targets (A rgb10a2, B/C rgba8, depth32) + their views are manager
`TextureId`/`TextureViewId`s rebuilt by `ensureTargets` to the offscreen size
(`rae_gb_offscreen_w/h` — the surface at the render scale, C glue) with a Rae
`targetsGen`; the frame uniform and the draws storage buffer are manager
`BufferId`s (`ensureGbBuffers`, `gbufferMaxDraws = 4096`,
`gbufferFrameFloats = 36`) with the draw cursor `drawCount` in Rae; each frame
is a manager `Recording` opened with the new multi-attachment
`gpu/GpuRenderPass.beginRenderPassMulti` over a `RenderTargetSet` (three
colour views + depth, per-attachment clear colours) and submitted TRACKED by
`end(cache:, resources:)` (pipelines + binds recorded as uses; `inFlight`
polled per frame, drained by `gbufferShutdown`). Every G-buffer draw — static,
skinned, instanced, terrain, sprite, grass, transparent — writes its records
with `writeBufferFromPtr` into `cache.draws` at the cursor and records into
`cache.pass`. The terrain, sprite and transparent binds name the frame
buffers by ID (no adoption); grass and water live in the App's manager and
BORROW the frame uniform / depth view handles across managers (a Rae-owned
external now, no C accessor). C keeps: the WGSL sources; the frame-derived
uniform MATH (`rae_gb_frame_data(viewProj, clear, w, h, out)` fills the 36
floats and remembers the jittered / previous view-projection + clear colour
for the still-C SSAO / lighting / SDF uploads — the `rae_g2d_xform(out)`
pattern); `rae_gb_set_frame_open` (the metaball prep's "pass open" check);
and it BORROWS the four views + size (`rae_gb_commit_targets(w, h, a, b, c,
depth)` / `rae_gb_forget_targets`) for the pyramid's mip-0 read and the size
its lit / AO / TAA / pyramid targets are built to (`rae_gb_view_a/b/c/depth`,
`rae_gb_targets_gen` still read by the post passes). Removed from C:
`gb_a/b/c/depth_tex`, `gb_frame_ubuf`, `gb_draw_sbuf`, `gb_draw_count`,
`gb_enc`, `gb_pass`, `rae_gb_set_target / targets_match / targets_ready /
release_targets_ext / set_frame_ubuf / set_draws_buffer / frame_ubuf /
draws_buffer / frame_bytes / draws_size / max_draws / draw_count /
advance_draws / frame_uniform / set_frame / pass / encoder / clear_frame /
frame_active` and the gated `rae_ext_Gbuffer_drawCount` (allowlist −1, gate
13). `rae_ext_Gbuffer_shutdown` only forgets the borrowed views and resets
the frame math now. Still C, filed as follow-ups: the shadow maps
(`rae_sm_*`) + the SDF prepare (slice 3b) and the owner-teardown replacement
of `Gbuffer.shutdownAll()` + the legacy `lib/GpuTiming` fold + the lit / AO /
TAA / pyramid targets and their post-pass adoptions (slice 3c).
