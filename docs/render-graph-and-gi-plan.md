# Rae render architecture: render graph, HDR pipeline, and real-time GI

Architecture decision document. **No code is changed by this document.**
Implementation parameters remain experimentally adjustable; the
*architecture* below is decided.

Supersedes the earlier, more hesitant draft of this file. Repository
findings from that draft are preserved; the central recommendation is not.

## What is being committed to

| Committed (architecture) | Not yet committed (parameters) |
|---|---|
| A real render graph, from the start | Aliasing, async compute, multi-queue |
| HDR scene color; tonemapping as a pass | Exact tonemap curve, exposure model |
| Single-sampled readable depth + normals | Normal encoding, exact formats |
| **TAA** as the antialiasing strategy (MSAA is dropped) | Jitter sequence length, clamp heuristics |
| Real-time **dynamic** GI as a first-class capability | Accumulation algorithm details |
| A **unified dynamic GI scene representation** | Cascade count, resolution, memory budget |
| **SDF-based light transport as the leading direction** | Voxelization implementation, update cadence |
| GI representation decoupled from render geometry | Per-geometry-class proxy choices |

Baked lighting is **excluded as a primary architecture**. Rae's content
includes destructible environments, runtime topology changes, procedural
geometry and metaballs; a preprocess-once solution is invalidated by the
requirements. Baked data may later exist as an optional cache for content
that happens to be static, but nothing in the renderer may assume it.

---

## 1. Executive recommendation

**Build the render graph now. Build HDR, readable depth/normals and TAA
through it. Then build real-time GI on a unified dynamic scene
representation, with SDF-based light transport as the leading
implementation.**

The prior draft argued that three passes did not justify a graph. That is
the wrong horizon. The graph is not an optimization applied once a renderer
is large — it is the structure the large renderer is built *on*. Retrofitting
it after shadows, GI, SSAO, TAA, bloom and post exist means paying the
migration cost precisely when the renderer is hardest to change.

Two decisions unlock the rest, and they interact:

1. **TAA replaces MSAA.** MSAA 4× antialiases geometry edges but not
   shading, and — decisively — its depth attachment is unreadable, because
   WebGPU guarantees no depth resolve. Dropping MSAA makes depth
   single-sampled and sampleable, which is the input every screen-space and
   gathering technique needs. **The blocker dissolves as a consequence of
   the AA decision, not as separate work.**
2. **TAA's machinery is GI's machinery.** Jittered projection, motion
   vectors, a history buffer and reprojection are required by temporal
   accumulation of indirect lighting just as much as by antialiasing.
   Building TAA early is not "doing AA first" — it is building the temporal
   substrate GI depends on.

---

## 2. Current renderer architecture (repository findings)

Determined by reading source, not assumed.

### Composition today

Three passes already exist, hardcoded, ordered by invocation convention
across two C modules rather than by data:

```
1. 3D raster pass      runtime_gpu3d.c       MSAA 4x color + Depth32Float,
                                             resolves into g_g2d_off_view
2. SDF metaball pass   runtime_gpu3d_sdf.c   fragment raymarch, writes frag_depth
                                             into the same depth attachment
3. 2D UI overlay       runtime_gpu2d_frame.c LoadOp_Load over the resolved
                                             offscreen, then present/screenshot
```

That implicit pipeline is itself the argument for a graph: the ordering is
already load-bearing and already undocumented in data.

### Facts

| Aspect | State |
|---|---|
| GBuffer | **None** — single forward pass |
| Shadows | **None** |
| Depth | `Depth32Float`, **MSAA 4×, never sampled** |
| HDR | **No** — ACES tonemap inside the material shader → effectively LDR |
| Meshes | Fixed array `G3D_MAX_MESHES 256`; interleaved pos3/nrm3/uv2, u32 indices |
| Draw data | One storage buffer, `G3D_MAX_DRAWS 4096`, indexed by `instance_index` |
| Materials | 8 floats inline per draw; **no textures** |
| Skinning | **None** — vertex layout has no joints/weights |
| Motion vectors | **None** |
| Resource lifetime | Process-global `static WGPU*`; all-or-nothing teardown |
| Command encoding | Global `g3d_enc`; one encoder per frame |
| Compute | `gpu.kernel(wgsl, entry)` + `gpu.run(bufs, gx,gy,gz)` — **arbitrary WGSL compute available** |
| Path tracing | `rae_ext_webgpu_raytrace` — flat sphere list, **no BVH**, CPU readback |
| Scene model | `lib/scene3d.rae` — parallel arrays, `MeshRenderer` vs `SdfPrimitive` tagged split, **no change/revision signal** |

