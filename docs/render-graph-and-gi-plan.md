# Render pipeline architecture and GI plan

Design proposal. **Nothing here is implemented.** The purpose is to decide
what the next stage of Rae's realtime 3D renderer should be, before writing
code. Everything marked *proposed* is a sketch for review, not committed
syntax or API.

Scope note: this document assumes **genuinely dynamic scenes**. Rae's target
content includes destructible environments, runtime-changing geometry,
animated/skinned objects, metaballs and procedural geometry. Baked lighting
is therefore explicitly **not** a candidate for the primary architecture. It
may appear later as an optional accelerator for content that happens to be
static, but no part of the renderer may assume a preprocess-once scene.

---

## 1. Executive recommendation

**Build a minimal render-pass abstraction plus a depth/normal prepass. Do
not build a full render graph yet, and do not commit to SDF GI yet.**

The reasoning is specific to what the repo actually contains:

1. **There is no GBuffer and no readable depth.** `runtime_gpu3d.c` renders
   a single forward pass into a 4× MSAA colour target with a
   `Depth32Float` attachment, and nothing samples that depth. WebGPU does
   not guarantee depth resolve, so an MSAA depth attachment is not
   readable as a texture.
2. **Every technique under discussion needs exactly that missing input.**
   SSAO needs depth + normals. SDF GI needs depth + normals to know where
   to gather. SSGI needs them. TAA needs depth + motion vectors. The single
   highest-leverage piece of work is not any of those techniques — it is
   the resource they all consume.
3. **The pass count is currently three**, and they are ordered by
   convention across two C files rather than by data. A full render graph
   (resource aliasing, lifetime analysis, automatic barrier inference) is
   not yet justified. A pass *abstraction* with declared reads/writes is,
   because the moment SSAO exists the ordering stops being obvious.

So the recommended next implementation step is: **depth/normal prepass +
minimal pass abstraction → SSAO as the first client of both.** SDF GI is a
larger decision that this document scopes but does not settle.

---

## 2. Current renderer architecture

Determined by reading the source, not assumed.

### Composition

There are already **three hardcoded passes**, which is the strongest
argument that an abstraction is due:

```
1. 3D raster pass        runtime_gpu3d.c        MSAA 4x colour + Depth32Float,
                                                resolves into g_g2d_off_view
2. SDF metaball pass     runtime_gpu3d_sdf.c    fragment raymarch, writes frag_depth
                                                into the same depth attachment
3. 2D UI overlay         runtime_gpu2d_frame.c  LoadOp_Load over the resolved
                                                offscreen, then present/screenshot
```

Ordering is expressed as an *invocation convention* — the app calls
`beginScene` → `renderScene` → `submit()` → the 2D path — enforced by
comments and by which module is `#include`d first in `rae_runtime.c`. There
is no data structure describing it.

### Key facts

| Aspect | State |
|---|---|
| GBuffer | **None.** Single forward pass; `grep -c gbuffer` → 0 |
| Shadows | **None** |
| Depth | `Depth32Float`, MSAA 4×, write-only — never sampled |
| Colour | MSAA 4× resolving into gpu2d's persistent offscreen |
| HDR | **No.** ACES tonemap happens inside the material shader → effectively LDR |
| Meshes | Fixed array, `G3D_MAX_MESHES 256`; interleaved pos3/nrm3/uv2, u32 indices |
| Draw data | One storage buffer, `G3D_MAX_DRAWS 4096`, indexed by `instance_index` |
| Materials | 8 floats per draw inline (baseColor+metallic, emissive+roughness). No texture support |
| Skinning | **None.** Vertex layout has no joints/weights |
| Resource lifetime | Process-global `static WGPU*` handles; teardown is all-or-nothing |
| Command encoding | Global `g3d_enc` static; one encoder per frame, begin/end inline |
| Compute | `gpu.kernel(wgsl, entry)` + `gpu.run(bufs, gx, gy, gz)` — **arbitrary WGSL compute is already available** |
| Path tracing | `rae_ext_webgpu_raytrace` — flat sphere list (19-float header + 10 floats/sphere), **no BVH**, CPU readback |
| Scene model | `lib/scene3d.rae` — parallel component arrays, `MeshRenderer` vs `SdfPrimitive` tagged split |

### Correcting an earlier claim in this repo's discussion

