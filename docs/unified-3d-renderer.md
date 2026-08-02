# Unified 3D renderer — evaluation and architecture

Three 3D renderer prototypes were authored independently (one per LLM) and
landed as examples 107, 108 and 109. This document records the evaluation,
the unification decision, and what is deliberately *not* unified.

## Evaluation

Scores weigh suitability as the foundation of the long-term renderer, not
just current visual output.

| Example | Author | Approach | Score |
|---|---|---|---:|
| 107 `gpu3d_raymarch` | GPT-5.5 | WGSL compute raymarcher, CPU framebuffer round-trip | **6.6** |
| 108 `gpu3d_procedural` | GPT-5.6-Sol | WGSL compute raymarcher, ECS-extract data model | **7.35** |
| 109 `gpu3d_pbr` | Claude Fable | Native render pass, hardware raster PBR | **7.9** |

Two independent reviews scored these 6.8/7.4/7.9 and 6.4/7.3/7.9; the table
above is the reconciled midpoint. Both agreed on the ordering and on 109 as
the base, so the ranking is robust to the disagreement.

**107 — 6.6.** The most complete *demo*: full fly camera, quality tiers, PNG
capture, animated 3D FBM, soft shadows, Fresnel spec. Its separate
camera/object/material buffers are the best packing of the two raymarchers —
static data is not re-uploaded every frame. Architecturally capped, though:
each frame does `gpu.run` → `downloadU32` → `updatePixels`, a full
GPU→CPU→GPU pixel round-trip, and it rebuilds the whole pipeline on quality
change. Objects are untyped float lists addressed by magic index.

**108 — 7.35.** The best-engineered raymarcher and the best *data model* of
all three: `Vec3`-typed components, real entity indirection
(`renderable.entity → transforms[entity]`), one unified `extractScene`
upload path, correct GGX + Smith + Schlick, SDF AO, deterministic 2×2
supersampling, torus primitive. Its comments explicitly anticipate a future
raster extractor and SSAO — designed to be superseded gracefully. Same
CPU-round-trip ceiling as 107, and it re-uploads the entire scene every
frame even when only the camera moved.

**109 — 7.9.** The only prototype on an architecture worth shipping for a
realtime demo: a real WebGPU render pass with Depth32Float + MSAA 4×,
GPU-resident triangle meshes, per-draw storage buffer indexed via
`instance_index`/`firstInstance`, resolving into gpu2d's persistent
offscreen so present, `RAE_GPU2D_SCREENSHOT` and the planned 2D HUD overlay
come for free. Correct Cook-Torrance + hemisphere ambient + ACES. Its
weaknesses were all API/runtime hygiene rather than architecture — and it
violated the project's Z-up mandate (see below).

## The unification decision

**Total unification is impossible at exactly one layer: geometry.** Raster
needs explicit triangle meshes; raymarch needs implicit SDF primitives plus
procedural displacement. Forcing those into one component would produce a
lowest-common-denominator API that serves neither.

Everything *above* geometry unifies cleanly. So the architecture is:

```
                 lib/scene3d.rae          (backend-agnostic)
   Transform3d · Material3d · Camera3d · Light3d · typed handles
                        |
        +---------------+---------------+
        |                               |
  MeshRenderer                    SdfPrimitive     <-- tagged split
        |                               |
  lib/gpu3d.rae                  raymarch backend
  (raster: render pass,          (fullscreen pass per
   depth, PBR; single-           metaball CLUSTER over
   sampled since #333)            SDF data)
        |                               |
        +---------------+---------------+
                        |
        gpu2d offscreen -> present / screenshot / HUD
```

A scene is authored once; each backend renders the component kind it
understands and ignores the other. That is the honest edge of unification.

**SDF clusters.** `SdfPrimitive` carries a `cluster` id and a `smoothing`
value. Only primitives sharing a cluster fuse; different clusters are
independent surfaces that merely occlude each other, so one scene can hold
several blobs with different looks. `smoothing` is the cluster's
stickiness — the distance over which balls merge — read from the cluster's
first primitive, since it describes the group.

Albedo is **per primitive**: the weight that fuses two distances also mixes
their colours, so a ball's colour bleeds exactly across the region where
its surface merges and a cluster can hold a gradient. Metallic, roughness
and emission come from the cluster's first primitive, because those shade
the fused surface as a whole rather than any one ball.

## Decisions where the reviews disagreed

