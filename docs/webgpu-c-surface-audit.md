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
