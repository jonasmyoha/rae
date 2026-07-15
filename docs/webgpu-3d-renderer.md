# Rae WebGPU 3D Renderer — Design (future / high-level)

**Status:** Active design, with the compute-based engine seed in
`examples/107_gpu3d_minimal`. This document describes the future raster path;
the concrete Assembly demo progression is in `docs/assembly-2026-demo-plan.md`.
Sequenced **after** the 2D/UI renderer Tier 0–1.
**Target:** Compiled (C backend) + Web, via WebGPU-everywhere (WGSL single
source). Live VM is frozen and a non-goal (`docs/live-vm-status.md`).

---

## 1. What this is

A 3D renderer **for games** — mobile games and indie Steam titles, not AAA — that
shares one machine with the UI:

- Same **WebGPU render binding** as the 2D renderer (extended for depth, MSAA,
  MRT, HDR formats).
- Same **ECS** (`UiWorld`/`ComponentTable(T)`) and **data-driven render graph**.
- Same **compositor** — 3D is a producer of render targets that the UI/2D layer
  composites (3D-in-a-card *and* fullscreen 3D, see 2D doc §7).
- Same **linear-light color pipeline** (`docs/color-management-plan.md`) and
  **right-handed Z-up Blender** world convention (`docs/coordinate-system.md`).

So a Rae game gets: 3D scene + 2D HUD/UI + particles + post, from one stack, on
desktop and web.

---

## 2. Goals / non-goals

**Goals**
- Indie/mobile-scale 3D: meshes, PBR-ish materials, real-time lights + shadows.
- **Good antialiasing** (the perceptual win indies notice first).
- **SSAO** for grounded contact shadows.
- **Optional GI** via techniques we prototype and pick from (see §6).
- **Great particles & effects** (reuse the compute path).
- **Good color-grading tools** — exposure, tonemap, 1D/3D LUTs (ties to the
  grading-app ambition in `color-management-plan.md`).
- **Scales down to mobile** and up to desktop via quality tiers.

**Non-goals (for now)**
- AAA feature parity (virtualized geometry, full RTGI, clustered-everything).
- A node-graph material editor (materials are data; authoring is later).
- Offline/path-traced final-frame rendering (the raytracer examples stay a
  separate compute demo, though they can feed a render target).

---

## 3. Guiding principles

1. **One stack, quality tiers.** A single render path with feature flags that
   scale by platform (mobile / web / desktop), not separate renderers.
2. **Data-driven.** Scene = ECS components; frame = a **render graph** built from
   data, not hardcoded passes. Same philosophy as the 2D DrawList extract.
3. **Linear-light, HDR internally.** Render to `rgba16f`, tonemap + grade at the
   end, output display-referred (sRGB / Display-P3). Per `color-management-plan`.
4. **Forward-first.** Favor a **clustered forward (Forward+)** path: MSAA-friendly,
   bandwidth-friendly (mobile/web tiled GPUs hate fat G-buffers), transparency is
   natural. Keep a deferred path as a *possible* desktop-tier option, not the base.
5. **Reuse, don't fork.** 3D extends the 2D renderer's binding, ECS, compositor,
   and color pipeline — it does not stand up a parallel engine.

---

## 4. Architecture — shared render graph

The 2D renderer's fixed compositor **generalizes into a data-driven render
graph** shared by 2D and 3D. Nodes declare inputs/outputs (textures, depth,
buffers); the graph schedules passes and manages transient render targets.

```
RenderGraph (data-driven; nodes from ECS + config)
  ├─ ShadowPass(s)        → shadow maps
  ├─ DepthPrepass         → depth (enables Forward+ culling, SSAO)
  ├─ LightCluster (compute)→ per-cluster light lists      [reuses lib/gpu compute]
  ├─ ForwardOpaque        → HDR color (rgba16f) + depth   [MSAA here]
  ├─ SSAO (compute/screen)→ AO buffer → applied in lighting/composite
  ├─ Sky / fog
  ├─ Transparent          → HDR color (sorted)
  ├─ Particles            → HDR color                     [compute-simulated]
  ├─ PostStack            → bloom, tonemap, color-grade (LUT)  → display color
  └─ UI / 2D composite    → swapchain                     [the 2D renderer node]
```

Mobile tier prunes nodes (no depth prepass / cluster step; simpler shadows;
FXAA instead of MSAA+TAA). The graph makes that a data decision.

---

## 5. Lighting, shadows, AA, SSAO

- **Lighting** — clustered forward; PBR metal/rough (or a cheaper mobile lit
  model as a tier). Punctual lights (dir/point/spot) + image-based ambient.
- **Shadows** — cascaded shadow maps for the sun; atlas for local lights; PCF
  soft edges. Mobile tier: one cascade or baked.
- **Antialiasing** — primary target **MSAA** in the forward pass (cheap-ish on
  tiled GPUs, clean geometric edges) + optional **TAA** on the desktop tier for
  spec/shading aliasing. **FXAA/SMAA** as the low-end fallback. AA quality is a
  headline indie feature, so this gets real attention.