Earlier notes (including QUEUE #326 as first written) said or implied *"WebGPU
has no ray tracing, so ray-based GI is off the table."* **That framing is
wrong and should not propagate.** The correct distinction:

| Technique | Available to Rae today? |
|---|---|
| Hardware RT APIs / driver BVH traversal | **No** — not in WebGPU core, not in browser |
| Ray tracing in **compute shaders** | **Yes** — `gpu.kernel`/`gpu.run` run arbitrary WGSL compute |
| SDF ray marching | **Yes** — already shipping in `runtime_gpu3d_sdf.c` |
| Voxel / grid traversal in compute | **Yes** — same compute seam |
| Compute path tracing | **Yes** — `rae_ext_webgpu_raytrace` exists |

What is unavailable is *hardware-accelerated BVH traversal*, not ray-based
light transport. Rae can trace rays; it just pays for traversal in ALU
rather than in fixed-function silicon. That materially changes the GI
options and rules **in** several techniques a "no RT" reading would exclude.

---

## 3. Problems with the current pipeline

1. **Depth is unreadable.** MSAA depth cannot be sampled and WebGPU has no
   guaranteed depth-resolve. This single fact blocks SSAO, SSGI, SDF GI
   gathering, TAA, motion blur, DOF, and depth-aware compositing.
2. **No normals available downstream.** Normals exist only inside the
   fragment shader. Any technique needing world normals must either
   reconstruct them from depth (cheap, but wrong at edges) or get a real
   normal target.
3. **Tonemapping is inside the material shader**, so there is no HDR buffer
   to run bloom/GI accumulation/exposure against. Composition order is
   effectively frozen.
4. **Pass order is implicit.** Adding a pass between the 3D and 2D passes
   means editing C in two modules and reasoning about which `#include`
   order applies.
5. **Resource lifetime is global and permanent.** Every target is a
   process-lifetime static. No transient allocation, no resize, no device
   loss handling.
6. **Fixed caps with cliff behaviour** — 256 meshes, 4096 draws.
7. **The path tracer is architecturally separate** from the raster path: its
   own extern, its own scene encoding (flat spheres), CPU readback. It
   cannot currently contribute to the raster frame.

---

## 4. Proposed render-pass architecture

### Recommendation: minimal pass abstraction, not a graph

The distinction that matters:

- A **pass abstraction** gives named passes with declared reads/writes and
  a validated execution order. Cost: small. Value now: high.
- A **render graph** adds transient resource allocation, memory aliasing,
  automatic barrier/transition inference and dead-pass culling. Cost:
  substantial. Value now: low at three-to-six passes.

Build the first. Design it so the second can be added later *without
changing pass authors' code* — that is the real requirement, and it is met
by making passes declare resources rather than acquire them.

### Proposed model (sketch, not committed API)

```
# PROPOSED — not implemented, not Rae syntax to rely on.
type PassId    { id: Int }
type ResourceId { id: Int, generation: Int }

type ResourceDesc {
  name:   String
  kind:   ResourceKind     # ColorTarget | DepthTarget | StorageTexture
                           # | SampledTexture | StorageBuffer | UniformBuffer
  format: TextureFormat
  sizing: Sizing           # FullRes | HalfRes | Fixed(w,h)
  samples: Int             # 1 or 4 — MSAA is a property of the resource
}

type PassDesc {
  name:    String
  kind:    PassKind        # Raster | Compute
  reads:   List(ResourceId)
  writes:  List(ResourceId)
  execute: PassFn          # receives resolved resources; records commands
}
```

Properties this gives immediately:

- **Explicit dependencies.** A pass that reads `depth` cannot be scheduled
  before the pass that writes it; the ordering is derived, not asserted.
- **Validation.** Reading a resource nobody writes, or writing one twice in
  a frame, is a startup error rather than a garbage frame.
- **Insertable techniques.** Adding SSAO means adding a node, not editing
  the frame function.
- **Debuggability.** The graph can print itself; per-pass GPU timing gets a
  natural home.

Deliberately **excluded from v1**: memory aliasing, transient pooling,
automatic barrier inference, async compute, multi-queue, sub-passes,
conditional/dead-pass culling. Each is a real feature and each can be added
behind the same declaration surface.

### Architectural principle: the graph must not know what GI is

The graph knows about *resources and ordering*. It must never contain the
words `ssao`, `gi`, `shadow` or `sdf`. A pass is "a thing that reads these
and writes those." This is what allows SSAO, SDF GI, voxel GI, compute path
tracing, reflections, bloom, TAA and volumetrics to be added or removed
without touching the scheduler.

Equally: the high-level renderer must not gain a dependency on SDFs just
because SDF GI is under investigation. SDF tracing, if adopted, is a pass
that reads an SDF resource — nothing above it should know.

---

## 5. Resource dependency model

What the graph must know, versus what stays in the backend:

**Graph knows:** logical name; kind (colour/depth/storage-texture/sampled/
storage-buffer/uniform); format; sizing policy (full/half/fixed); sample
count; per-pass read/write intent.

**Backend keeps:** actual `WGPUTexture`/`WGPUBuffer` handles, bind group
layouts and bind groups, pipeline objects and shader modules, the command
encoder, and queue submission.

Read/write intent is the minimum needed to derive both ordering *and* — in a
later version — barriers. It is worth recording intent precisely now
(`SampledRead` vs `StorageRead` vs `AttachmentWrite`) even though v1 only
uses it for ordering, because retrofitting intent later means revisiting
every pass.

**MSAA belongs to the resource, not the pass.** This is the lesson from the
current depth problem: the sample count of the depth target is precisely
what makes it unreadable, and that fact must be visible in the declaration
so validation can reject "SSAO reads a 4×-MSAA depth target" at startup.

---

## 6. Compute and raster integration

Both are graph nodes; only the `execute` body differs (begin a render pass
vs begin a compute pass). This is straightforward in WebGPU because both
record into the same `WGPUCommandEncoder`, which Rae already creates once
per frame.

The expected split:

| Pass | Kind |
|---|---|
| Depth/normal prepass, forward/GBuffer, SDF metaball raymarch, UI overlay | Raster |
| SSAO + its blur, SDF/voxel tracing, path tracing, bloom, TAA resolve, tonemap | Compute (or fullscreen raster where simpler) |

Two practical notes:

- **Fullscreen-triangle raster is often the better choice over compute** for
  simple full-frame operations in WebGPU: no storage-texture format
  restrictions, and blending is free. The abstraction should not push
  authors toward compute for its own sake.
- **Storage textures have format constraints** in WebGPU (notably around
  `rgba8unorm` write support and no read-write in the base spec). This must
  be validated at declaration time, not discovered mid-frame.

---

## 7. Dynamic scene representation

The requirement is that the renderer never assumes preprocess-once content.
The categories, and what each needs from a lighting-relevant representation:

| Category | Transform | Topology | Suitable structure |
|---|---|---|---|
| Static environment | fixed | fixed | Any. Bake **as a cache**, never as an assumption |
| Rigid dynamic (crates, vehicles) | per frame | fixed | Per-object structure in object space, instanced by transform. Cheap |
| Skinned characters | per frame | deforming | Hardest. Proxy, or low-res per-frame revoxelization |
| Destructible | per frame | **changing** | Needs *local* rebuild. Favours grids over trees |
| Procedural | varies | varies | Depends on generator; may be analytic |
| **Metaballs / implicit** | per frame | analytic | **Already an SDF.** No conversion needed at all |

Two observations that should drive the choice:

**Destructibility favours grids/SDFs over BVHs.** When a wall is blown open,
a voxel or clipmap SDF needs only the affected region re-voxelized — a
bounded, local, parallel update. A BVH needs a refit that degrades with
topology change, or a rebuild. This is a genuine architectural argument, not
a preference.

**Metaballs make SDF representation strictly more attractive.** Rae's
implicit geometry *is* a signed distance field already — `runtime_gpu3d_sdf.c`
evaluates it analytically. For that content there is no voxelization step,
no resolution loss and no update cost: the GI tracer can evaluate the exact
field. Any triangle-only lighting representation would have to approximate
metaballs; an SDF-based one represents them natively. If Rae's identity
includes implicit geometry, this is a strong pull toward SDF tracing.

---

## 8. SSAO integration

SSAO should be an ordinary pass, not a hardcoded renderer step.

```
# PROPOSED
depthPrepass:  writes depth(1x), normals(rgba8/rg16f)
ssao:          reads  depth, normals        writes aoRaw   (half-res)
ssaoBlur:      reads  aoRaw, depth          writes ao      (edge-aware)
lighting:      reads  gbuffer, ao, gi, ...  writes hdrColor
```

Design decisions, with reasons:

- **Inputs.** A real normal target, not depth-reconstructed normals.
  Reconstruction is cheap but wrong exactly at the geometric edges where AO
  matters most.
- **Resolution.** Half-res AO with an edge-aware (bilateral, depth-guided)
  upsample. Full-res is affordable at demo scene complexity but half-res
  plus a good blur usually looks better per millisecond.
- **Radius: short, by design.** Contact scale only. This is not a
  compromise — it is SSAO's permanent role in a renderer that also has
  world-space GI. Building a large-radius SSAO now would create something
  SDF GI later obsoletes.
- **Application point: the indirect/ambient term only.** AO must not
  multiply direct lighting; a surface in direct sunlight inside a crevice is
  still lit. Applying AO to ambient/indirect only is both more correct and
  the mechanism that prevents double-darkening when GI arrives.
- **Temporal filtering: not in v1.** It needs motion vectors and history,
  i.e. TAA infrastructure. Interleaved-gradient/blue-noise rotation plus the
  bilateral blur is enough to start. (Kernel rotation is the first real
  consumer of the queued noise library, #320.)
- **The MSAA problem must be solved first.** SSAO cannot read the current
  4× depth attachment. This is why the prepass is a prerequisite, not a
  nicety.

---

## 9. SDF GI evaluation

Evaluated honestly, including the parts that do not work.

### Representation options

1. **Analytic only** — trace the metaball/primitive field directly. Exact,
   zero build cost, but only covers implicit geometry.
2. **Global voxel SDF (clipmap/cascades)** — camera-centred cascades, coarser
   with distance. Handles arbitrary geometry; fixed memory; standard choice
   (this is broadly the Godot SDFGI shape).
3. **Per-object SDFs + global composite** — bake a distance field per mesh
   asset in object space, instance by transform, composite into a global
   field (broadly the Unreal DFAO shape). Excellent for rigid dynamics,
   poor for deformation.
4. **Hybrid: analytic implicit + voxel triangles.** Evaluate metaballs
   analytically and sample a voxel field for triangle geometry, `min()` the
   two. **This is the option that best fits Rae specifically**, because it
   keeps implicit geometry exact and only pays voxelization cost for meshes.

### Construction and update

Voxelization of triangles is a compute job Rae can already express. The
viable dynamic strategy is *partial* update: re-voxelize only regions whose
contents changed this frame, amortising cascades (near cascade every frame,
far cascades round-robin). Destruction fits this model naturally — it dirties
a bounded region.

### Cost and quality concerns, stated plainly

- **Memory.** A 128³ R8 cascade is 2 MB; four cascades ~8 MB. Tolerable.
  256³ is 16 MB per cascade and starts to hurt, especially for WASM.
- **Resolution vs thin geometry.** Voxel SDFs lose thin walls, railings and
  foliage — they either vanish or over-occlude. This is the classic failure
  mode and it is *not* fully solvable at reasonable resolution.
- **Temporal stability.** Indirect lighting must be accumulated over frames
  to be quiet, which reintroduces the need for history/reprojection — the
  same infrastructure TAA needs. Budget for it.
- **Marching cost** scales with cascade count and step count; empty space is
  cheap, near-surface is not.

### Skinned geometry — the honest position

Distance fields do not represent deforming meshes well, and per-frame
revoxelization of characters is expensive. Do not dismiss SDF GI over this,
but do not pretend it is solved either. Plausible architectures, in
increasing fidelity:

1. **Exclude from occlusion; receive only.** Characters are lit by GI but do
   not occlude or bounce. Cheapest, often visually acceptable.
2. **Capsule proxies.** A handful of capsules per character injected into
   the field — this is the established answer (Unreal's capsule shadows).
3. **Low-res per-frame object SDF.** A small dynamic volume (e.g. 32³) per
   character, revoxelized each frame in compute. Feasible for a handful of
   characters, not for a crowd.
4. **Screen-space fallback for dynamics.** Use screen traces where the SDF
   has no data — the hybrid most modern systems land on.

### Lighting injection and accumulation

Direct lighting must enter the traced representation somehow: either inject
radiance into voxels (light the voxel grid each frame from the sun/lights),
or trace toward lights from the gather point. Accumulate into either
screen-space irradiance or a probe volume, with temporal reprojection.

### Interaction with the existing path tracer

The existing path tracer is currently a separate world (flat spheres, CPU
readback). Under the proposed architecture it becomes *another pass* writing
an HDR resource. Longer term it is the natural **reference implementation**:
render the same scene offline-quality and diff against the realtime GI to
validate it. That is a genuinely valuable use and an argument for unifying
its scene encoding with `lib/scene3d.rae`.

---

## 10. Alternatives comparison

Scored against Rae's actual requirements. "Dyn" = handles runtime geometry
change; "Meta" = handles metaballs natively; "Skin" = handles skinned meshes.

| Technique | Dyn | Meta | Skin | Quality | Perf | Complexity | WASM |
|---|---|---|---|---|---|---|---|
| **SSAO** (contact only) | ✔ | ✔ | ✔ | Low (not GI) | Cheap | Low | ✔ |
| **SSGI** (screen-space bounce) | ✔ | ✔ | ✔ | Medium, leaks off-screen | Cheap-med | Low-med | ✔ |
| **SDF tracing GI** | ✔ (local rebuild) | **✔ exact** | ✖ needs proxy | Good | Medium | **High** | ✔ (memory-sensitive) |
| **Sparse voxel GI** | ✔ | via voxelization | ✖ needs proxy | Good | Medium-high | High | Memory risk |
| **Irradiance probes/volumes (dynamic)** | Partial — relight ✔, relocate ✖ | via tracer | ✔ receive | Good diffuse | Cheap at runtime | Medium | ✔ |
| **Compute path tracing** | ✔ fully | ✔ | ✔ | **Best** | **Expensive** | Med (exists!) | ✔ but slow |
| **Hardware RT** | ✔ | ✖ | ✔ | Best | Fast | Med | **✖ unavailable** |
| **Baked GI** | **✖ excluded by requirements** | ✖ | ✖ | Good | Free | Low | ✔ |

Reading of this table for Rae:

- **Hardware RT is out** on platform grounds, not preference.
- **Baked GI is out** by requirement — destructible content invalidates it.
- **SDF tracing is the strongest fit for the *content*** (metaballs exact,
  destruction local) but the **most expensive to build**, and skinned
  characters need a proxy.
- **Compute path tracing deserves more consideration than it usually gets
  here**, because Rae *already has one* and because a demo has a fixed,
  small scene and a controllable frame budget. Path tracing handles every
  category above — dynamic, destructible, metaballs, skinned — with no
  acceleration-structure maintenance problem if the scene is small enough to
  brute-force or to trace against the analytic SDF directly.
- **SSGI is the best quality-per-effort stepping stone** and shares all its
  inputs with SSAO.

A defensible staging that does not gamble: **SSAO → SSGI (reuses the same
inputs) → decide between SDF tracing and compute tracing once there is a
real scene to measure.**

---

## 11. WebGPU considerations

- **No depth resolve guarantee** → the prepass must be single-sampled, or
  the main pass must be. This is the constraint that shapes everything.
- **No read-write storage textures** in base WGSL → ping-pong pairs for
  iterative passes; declare both.
- **Storage-texture format support is narrow** → validate formats at
  declaration.
- **No hardware RT, no subgroup guarantees, no async compute** in core.
- **Binding limits** (bind groups, storage buffers per stage) are real and
  low on some backends; a graph that allocates bind groups per pass must
  respect them.
- **WASM/browser is a hard target** (#321) → memory ceilings are tighter,
  and anything that assumes desktop VRAM headroom must degrade.
- **Timestamp queries are optional** → per-pass GPU timing needs a fallback.

---

## 12. Metaball considerations

Rae already renders metaballs as a fragment raymarch writing `frag_depth`
into the shared depth attachment, so **implicit and triangle geometry
already occlude each other correctly**. That hybrid is working today and is
a real asset.

For lighting, the consequences:

- Metaballs are **natively an SDF**. Any SDF-based GI can trace them exactly,
  with no voxelization, no resolution loss and no update cost.
- Any **triangle-centric** lighting representation (BVH, per-mesh SDF bake)
  must approximate them.
- If Rae's direction includes substantial implicit geometry, this is the
  single strongest argument for SDF tracing over voxel or BVH approaches.
- Caveat: the current implementation submits **one metaball group with one
  material**. Multi-material and multi-group implicit geometry is unbuilt.

---

## 13. Dynamic and destructible geometry

Requirements the architecture must not violate:

1. No structure may require a full rebuild on geometry change.
2. Updates must be **local and bounded** — destruction dirties a region.
3. Update cost must be amortisable across frames.
4. Lighting must degrade gracefully during update, not pop.

Implications: prefer **grid/clipmap structures over trees**; keep a
**dirty-region queue**; drive updates from the scene layer (`lib/scene3d.rae`
would need a change/revision signal — it currently has none); and accept
temporal lag in far cascades.

Destruction is where SDF/voxel approaches genuinely beat BVH refit, and it
should be weighted accordingly if destructible environments are a real
product requirement rather than an aspiration.

---

## 14. Skinned and deforming geometry

Covered in §9. The architectural requirement is narrower than the GI
question: **the renderer must be able to treat "geometry that contributes to
lighting" and "geometry that is drawn" as different sets.** If that
separation exists, every skinned-mesh strategy (exclude, proxy, revoxelize)
becomes a policy choice rather than a rewrite.

Note that Rae has **no skinning at all** today — the vertex layout is
pos3/nrm3/uv2 with no joints or weights. Skinned characters are a
prerequisite project, not a detail of the GI work, and the GI decision
should not be over-fitted to a feature that does not yet exist.

---

## 15. Migration strategy

Incremental, each step shippable and independently verifiable:

1. **Depth/normal prepass.** Single-sampled depth + a normal target.
   Immediately verifiable by visualising both. Unblocks everything.
2. **Minimal pass abstraction.** Port the three existing passes into it
   unchanged. Success = identical screenshots, so the refactor is provable.
3. **HDR target + separate tonemap pass** (queue #309). Needed before any
   light accumulation is meaningful.
4. **SSAO** as the first genuinely new pass (#325).
5. **Measure.** Only then choose between SSGI / SDF tracing / compute
   tracing, with a real scene and real numbers.
6. Resource lifetime, transient allocation and barrier inference **only if**
   pass count or memory pressure justifies them.

Step 2's screenshot-equality check is what makes this safe: the abstraction
lands with zero visual change, so any later regression is attributable.

---

## 16. What should NOT be built yet

- A full render graph with aliasing, lifetime analysis and barrier
  inference. Three passes do not justify it.
- SDF GI, or any voxelization pipeline, before the prepass and measurement.
- Baked lighting of any kind as a primary path.
- Skinning, purely to make GI decisions easier.
- An Unreal-style renderer framework, feature registry or material graph.
- Multi-queue / async compute.
- Any new Rae language syntax. Nothing in this document needs it.

---

## 17. Open questions requiring explicit approval

1. **Prepass vs full GBuffer.** A depth+normal prepass costs an extra
   geometry pass but keeps the forward shading path. A GBuffer enables
   deferred lighting and many lights, but is a bigger change and worse for
   the MSAA story. *Recommendation: prepass first.*
2. **Keep MSAA, or move to TAA?** MSAA 4× conflicts with every
   screen-space technique (no depth resolve) and does not antialias shading.
   TAA is the modern answer but needs motion vectors and history.
   *This is the most consequential unresolved decision in this document.*
3. **Is destructibility a real requirement or an aspiration?** It
   substantially changes the GI recommendation. If real, grids win. If not,
   more options open up.
4. **How much does implicit geometry matter long-term?** If metaballs are
   central to Rae's identity, SDF tracing gains a lot of weight.
5. **Should the path tracer be unified** with `lib/scene3d.rae` and become a
   reference/validation pass?
6. **Memory ceiling for the WASM target** — this bounds cascade resolution
   before any code is written.
7. Does `lib/scene3d.rae` gain a **change/revision signal**? Dirty-region
   updates need one and it does not exist.

---

## 18. Recommended next implementation steps

In order, with rationale:

1. **Depth/normal prepass** (single-sampled). The unlock for every
   technique discussed. Small, self-contained, immediately verifiable.
2. **Minimal render-pass abstraction** — declared reads/writes, derived
   ordering, no aliasing/lifetime machinery. Port the existing three passes
   with screenshot equality as the acceptance test.
3. **SSAO** — contact-scale, half-res, bilateral upsample, applied to the
   indirect term only. First real client of both (1) and (2), and first
   consumer of the noise library (#320).
4. **HDR target + tonemap pass** (#309) — before any light accumulation.
5. **Then decide** between SSGI, SDF tracing and compute path tracing, with
   measurements from a real scene, and settle open question 2 (MSAA vs TAA)
   at the same time.

**Not** SDF GI next. It is the most interesting option and possibly the
right long-term one — particularly given metaballs and destructibility —
but committing to it before there is a readable depth buffer, a pass
abstraction to host it, or a scene to measure would be choosing a technique
before establishing the ground it stands on.