### Ray tracing availability — correcting a claim in circulation

Earlier notes in this repo (including QUEUE #326 as first written) implied
*"WebGPU has no ray tracing, so ray-based GI is unavailable."* **That is
wrong and must not propagate.**

| Technique | Available to Rae |
|---|---|
| Hardware RT APIs / driver BVH traversal | **No** — not in WebGPU core or browser |
| Ray tracing in **compute shaders** | **Yes** — arbitrary WGSL compute already ships |
| SDF ray marching | **Yes** — already shipping in `runtime_gpu3d_sdf.c` |
| Voxel / grid traversal in compute | **Yes** |
| Compute path tracing | **Yes** — already exists |

Unavailable is *fixed-function BVH traversal*, not ray-based light
transport. Rae pays traversal in ALU. For SDF tracing that cost is
inherently low, because sphere tracing skips empty space — which is a
genuine argument in SDF's favor on this platform, not a consolation.

---

## 3. Problems with the current pipeline

1. **Depth unreadable** (MSAA, no resolve) — blocks SSAO, GI gathering, TAA,
   SSR, volumetrics, depth-aware composition.
2. **No normals or velocity** downstream.
3. **Tonemapping inside the material shader** — no HDR buffer exists, so
   there is nowhere for GI, bloom or exposure to operate.
4. **Pass order implicit** across two C modules.
5. **All resources process-global** — no transients, resize, or device loss.
6. **Fixed caps with cliff behavior** (256 meshes / 4096 draws).
7. **Path tracer architecturally isolated** — own scene encoding, CPU
   readback, cannot contribute to the raster frame.
8. **No scene change signal** — `lib/scene3d.rae` cannot tell a consumer
   what moved, so dirty-region updates are impossible today.

---

## 4. The render graph

### It is a real graph, from v1

Not a renamed pass list. v1 provides:

- logical **resources** with descriptions
- **pass nodes** with explicit **read/write declarations**
- **dependency derivation** from those declarations
- **topological execution ordering**
- **validation** of invalid dependency structures
- **raster and compute** passes as peer node kinds
- a single command encoder where appropriate
- **resource ownership/lifetime boundaries** that can evolve

Deliberately **not in v1**: memory aliasing, transient pooling
optimization, async compute, multi-queue, dead-pass elimination beyond what
falls out naturally, engine-wide feature registries, Unreal-style framework
scaffolding.

The distinction that matters: v1 is architecturally a graph, so the omitted
items are *additions behind an unchanged declaration surface* rather than
redesigns.

### Proposed model (sketch — not committed API, not Rae syntax)

```
# PROPOSED. Illustrative only.
type ResourceId { id: Int, generation: Int }
type PassId     { id: Int }

type Lifetime {
  Transient    # created and discarded within one frame
  Persistent   # survives across frames (TAA history, SDF cascades, probes)
  External     # owned elsewhere (swapchain, gpu2d offscreen)
}

type Access {
  AttachmentWrite   # color/depth attachment
  AttachmentRead    # LoadOp_Load
  SampledRead       # texture binding
  StorageRead
  StorageWrite
  UniformRead
}

type ResourceDesc {
  name:     String
  kind:     ResourceKind   # Texture2D | Texture3D | Depth | StorageBuffer | UniformBuffer
  format:   TextureFormat
  sizing:   Sizing         # FullRes | HalfRes | QuarterRes | Fixed(w,h) | Volume(x,y,z)
  lifetime: Lifetime
}

type PassDesc {
  name:    String
  kind:    PassKind        # Raster | Compute
  reads:   List(ResourceUse)   # (ResourceId, Access)
  writes:  List(ResourceUse)
  execute: PassFn
}
```

**Three design points worth arguing for explicitly:**

1. **`Lifetime` is in v1 even though pooling is not.** Persistent resources
   are not an optimization detail — TAA history and SDF cascades are
   *semantically* cross-frame, and a graph that cannot express that will be
   wrong rather than merely unoptimized. Getting this into the vocabulary
   now costs nothing and prevents a later redesign.
2. **`Access` is recorded precisely from v1**, even though v1 only uses it
   for ordering. Barrier inference later needs intent, and retrofitting
   intent means revisiting every pass author.
3. **Sample count is deliberately absent.** With TAA adopted, everything is
   single-sampled. Re-adding MSAA later would mean re-adding it to the
   resource description — an intentional speed bump, since MSAA is what
   made depth unreadable.

### The graph must not know what GI is

The graph vocabulary is resources, access and ordering. It must never
contain the tokens `gi`, `ssao`, `shadow`, `sdf` or `taa`. A pass is "an
operation that reads these and writes those." This is exactly what lets
SSAO, SDF GI, voxel GI, compute path tracing, reflections, bloom, TAA and
volumetrics be added, reordered or removed without touching the scheduler.

Symmetrically: nothing above the GI pass may depend on SDFs. The GI
representation is queried through an interface (§7), not reached into.

---

## 5. Resource model

**Graph knows:** name, kind, format, sizing policy, lifetime class, and
per-pass access intent.

**Backend keeps:** `WGPUTexture`/`WGPUBuffer` handles, bind group layouts
and bind groups, pipelines and shader modules, the command encoder, queue
submission.

Anticipated resource set once the pipeline below exists:

| Resource | Format (proposed) | Sizing | Lifetime |
|---|---|---|---|
| `depth` | Depth32Float, **1 sample** | Full | Transient |
| `normal` | rgba8snorm or rg16f (octahedral) | Full | Transient |
| `velocity` | rg16f | Full | Transient |
| `hdrColor` | rgba16f | Full | Transient |
| `taaHistory` | rgba16f | Full | **Persistent** |
| `ao` | r8unorm | Half | Transient |
| `indirectDiffuse` | rgba16f | Half or Full | Transient |
| `sdfCascades` | r16f / r8snorm 3D | Volume | **Persistent** |
| `radianceCache` | rgba16f 3D or array | Volume | **Persistent** |
| `shadowMap` | Depth32Float | Fixed | Transient |
| `bloomChain` | rgba16f mips | Pyramid | Transient |
| `ldrColor` / swapchain | bgra8unorm | Full | **External** |

The Persistent column is the part a naive graph gets wrong.

---

## 6. Frame architecture

Proposed ordering with dependencies. Not assumed — derived from what each
stage consumes.

```
Scene update (transforms, topology changes, destruction events)
        │
        └── GI representation update  (dirty regions → cascades/proxies)
                    │
==================  RENDER GRAPH  ==================
                    │
  depthNormalPrepass ──────────► depth, normal, velocity
        │   (triangles AND metaballs both write here)
        ▼
  shadowPass ──────────────────► shadowMap
        │
        ▼
  giRepresentationUpdate (compute) ──► sdfCascades, radianceCache
        │
        ▼
  giTrace (compute) ───────────► indirectDiffuse
        │      reads depth, normal, sdfCascades, radianceCache
        ▼
  ssao (compute) ──────────────► aoRaw ──► ssaoBlur ──► ao
        │      reads depth, normal
        ▼
  forwardLighting ─────────────► hdrColor
        │      reads depth, normal, shadowMap, ao, indirectDiffuse
        ▼
  taaResolve (compute) ────────► hdrResolved  (+ writes taaHistory)
        │      reads hdrColor, taaHistory, velocity, depth
        ▼
  bloom ──► exposure ──► tonemap ──► ldrColor
        │
        ▼
  uiOverlay ──► present / screenshot
```

**A structural change worth calling out:** metaballs currently render as a
*separate shaded pass after* the raster pass. Under this architecture they
instead participate in the **prepass** — the raymarch writes `frag_depth`
and a normal — and are then shaded by the same forward lighting pass as
triangles. That unifies implicit and explicit geometry for shading, AO, GI
and TAA in one move, rather than needing each technique to special-case
them. It also means metaballs receive GI for free.

---

## 7. The unified dynamic GI scene representation

This is the core architectural addition, and the part that makes the GI
question tractable rather than a technique bake-off.

**Do not design "an SDF GI pass." Design a dynamic scene representation
that light-transport algorithms query.**

```
                        Scene (lib/scene3d.rae)
                                │
                    ┌───────────┴───────────┐
                    │                       │
            Render geometry          GI representation
         (what gets rasterised)    (what light transport queries)
                    │                       │
                    │        ┌──────────────┼──────────────┐
                    │        │              │              │
                    │   Analytic SDF   Voxel/clipmap   Object SDFs
                    │   (metaballs,      SDF from       (rigid, baked
                    │    primitives)    triangles       object-space,
                    │        │           + dirty         instanced)
                    │        │           regions             │
                    │        │              │           Proxies
                    │        │              │        (capsules for
                    │        │              │          skinned)
                    │        └──────────────┼──────────────┘
                    │                       │
                    │              Composite GI scene
                    │            (min() over distance;
                    │             radiance cache)
                    │                       │
                    └───────────┬───────────┘
                                ▼
                          GI queries:
                    distance(p) · trace(ray) · radiance(p, ω)
                                ▼
                        Indirect lighting
```

**The decisive property: render geometry ≠ GI representation.** Once that
separation exists, every hard geometry class becomes a *policy choice about
which provider it registers with*, not an architectural crisis. This is what
prevents skinned meshes from vetoing the design (§10).

### Provider composition

Providers are composited by `min()` over signed distance, plus a radiance
lookup. Each geometry class registers with whichever provider fits:

| Geometry class | Provider | Update cost |
|---|---|---|
| Metaballs / implicit | **Analytic** — exact, evaluated directly | **Zero** |
| Static environment | Voxel clipmap | Once, then only on change |
| Destructible | Voxel clipmap, **dirty region** | Bounded, local |
| Rigid dynamic | Per-object SDF baked object-space, instanced by transform | Transform only |
| Skinned characters | Capsule proxies, or low-res dynamic volume | Small per frame |
| Procedural | Analytic if expressible, else voxel | Varies |
| Anything missing | **Screen-space fallback** (§12) | Free (reuses depth/normal) |

### Radiance, not just distance

Distance answers occlusion; GI needs incoming *light*. Candidate mechanisms,
to be settled experimentally:

1. **Voxel radiance injection** — inject direct lighting into a radiance
   grid each frame, cone-trace it. Well-understood; couples naturally to the
   cascade structure already needed for distance.
2. **Trace-and-shade** — sphere-trace to a hit, evaluate lighting there.
   Sharper, more expensive, no radiance grid to maintain.
3. **Radiance cascades** — hierarchical probe cascades trading angular for
   spatial resolution with distance. Modern, designed for fully dynamic
   scenes with no precomputation, and a natural fit for SDF scenes. *Stated
   as a candidate; I have not validated a 3D implementation and its details
   should be researched before commitment.*
4. **Screen-space radiance reuse** — sample last frame's lit result for
   nearby bounces; cheap, and a useful component rather than a whole system.

All four sit behind the same `radiance(p, ω)` query, which is the point of
defining the interface first.

---

## 8. HDR pipeline

Foundational, not a later migration step.

```
material / direct lighting
        ↓
    HDR scene color   (rgba16f)
        ↓
  GI · AO · reflections composited in HDR
        ↓
  temporal accumulation (TAA)
        ↓
  bloom
        ↓
  exposure
        ↓
  tonemap (ACES)  ← moves OUT of the material shader
        ↓
  UI overlay (LDR)
        ↓
    output
```

The current in-shader ACES tonemap must be removed early. It is not merely
suboptimal: **GI accumulation on tonemapped values is mathematically wrong**
— light does not add after a nonlinear curve. Any indirect lighting built on
the current pipeline would be incorrect regardless of how good the GI
algorithm is. This is a prerequisite, not a polish item.

UI composites **after** tonemapping, in LDR, which matches the existing
gpu2d overlay behavior.

---

**Bloom as built (render-perf-8, `lib/GbufferBloom.rae`).** The `bloom`
graph pass runs between the anti-aliasing and the composite. It reads what
the composite will tone-map (the anti-aliased scene or the TAA result) and
builds a chain of up to 6 half-size levels (1/2 down to 1/64 of the scene),
in the lit format (rg11b10ufloat):

- **First level:** 13 taps with a Karis average against fireflies, then a
  soft-knee threshold on the EXPOSED brightness (`bloom_prefilter.wgsl`).
- **Next levels down:** 13 taps each (`bloom_down.wgsl`).
- **Back up:** each level's downsample plus a tent-filtered upsample of the
  level below, into separate textures, so no blend state is needed
  (`bloom_up.wgsl`).
- **Composite:** adds intensity x bloom to the HDR color before exposure
  and tone mapping.

Apps control it with `setBloom(renderer:, settings: BloomSettings { enabled,
threshold, knee, intensity })` or `setBloomEnabled(renderer:, on:)`.
`RAE_BLOOM=1` / `RAE_BLOOM=0` forces it on or off for comparisons. Off (the
default), the pass returns at once, nothing is allocated, and the
composite skips the sample.

- **Default threshold 1.0, not ~0.3.** With exposure 1.0 the scene units
  are the exposed units. A threshold of 0.3 is mid-gray after tone mapping,
  so it made the whole 112 frame (floor, sky) glow. At 1.0 only what is
  brighter than white glows. The defaults are knee 0.5 and intensity 0.2.
- **Per example:**
  - 112 turns bloom on: its specular highlights glow.
  - 114 leaves it off: looking toward the sun, the bright sky glows over
    the characters and the trees and lowers the contrast.
  - 119 leaves it off: its sun glints are tiny and only a little above
    white, so after the blur they add almost nothing (a mean of +0.2
    levels, even with threshold 0.8 and intensity 0.3). Bloom on the ocean
    needs the water shader to make its glints much brighter than white.
- **Cost** at a phone's scene size (`RAE_RENDER_CEILING=1600x720`, 112, on
  the Mac): frame 2.3 -> 2.8 ms; the bloom span is about 0.42 ms over the
  timing marks' own ~0.1 ms. That is 11 passes for 6 levels. On a phone,
  measure, and consider fewer levels.