- **SSAO** — depth(+normal)-based, computed in a screen-space pass (compute or
  fragment), applied to ambient. Half-res + blur for mobile. A scalable-quality
  knob.

---

## 6. Optional global illumination — prototype, then pick

GI is **optional and tiered**; we prototype a few and choose per project:

- **Baked lightmaps + light probes (irradiance)** — best quality/perf for static
  scenes, mobile-friendly, offline bake. *Likely the default for shipped games.*
- **Screen-space GI (SSGI / screen-space bounce)** — runtime, no bake, screen-
  space limits; a desktop-tier enhancement.
- **Irradiance volumes / DDGI-lite** — dynamic probe grid; a mid-tier runtime
  option to prototype.
- **Voxel GI** — desktop-only experiment; likely too heavy for our scope.

We build the **probe/irradiance plumbing first** (it's reused by baked *and*
dynamic paths) and treat GI techniques as swappable render-graph nodes.

---

## 7. Particles & effects

- **GPU particles** — simulate in a **compute pass** (reuse `lib/gpu.rae`), render
  as instanced quads/meshes in the HDR forward/transparent stage; soft particles
  via depth, additive/alpha blend, HDR for bloom-friendly glows.
- **Effects** — bloom (HDR threshold + separable blur), screen-space distortion,
  decals (later), trails. Shares the 2D renderer's offscreen/blur machinery.

---

## 8. Color grading tools

Directly leverages `docs/color-management-plan.md` (linear-light, ACES-class):

- **Exposure** (auto/manual), **tonemap** (ACES/AgX-class), **white balance**.
- **1D + 3D LUTs** (`.cube`), lift/gamma/gain, saturation/contrast — the same
  grading primitives the planned grading app needs, exposed to games as a
  post-stack node with presets.
- HDR output where the display supports it; correct SDR fallback.

This is a differentiator: indie games get filmic grading "for free" because the
grading app and the game renderer are the same linear-light machine.

---

## 9. ECS & data-driven scene

3D scene state lives in ECS components on the shared `UiWorld` (or a sibling
world): `Transform3D`, `MeshRenderer` (mesh + material handle), `Light`,
`Camera`, `Environment` (sky/IBL/fog), `ParticleEmitter`, `PostProfile`
(grade/tonemap settings). A pure **extract system** (mirroring the 2D DrawList
extract) produces the render-graph inputs each frame — O(n), sparse-set lookups,
no nested per-entity scans (`CLAUDE.md` §UI rendering loops).

Assets (meshes, textures, materials, LUTs) are handles into GPU resource tables,
same opaque-handle model as the 2D renderer and `lib/gpu.rae`.

---

## 10. Platform scaling

| Tier | AA | Shadows | SSAO | GI | Particles |
|---|---|---|---|---|---|
| Mobile/Web | FXAA or 2x MSAA | 1 cascade / baked | half-res, optional | baked probes | CPU or light compute |
| Desktop indie | MSAA (+TAA) | cascades + local | full-res | baked + SSGI option | full compute |

One code path; the render graph + feature flags select the tier.

---

## 11. Impact on the 2D renderer (changes made there)

This design pushed three changes into `docs/webgpu-2d-ui-renderer.md`:

1. **Compositor → render graph.** The 2D doc's fixed compositor is reframed as a
   node in the shared, data-driven render graph this doc introduces.
2. **Color space ordering.** The 2D UI composites **after** 3D tonemap + grade,
   in display-referred space, while **blending in linear light**
   (`color-management-plan`). The 2D doc now states the color/blend space and
   where UI sits relative to post.
3. **Render binding scope.** The Tier-0 WebGPU render binding (QUEUE #109) must
   anticipate **depth buffers, MSAA, MRT, and HDR (`rgba16f`) formats** so the 3D
   renderer extends it rather than forking it.

---

## 12. Phasing (after 2D Tier 0–1)

- **3D-Tier 0:** forward opaque + depth, one camera, single dir light + CSM,
  MSAA, HDR target + tonemap, composite into UI. A lit spinning mesh in a card.
- **3D-Tier 1:** clustered lights, SSAO, particles (compute), bloom, color-grade
  post-stack with LUTs.
- **3D-Tier 2:** GI prototypes (baked probes first, then SSGI), TAA, transparency
  sorting, decals.

Each tier ships an example (lit mesh → small scene → a tiny playable demo) the
way the 2D plan ships text/vector/ui examples.

---

## 13. Open questions

- Forward+ only, or a deferred desktop tier — decide after a bandwidth bench.
- Shared world vs. dedicated 3D world for scene ECS.
- Mesh/material/scene asset format (glTF import? Blender pipeline given Z-up).
- Skinning/animation scope for v1 (static + simple skeletal?).
- How much of the post-stack is shared verbatim with the future grading app.
