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