## 9. Depth, normals, velocity, and TAA

**Decision: TAA. MSAA is dropped.**

Consequences, all of them wanted:

- Depth becomes **single-sampled and sampleable** — the blocker for SSAO,
  GI gathering, SSR and volumetrics disappears as a side effect.
- Shading is antialiased, not just geometric edges — which matters more for
  a PBR renderer with specular highlights than MSAA ever did.
- The **temporal substrate GI needs** (history, reprojection, jitter) gets
  built once and shared.

TAA requires three things Rae lacks:

1. **Jittered projection.** A sub-pixel offset per frame (Halton 2,3 is the
   usual choice) applied in `mat4Perspective`. Must be *removed* before
   reprojection maths.
2. **Motion vectors.** Per-pixel screen-space velocity, requiring
   **previous-frame transforms per entity**. `lib/scene3d.rae` must store
   the prior transform — a real, small change to the scene model.
3. **History buffer + neighborhood clamping.** Persistent `rgba16f`, with
   variance/neighborhood clipping to control ghosting.

Known costs, stated plainly: TAA ghosts on disocclusion and can soften the
image; it needs care with thin geometry and rapid motion. For a demo with
controlled camera paths this is a favorable trade, and the shared temporal
infrastructure justifies it independently.

**Normals** come from a real target, not depth reconstruction —
reconstruction is wrong exactly at the geometric edges where AO and GI
matter most.

