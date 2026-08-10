# Shadow system design

Status: **design, not built.** Nothing in this document exists in the
renderer yet. It supersedes the one-line placeholders in
`webgpu-3d-renderer.md` §5 ("cascaded shadow maps for the sun; PCF") and
the bare `shadowMap` / `shadowPass` entries in
`render-graph-and-gi-plan.md` §5-§6, both of which were written against
the forward prototype that #356 replaced.

Related queue items: **#350** (direct shadows, the motivating bug),
**#371** (deferred lighting quality list, currently says "cascaded shadow
maps with PCF"), **#359** (rebuild gate — shadows are deferred behind it
deliberately), **#338-#342** (the GI arc this must compose with).

---

## 1. What we are solving

Recorded in #350, verified in example 110: **SSAO darkens the indirect
term only, which is correct, so a sunlit floor shows almost no darkening
beneath an object resting on it.** The AO buffer clearly darkens the
plane under the spheres; the composited image barely changes, because the
plane's ambient fraction is small next to the sun. Objects read as
hovering even when they touch.

That is a *direct* light occlusion problem. No amount of AO tuning fixes
it, and widening the AO radius to fake it would collide with real GI
later (#337 keeps AO at contact scale for exactly this reason).

### Requirements

| # | Requirement | Consequence |
|---|---|---|
| R1 | Soft edges, physically plausible | Penumbra must WIDEN with blocker distance; a constant-width blur is the thing that reads as "game shadow" |
| R2 | Minimal pixelation / shimmer | Rules out naive CSM at low resolution; demands texel-grid snapping and a resolution-independent softening term |
| R3 | Scales mobile → high-end | Quality knobs must change cost by ~10x, not 20% |
| R4 | Composes with SSAO (#337) | No double-darkening: strict separation of direct vs indirect occlusion |
| R5 | Composes with SDF GI (#338-#342) | Should SHARE the GI scene representation rather than duplicate it |
| R6 | Deferred architecture (#356) | Shadowing is a screen-space resolve, not a per-object forward operation |
| R7 | WebGPU only | No hardware ray tracing. Compute + raster only. |

### A naming correction

The question "does it use the cascades we already have?" mixes two
different things:

* **SDF cascades** (`sdfCascades`, `render-graph-and-gi-plan.md` §5) are
  a *planned, not built* volumetric scene representation for GI.
* **Shadow cascades** are a *partition of the view frustum* for a
  directional light's shadow maps. These do not exist in any form.

Neither exists today. But §5 below argues they should be related: the SDF
cascades are the best long-range shadow source we will have, and building
shadows without planning for that is how you end up with two scene
representations.

---

## 2. Why not the obvious answers

| Technique | Why not, alone |
|---|---|
| **CSM + PCF** | Fixed-width penumbra (fails R1). Softness is measured in shadow-map texels, so it changes with cascade and resolution — the seam between cascades becomes visible as a change in blur width. Wide PCF is many taps for a blur that is still wrong. |
| **PCSS** | Right idea for R1 (blocker search → variable penumbra) but the blocker search is the expensive half, it is noisy, and it still samples a texel grid (R2). Good as a *component*, bad as the whole answer. |
| **VSM / EVSM / MSM** | Genuinely prefilterable — mip and blur the map, get soft anti-aliased edges cheaply. But light leaking is a correctness bug, and it leaks worst exactly where two occluders are at different depths, which is where contact shadows live. EVSM needs fp32 and heavy bandwidth (bad for R3 on mobile). Moment maps need 4 channels. Rejected as the primary. |
| **Pure SDF-traced shadows** | Beautiful for R1/R2/R5 — no texel grid at all, so no pixelation, and the cone angle gives true physical softness. But limited by cascade voxel resolution: it cannot resolve a thin object's contact shadow, and it cannot see geometry absent from the representation. Cannot be the only source. |
| **Ray-traced** | Not available in WebGPU (R7). |

Every single technique fails at least one requirement. **The answer is a
hybrid**, and the design work is choosing which technique covers which
spatial scale, and making the seams invisible.

---

## 3. The design: three layers by spatial scale

Shadowing is split by **distance from the receiver to the occluder**,
because that is what determines both what the eye notices and which
technique is accurate there.

```
   contact          mid-field                far-field
   0 – 0.5 m        0.5 – ~50 m              beyond cascades
   ─────────────    ───────────────────      ─────────────────
   Layer B          Layer A                  Layer C
   screen-space     cascaded shadow map      SDF cone trace
   ray march        stochastic + temporal    (shared with GI)
   (depth buffer)   (blue noise + TAA)
```

The **shadow mask** is the minimum of the three, written once to a
full-resolution `r8unorm` target that the lighting pass multiplies into
the *direct* sun term only.

### Layer A — the cascaded shadow map (the workhorse)

Standard cascaded ortho shadow maps for the single directional sun, with
three deliberate departures from textbook PCF:

**A1. Stochastic sampling, not a fixed kernel.** One (or a few) samples
per pixel per frame, positions drawn from a blue-noise disc rotated by
the same interleaved gradient noise the SSAO pass already uses, jittered
per frame by frame index. This is the same trick #337 uses to turn
banding into noise the denoiser can eat.

**A2. Penumbra width from a blocker estimate.** A cheap blocker search
(4-8 taps at a fixed small radius) gives average blocker depth; the
sample disc radius is then

```
penumbraWorld ≈ (receiverDepth − blockerDepth) × tan(sunAngularDiameter)
penumbraTexels = penumbraWorld / cascadeTexelWorldSize
```

NOT the textbook PCSS form `(dR − dB)/dB × lightSize`. That formula is
for an area light at finite distance, where penumbra grows with the
*ratio* of distances. The sun is directional: penumbra depends only on
the receiver-to-blocker *gap*, linearly. Using the PCSS form with an
arbitrary "light size" is exactly how implementations end up far too
soft at contact — the error is largest when `dB` is small, which is the
contact case.

The sun subtends **~0.53°**, so a blocker 2 m above a surface casts a
penumbra about **2 m × tan(0.53°) ≈ 1.9 cm** wide. That number is the
sanity check: shadows should be *nearly sharp* at contact and visibly
soft only for distant blockers — near-sharp contact is most of why a
shadow reads as real.

**A3. Denoise, in two stages that must not be conflated.** 1-2
taps/pixel/frame is noisy. Two distinct mechanisms can absorb it:

* **Colour TAA (#335), for free.** The noisy mask multiplies the direct
  term, so shadow noise becomes shading noise, and the existing TAA
  eats it like any other shading noise. Zero new machinery — this is
  what step 1 ships with. Its limit: the colour-space clamp cannot tell
  shadow noise from detail, so convergence in a penumbra is slower and
  a fast-moving penumbra can shimmer.
* **A dedicated mask history (step 3).** Accumulate the mask itself,
  before lighting, with its own rejection on mask delta. Sharper
  penumbrae and faster convergence, at the cost of one r8 history
  buffer (~2 MB) and a real reprojection pass. This is NOT "machinery
  we already own" — it is new, and it is deferred until measurement
  shows colour TAA is not enough.
* **A depth-aware spatial blur** — #372 already calls for a dedicated
  depth-aware SSAO blur. The shadow mask wants the *same* filter with a
  different radius. Build it once, parameterised.

This combination is what makes R1 and R2 affordable at the same time: the
softness comes from the sample *distribution* (resolution-independent, no
texel grid visible) rather than from a blur applied after the fact.

**Cascade construction rules** (each of these is a specific artefact
fixed):

| Rule | Artefact if omitted |
|---|---|
| Snap each cascade's ortho centre to whole shadow-map texels in world space | Shadows crawl and shimmer as the camera moves — the single most-noticed shadow artefact, and exactly the "pixelation" complaint |
| Fit cascades to the frustum slice bounding sphere, not the frustum corners | Shadow map resolution changes with camera yaw, so shimmer returns on rotation |
| Overlap cascades and dither the transition using the same blue noise | A hard cascade boundary is visible as a line where blur width changes |
| Normal-offset bias (offset along the geometric normal by ~1 texel world size), not just depth bias | Depth bias alone causes peter-panning, which detaches contact shadows — the very bug we are fixing |
| **Do not** use reverse-Z for the cascades | Reverse-Z buys precision from the float exponent against perspective's 1/z. Ortho depth is linear, so it buys nothing and only risks disagreeing with the main pass's convention. Worth stating so nobody "fixes" the inconsistency later. |

### Layer B — screen-space contact shadows

A short ray march (roughly 8-16 steps over ~0.5 m) from each pixel toward
the sun, through the **depth buffer we already have**, using the depth
pyramid from #367/#369 to skip empty space.

This exists to fix the specific failure that shadow-map bias creates: the
gap between an object and its own contact shadow. It runs **only where
Layer A says the pixel is lit** — a shadowed pixel cannot get darker, so
the march is skipped, which is also where most of the cost saving is.

It is screen-space, so it fails on off-screen occluders and at grazing
angles. That is acceptable: it is a *correction term* over ~0.5 m, and
Layer A covers the same range less precisely. Combine with `min()`.

### Layer C — SDF cone-traced shadows

Once the GI representation (#338/#339) exists, the SDF cascades can be
cone-traced directly for a soft shadow factor, using the standard
running-minimum trick:

```
shadow = min over march of  clamp(k * d(p) / t, 0, 1)
```

where `d` is the SDF distance at the marched point, `t` the distance
travelled, and `k` the cone tightness derived from the sun's angular
diameter — the *same* 0.53° that drives A2, so the two layers agree about
how soft a shadow should be.

This layer earns its place three times over:

1. **Metaballs and analytic SDF geometry have no triangles to
   rasterise.** They currently cannot appear in a shadow map at all
   without a separate conservative proxy. They trace exactly.
2. **Far-field shadows beyond the last cascade** — mountains shadowing a
   valley — at a cost that does not grow with world size.
3. **It is the low-end tier's entire shadow implementation** (§6).

It shares its scene representation with GI, satisfying R5: the SDF
cascade update is paid once and consumed by both.

---

## 4. Where it lives in the frame

Shadowing becomes **one screen-space pass producing one mask**, which is
what the deferred architecture (#356) makes natural. Written as render
graph resources and edges, so ordering is *derived* and not asserted:

```
  gbuffer ──────────► gAlbedo, gNormal, gMaterial, depth
     │
     ├──► depthPyramid  (exists, #367/#369)
     │
  shadowCascades (raster, N draws) ──► shadowMap[array]
     │
  shadowMask (compute)  reads depth, gNormal, shadowMap, depthPyramid,
     │                        sdfCascades (when Layer C is on)
     └──────────────► shadowMaskRaw   r8unorm, full res
     │
  shadowDenoise (compute) reads shadowMaskRaw, depth, gNormal
     └──────────────► shadowMask      r8unorm, full res
     │
  ssao ──► aoTex (half res, exists)
  gi   ──► indirectDiffuse (later)
     │
  lighting  reads gAlbedo, gNormal, gMaterial, depth,
     │             shadowMask, aoTex, indirectDiffuse
     └──────────────► hdrColor
     │
  taa ──► tonemap ──► composite
```

Two resolution decisions worth stating explicitly, because they differ
from SSAO and the difference is not arbitrary:

* **The shadow mask is FULL resolution.** SSAO is half-res because
  ambient occlusion is low-frequency; a bilateral upsample loses nothing.
  Shadow edges are the opposite — they are the highest-frequency signal
  in the image, and half-res shadows read as exactly the pixelation R2
  forbids.
* **The mask is r8unorm, not a bitmask.** It carries a *partial*
  visibility fraction, because that is what soft shadows mean. One byte
  per pixel; ~2 MB at 1080p.

---

## 5. Composition rules (R4, R5)

This is the part that silently goes wrong, so it is stated as
invariants rather than as description:

```
direct   = sunColor · BRDF(N,V,L) · NoL · shadowMask
indirect = giIndirect · aoNearField        (today: hemisphere ambient · ao)
out      = direct + indirect + emissive
```

1. **`shadowMask` multiplies DIRECT light only.** It never touches
   ambient or GI.
2. **`aoTex` multiplies INDIRECT light only.** It never touches the sun.
   This is already how #337 composes and is why #350 exists at all.
3. **When GI lands (#339), AO does not simply multiply it.** World-space
   GI already contains the occlusion AO approximates; multiplying both
   double-darkens. AO's surviving role is near-field detail below the GI
   representation's resolution, plus specular occlusion — a bounded
   correction, not a global multiplier. Budget for shrinking AO's
   influence when GI arrives rather than discovering the conflict in an
   image.
4. **Layers A, B and C combine with `min()`, not by multiplication.**
   They are three estimates of one physical quantity — the fraction of
   the sun disc visible — and at contact points all three see the SAME
   occluder, so multiplying compounds one occlusion three times. The
   honest caveat: when two DIFFERENT occluders each half-shadow a pixel,
   `min()` under-darkens (true visibility is closer to the product).
   That case is rarer and less visible than triple-darkened contacts,
   which is why `min()` wins — but it is a chosen approximation, not a
   law.

---

## 6. Scaling: the tiers

Quality is a small number of knobs, and the graph selects them (the graph
itself must not learn the word "shadow" — see `render-graph-and-gi-plan.md`
§4).

| Knob | Low | Balanced (default) | High |
|---|---|---|---|
| Cascades | 1 | 3 | 4 |
| Cascade resolution | 1024² | 2048² | 2048²-4096² |
| Shadow map format | Depth16Unorm | Depth16Unorm | Depth32Float |
| Layer A samples/px/frame | 1, fixed radius (no blocker search) | 1 + 8-tap blocker search | 2-4 + 16-tap blocker search |
| Layer B (contact) | off | 8 steps | 16 steps |
| Layer C (SDF trace) | **the whole shadow system, once it exists** | far-field + SDF geometry | far-field + SDF geometry |
| Denoise | spatial only | spatial + TAA | spatial + TAA |
| Mask resolution | half | full | full |

Estimated shadow-map memory: low **2 MB** (1×1024²×16-bit), balanced
**24 MB** (3×2048²×16), high **128 MB** (4×2048²×32 + mask + history).

**Depth16Unorm on mobile is deliberate.** With well-fitted ortho cascades
the depth range per cascade is small, and 16 bits is enough; it halves
shadow-map bandwidth, which on a tiled mobile GPU is the dominant cost.
The desktop tier keeps 32-bit because the far cascade's range is large.

**The low tier comes in two eras.** Until Layer C exists (build steps
1-4), low is the honest degraded version of Layer A: one 1024² cascade,
one sample, spatial denoise, half-res mask. Once #339 lands, low
switches to Layer C alone: SDF cone tracing, which is soft and stable —
no shimmer, no texel grid, no bias artefacts — at the cost of missing
small-scale contact detail.

**And the cost claim there needs honesty.** SDF tracing is not cheaper
per PIXEL — a 16-32 step march through a 3D texture costs more resolve
ALU than a couple of shadow-map taps. What it eliminates is the entire
shadow GEOMETRY pass (re-rasterising the scene N times per frame, which
scales with scene complexity) and the shadow-map memory. So it wins on
geometry-heavy scenes and on total memory, and loses on resolve-bound
ones; whether it is a net win on a given phone is a measurement, not an
assumption. The claim that survives either way: it *looks* better than
a crunchy 1024 map, because its failure mode is softness rather than
aliasing.

The structural point stands: this option exists only because the SDF
representation is being built for GI anyway.

### Implementation notes the diagram hides

* **r8unorm is not a writable storage format in WebGPU**, so a compute
  `shadowMask` pass cannot write it directly. Options: write `r32float`
  storage and blit down (wastes bandwidth), or produce the mask in a
  fragment pass with an r8unorm colour attachment (fine — the pass is
  full-screen either way). Decide in step 1; the graph does not care.
* **Every vertex format needs a depth-only pipeline.** The cascade pass
  re-rasterises the scene, so the static (32-byte) and skinned (80-byte,
  #374) formats each need a shadow variant — the skinned one runs the
  same palette skinning with a null fragment stage. Forgetting this
  means the walker casts no shadow, silently.
* **Cascades do not all need re-rendering every frame.** The far
  cascade moves slowly; updating it every 2-4 frames (or only when the
  camera crosses a texel) is one of the biggest costs levers on mobile
  and costs nothing visually if the transition dither covers it. Design
  the cascade pass as N independent graph passes so the graph can skip
  some — do not bake "render all cascades" into one node.

---

## 7. Risks, and how each is detected

| Risk | Detection |
|---|---|
| TAA ghosting on moving shadows — the mask is temporally accumulated, so a moving occluder smears | Compare a moving-blocker frame against a converged still frame; needs a shadow-specific history rejection on mask delta, not just the colour clamp |
| Blue-noise sampling reads as *grain* rather than as *soft* if the denoiser is too weak | A/B still-frame stdev in the penumbra, the same measurement discipline #373 used for SSAO banding |
| Cascade transitions visible despite dithering | Fly a camera along the transition and diff consecutive frames; a seam shows as a stationary band |
| Peter-panning returns under normal-offset bias at grazing angles | The example-110 sphere-on-plane case is the standing reference; assert the contact shadow touches |
| Layer C at low cascade resolution loses thin geometry entirely | Ground-truth against the compute path tracer (#340), which is already planned as in-engine truth |
| Double-darkening when GI lands | Render with GI on and AO forced to 1.0; the difference should be small and near-field only |

---

## 8. Build order

Each step is independently visible and independently verifiable. Nothing
here should start before #359's rebuild gate.

1. **A-minimal** — 3 cascades, texel-snapped, 1 sample + fixed radius,
   spatial denoise. *Gate: the sphere in 110 stops hovering.* This alone
   closes #350.
2. **Layer B** — contact shadows over the depth pyramid, `min()`
   combined. *Gate: contact shadow touches the object at all camera
   angles.*
3. **A2 + A3** — blocker search, variable penumbra, TAA history. *Gate:
   measured penumbra width grows linearly with blocker distance and
   matches the 0.53° prediction within a few percent.*
4. **Tier knobs + mobile** — Depth16Unorm, the interim 1-cascade low
   tier (§6), half-res mask, cascade update-rate throttling. *Gate:
   measured frame cost ratio between tiers is ≥5x.*
5. **Layer C** — after #339 gives an SDF representation. Far-field and
   SDF-geometry shadows, then the low tier switches to it entirely.

Steps 1-4 are useful with no GI at all. Step 5 is where the two arcs
meet, and designing for it now is what stops step 1 from being rewritten
later.

---

## 9. Open questions

* **Local lights.** This document covers the directional sun only. Point
  and spot shadows want a different structure (atlas + per-light
  resolution budget); the mask becomes per-light or the lighting pass
  loops. Worth deciding before step 1 fixes the mask's shape.
* **Transparency.** Not addressed. Needs either a separate translucent
  shadow term or acceptance that transparent surfaces cast opaque
  shadows.
* **Does Layer B belong inside the SSAO pass?** Both march the depth
  buffer with the depth pyramid at contact scale. Merging saves a pass
  and one depth read; separating keeps the direct/indirect split of §5
  physically clean. Measure before merging.
* **Cascade count vs one large map with a virtual/sparse allocation.**
  Sparse shadow maps scale better with world size but are a large jump in
  complexity and need `texture_binding_array`-class features WebGPU does
  not portably have.