**Z-up vs Y-up — Z-up wins.** `docs/coordinate-system.md` is a project-wide
mandate: right-handed Z-up Blender conventions, "New 3D code MUST use Z-up".
109 was Y-up and was therefore the violator; 107/108 (`worldUp = (0,0,1)`)
were already correct. The recommendation to standardize on Y-up "because it
matches WebGPU clip conventions" was wrong: only the projection matrix ever
touches clip space, so the host API's up-axis preference never needs to
reach world space. math3d and mesh3d were converted.

**Typed handles vs raw `Int` — typed wins.** A bare `Int` mesh id can be
passed where a material id belongs and silently renders garbage.
`MeshHandle`/`MaterialHandle` carry a `generation` so a stale handle to a
recycled slot is detectable rather than aliasing whatever landed there next.

**World-oriented API vs per-entity draw calls — world-oriented wins.**
Apps call `beginScene` + `renderScene`, not a hand-rolled loop of
`drawMesh` with nine positional floats. The flat C ABI stays as an internal
encoding detail.

**`World3d` with `ComponentTable` + full extraction/culling/frame-graph vs
incremental seam — incremental first.** The richer design (dirty revisions,
frustum culling, sort/batch, immutable frame data, staged frame graph) is
the right destination and is queued, but landing it in one step would gate
the demo on a large refactor. Parallel component arrays with an entity index
give the same authoring shape and can grow into `ComponentTable` without
changing app code.

## Known limitations (all queued, none silently ignored)

- **Matrices heap-allocate.** Every `mat4*` returns a fresh `List(Float)`,
  so a frame allocates many transient matrices. Rae has no fixed-size array
  type; the practical fix is a `Mat4` value type of four inline `Vec4`
  columns.
- **Normals use the model matrix directly**, so non-uniform scale skews
  lighting. Needs the inverse-transpose normal matrix.
- **Tonemapping happens inside the material shader**, making output
  effectively LDR. Needs an `rgba16f` HDR target and a separate tonemap pass
  before bloom/post can exist.
- **WGSL and PBR policy live in C** (`runtime_gpu3d.c`). The C layer should
  shrink to raw WebGPU resource/pass operations with shader and render-graph
  decisions in Rae.
- **Raymarchers still round-trip through the CPU.** Porting them to render
  into a GPU texture and blitting into the gpu2d offscreen removes
  `downloadU32`/`updatePixels` and unifies present + screenshot + HUD.
- **GPU3D depends on GPU2D's private globals**; these should become an
  owned renderer/device context. Device loss, resize and partial resource
  teardown are unhandled.
- **Hybrid SDF/mesh depth** (SDF objects occluding meshes) needs proxy
  volumes writing `frag_depth`; must be validated across Metal/Vulkan/D3D12
  and browser WebGPU.
- **SSAO with MSAA** needs an explicit depth/normal strategy — WebGPU depth
  resolve cannot be assumed.
- **No WASM path yet.** This remains the biggest deployment unknown for the
  Assembly entry; the render-pass backend is the standard browser-3D shape,
  so betting on raster de-risks the port.
- **`G3D_MAX_DRAWS` is 4096.** The fixed per-draw storage buffer accepts at
  most 4096 mesh submissions per frame. The renderer discards submissions
  beyond that capacity and emits one renderer-lifetime error rather than
  silently dropping geometry. `RAE_GPU3D_DRAW_LIMIT=N` may lower the effective
  limit for diagnostics and regression tests, but cannot raise the hard cap.

## Keep-list per prototype

- **107**: split static/per-frame buffer update policy; animated 3D noise
  and SDF displacement; fly camera.
- **108**: the component/extraction data model (adopted as `scene3d`); GGX
  implementation; torus; deterministic supersampling; the explicit
  geometry-AO vs SSAO distinction.
- **109**: raster pipeline, depth, MSAA, direct GPU presentation; Rae-side
  mesh generation and matrix math; the material-study fixture as a
  regression screenshot; orbit camera.

Retain 108's shader as the raymarch feature base rather than merging both
raymarch hosts, but adopt 107's split-buffer update policy.

## Example roles (kept separate on purpose)

Three examples, one renderer library — not one oversized example:
`107_gpu3d_raymarch` (SDF/procedural features), `108_gpu3d_procedural`
(data-oriented procedural scene), `109_gpu3d_pbr` (minimal raster
correctness + material study).