---

**TAA as built (render-perf-10, `lib/deferred_taa.wgsl`).**

- **Resolve:** in YCoCg on Reinhard-compressed color.
- **History:** clipped toward the 3x3 neighborhood mean, inside a box of
  one standard deviation (variance clipping). It is fetched with a 5-tap
  Catmull-Rom filter, not the nearest texel.
- **Current-frame weight:** 0.05 when still, rising to 0.2 at 8 pixels of
  motion.
- **Reactive pixels** weigh at least 0.6. Two sources mark them:
  - the G-buffer writes `gba.z = 1` (this channel was unused) for geometry
    whose motion vectors miss part of its movement: the wind-blown grass
    (`grass_render.wgsl`) and the deforming metaballs (`gbuffer_sdf.wgsl`);
  - the TAA finds the pixels a transparent surface drew over this frame
    (the water) by comparing the lit image with the opaque snapshot,
    when one was taken (`targets.litCopyFresh`).
- **Override:** `RAE_TAA=1` / `RAE_TAA=0` forces TAA on or off over the
  app's choice.

Measured with 120 fixed-step frames while the camera turns 0.02 rad per
frame, against the same frame without TAA (pixels more than 24 levels of
luma off):

| example | old TAA | new TAA |
|---|---|---|
| 114 | 0.71% | 0.44% |
| 112 | 0.34% | 0.21% |
| 119 | 10.0% | 3.3% |

The old TAA left a stepped double edge behind 112's moving metaball and
smeared 119's water detail into streaks. The new one keeps a single edge
and the water's detail.

## 10. Difficult geometry classes (and why they do not veto anything)

The prior draft let skinned meshes weigh too heavily. The correct framing:
**the architecture must allow different GI representations for different
geometry classes**, and once it does, each class is a tunable policy.

Skinned characters, in increasing fidelity — all valid, all swappable:

1. **Receive-only** — lit by GI, contribute no occlusion or bounce.
2. **Capsule proxies** — a handful of capsules injected into the field;
   the established approach.
3. **Low-res dynamic object SDF** — e.g. 32³ per character, revoxelized in
   compute each frame. Fine for a few characters.
4. **Screen-space fallback** — screen traces cover what the SDF cannot see.

Note Rae has **no skinning at all today**, so this must not over-constrain
the design. The requirement on the architecture is only that the GI
representation is a separate, pluggable set of providers — which §7
guarantees.

Destructible geometry is the opposite case: it *favors* this architecture.
Destruction dirties a bounded region, and grid/clipmap structures support
local rebuild, whereas BVH refit degrades with topology change. Requirements
for the update path: no full rebuilds, bounded local updates, amortisable
across frames, graceful degradation while updating.

**Thin geometry remains a genuine limitation.** Voxel SDFs lose railings,
foliage and thin walls; they vanish or over-occlude. This is not fully
solvable at reasonable resolution and is the strongest honest argument
against pure SDF tracing. The mitigation is the screen-space fallback
component (§12), which sees exactly the thin detail the volume misses.

---

## 11. SSAO

SSAO is a contact-scale complement to GI, not a substitute and not the
architectural goal.

```
ssao:      reads depth, normal          writes aoRaw  (half-res)
ssaoBlur:  reads aoRaw, depth           writes ao     (edge-aware)
```

- **Short radius by design** — permanent role alongside world-scale GI.
  Building a large-radius SSAO now creates something GI obsoletes.
- **Half-res + bilateral upsample.**
- **Applied to the indirect/ambient term only** — never to direct light. A
  surface in direct sun inside a crevice is still lit. This is both more
  correct and the mechanism that prevents double-darkening once GI lands.
- **Blue-noise / interleaved-gradient kernel rotation** — first real
  consumer of the queued noise library (#320).
- Temporal filtering comes free once TAA exists.

**As built (render-perf-6, `lib/GbufferAo.rae`).** The `ssao` graph step
runs four fullscreen passes at half the scene size:

1. `deferred_ao_depth.wgsl` makes a half-size depth. Each texel keeps one
   of its 2x2 full-size depths, in a checkerboard: the nearest in even
   cells, the farthest in odd cells. It also stores which of the four it
   kept, so the SSAO reads that texel's normal and its true position.
2. `deferred_ao.wgsl` marches 3 slices x 6 steps over that depth. The slice
   rotation follows a 4x4 Bayer pattern (16 rotations per 4x4 block).
3. `deferred_ao_blur.wgsl` blurs along x, then along y: 5 taps, weighted by
   relative depth difference.
4. Lighting upsamples with the 4 half texels around each pixel (bilinear
   weights times depth similarity). Before, it used a 3x3 filter at full
   size: 9 AO and 9 depth loads per pixel.

Headless at 2560x1600 (scale 1.0), frame time: 114 5.2 -> 4.3 ms, 112
8.7 -> 7.3 ms. The mean brightness of the contact regions (114: feet and
box bases, 112: an inside corner and the floor under the torus and the
sphere) changed by 0.1% or less, and the AO is smoother: the full-size
version showed its noise pattern.

---

## 12. SSGI's role — decided

**Screen-space tracing is not a stepping stone and not a separate phase. It
is a fallback component inside the GI pass.**

Rejected role: "SSGI first because it is easy, SDF GI later." That builds a
system to throw away.

Accepted role: the GI pass consults the volumetric representation, and uses
**screen-space traces where the representation has no data** — thin
geometry below voxel resolution, skinned characters not in the field,
fine detail. This is the hybrid most modern systems converge on, and it
directly mitigates §10's two admitted weaknesses.

It is also useful for **development and validation**: screen-space results
are cheap ground truth for "is the gather direction sane" while the volume
pipeline is being built.

---

## 13. The existing compute path tracer

Currently isolated: flat sphere list, own encoding, CPU readback.

Under this architecture it gains two real roles:

1. **Reference implementation.** Render the same scene at offline quality
   and diff against the realtime GI. Having ground truth in-engine is
   unusually valuable when building GI, and Rae already has it — most
   projects do not.
2. **A graph pass like any other**, writing an HDR resource instead of
   reading back to CPU.

This argues for unifying its scene encoding with `lib/scene3d.rae` so both
consume one scene. It is also a plausible "beauty mode" for static demo
shots where frame time is not a constraint.

---

## 14. Why SDF is the leading direction for Rae specifically

Not general enthusiasm — three properties of *this* project:

1. **Metaballs are already an SDF.** `runtime_gpu3d_sdf.c` evaluates the
   field analytically. SDF-based light transport represents that geometry
   **exactly**, with no voxelization, no resolution loss and no update cost.
   Any triangle-oriented acceleration structure must approximate it. If
   implicit geometry is part of Rae's identity, this is decisive.
2. **Destruction favors grids over trees.** Local re-voxelization of a
   dirty region is bounded and parallel; BVH refit degrades under topology
   change.
3. **Sphere tracing suits a platform without hardware BVH.** Empty space is
   skipped in a few large steps. The absence of fixed-function traversal
   penalises BVH approaches far more than it penalises SDF marching.

Against, honestly: thin geometry (§10), skinned meshes needing proxies
(§10), cascade memory on the WASM target (§15), and temporal stability
requiring accumulation (which TAA now provides).

---

## 15. WebGPU considerations

- **No depth resolve** → resolved by dropping MSAA (§9).
- **No read-write storage textures** in base WGSL → ping-pong pairs;
  declare both resources.
- **Narrow storage-texture format support** → validate at declaration.
- **No hardware RT, no guaranteed subgroups, no async compute** in core.
- **Binding limits** are low on some backends; per-pass bind group
  allocation must respect them.
- **WASM/browser is a hard target** (#321) — the tightest constraint on
  cascade resolution and persistent volume memory. A 128³ R8 cascade is
  2 MB (≈8 MB for four); 256³ is 16 MB each and likely out of budget.
  **Memory ceiling must be fixed before cascade parameters are chosen.**
- **Timestamp queries optional** → per-pass timing needs a fallback.
- **`Float` is f32** and matches WGSL natively (see `primitive-types.md`) —
  CPU-side scene data uploads without conversion.

---

## 16. Migration strategy

Each step independently verifiable; step 2 is the safety mechanism.

1. **Build the render graph** (§4 v1 scope).
2. **Port the three existing passes into it, with no visual change.**
   Acceptance test: **byte-identical headless screenshots**. This makes the
   refactor provable and any later regression attributable.
3. **Single-sampled depth + normal + velocity prepass**; drop MSAA.
   Metaballs move into the prepass (§6).
4. **HDR scene color; move tonemapping out of the material shader** into
   its own pass.
5. **TAA** — jitter, motion vectors, history, clamping.
6. **SSAO** as the first new lighting technique.
7. **GI scene representation infrastructure** (§7) — providers, composite,
   dirty regions, query interface. No GI algorithm yet.
8. **First SDF GI prototype** against that interface.
9. **Validate against the path tracer** (§13).
10. **Dynamic/destructible update paths**; proxies for dynamic classes as
    those renderer features arrive.
11. **Only then** graph optimization — transient pooling, aliasing, barrier
    inference — driven by measurement.

Prerequisite outside the renderer: `lib/scene3d.rae` needs a
**change/revision signal** and **previous-frame transforms** (steps 5 and 7
both require it).

---

## 17. What should NOT be built yet

- Render-graph aliasing, transient pooling, async compute, multi-queue.
- Baked GI as a primary path (excluded by requirements).
- Skinning, purely to make GI decisions easier.
- Standalone SSGI as a separate phase (§12 — it is a component).
- Unreal-style feature registries, material graphs, or a plugin framework.
- Final cascade resolutions, probe densities or accumulation constants —
  these are experimental parameters, not architecture.
- Any new Rae language syntax. Nothing here requires it.

---

## 18. Open questions requiring approval

Reduced from the prior draft — TAA and the graph are now decided.

1. **Prepass vs full GBuffer.** A depth/normal/velocity prepass keeps
   forward shading and costs an extra geometry pass; a GBuffer enables many
   lights and cheaper deferred composition. *Recommendation: prepass first;
   the GI representation matters more than light count for the target
   content.*
2. **WASM memory ceiling for persistent volumes** — bounds cascade
   resolution and count before any GI code is written.
3. **Is destructibility a near-term requirement or a direction?** It does
   not change the architecture (§7 handles both) but it changes step 10's
   priority.
4. **Should the path tracer's scene encoding be unified** with
   `lib/scene3d.rae` now, or when step 9 needs it?
5. **Radiance mechanism** (§7) — voxel injection vs trace-and-shade vs
   radiance cascades. Deliberately deferred to measurement; the query
   interface is what is being committed to.

---

## 19. Answers to the framing questions

1. **What render architecture is Rae adopting?** A render graph with
   declared resources and derived ordering, an HDR pipeline, TAA, and
   real-time dynamic GI over a unified scene representation.
2. **Role of the render graph?** Structure and ordering, generic over
   technique. It never knows what GI is.
3. **What resources does it manage?** Textures, depth, storage textures,
   storage/uniform buffers, 3D volumes, external targets — each with a
   lifetime class (Transient / Persistent / External).
4. **Raster and compute?** Peer node kinds recording into one encoder.
   Fullscreen-triangle raster stays available where it beats compute.
5. **HDR pipeline?** rgba16f scene color; GI/AO/bloom in HDR; exposure and
   tonemap as passes; UI composited in LDR after tonemap.
6. **Depth/normal strategy?** Single-sampled prepass producing depth,
   normal and velocity; MSAA dropped in favor of TAA; metaballs
   participate in the prepass.
7. **Intended GI architecture?** Real-time dynamic GI over a composite
   scene representation queried via `distance` / `trace` / `radiance`, with
   SDF-based transport leading and a screen-space fallback.
8. **Why SDF for Rae?** Metaballs are natively SDF; destruction favors
   local grid updates; sphere tracing suits a platform without hardware BVH.
9. **How do geometry classes coexist?** As providers composited by `min()`
   over distance (§7), each class registering with whichever provider fits.
10. **Hard classes without infecting the architecture?** Because render
    geometry ≠ GI representation, they are policy choices — receive-only,
    proxies, low-res dynamic volumes, or screen-space fallback.
11. **SSAO's role?** Contact-scale AO on the indirect term only.
12. **SSGI's role?** A fallback component inside the GI pass for geometry
    the volume cannot represent — not a phase.
13. **Path tracer's role?** Reference/validation ground truth, and a normal
    graph pass.
14. **Implement first?** The render graph, then the port with screenshot
    equality.
15. **Not yet?** §17.

---

## 20. Recommended next implementation steps

1. **Render graph v1** — resources, descriptions, pass nodes, read/write
   declarations, dependency derivation, topological ordering, validation,
   raster + compute nodes, lifetime classes.
2. **Port the existing three passes**, byte-identical screenshots as the
   acceptance test.
3. **Prepass**: single-sampled depth + normal + velocity; drop MSAA; move
   metaballs into it.
4. **HDR + tonemap pass**; remove tonemapping from the material shader.
5. **TAA**; add previous-frame transforms to `lib/scene3d.rae`.
6. **SSAO.**
7. **GI representation infrastructure**, then the first SDF GI prototype,
   validated against the path tracer.

The architecture is decided. The parameters are not, and should be settled
by measurement against a real scene.
