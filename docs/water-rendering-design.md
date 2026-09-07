# Water rendering for Rae — survey and design

Status: staged implementation (September 2026). The copy-only
`transparentForward` graph node and `litCopy` target are implemented (#842),
and the pass now DRAWS: alpha-blended instanced geometry into the lit HDR
target against a read-only scene depth (#843, `lib/TransparentForward.rae` +
`lib/transparentForward.wgsl`). Water itself is not implemented yet (#830).
The survey below describes the original pre-water baseline.

### Implemented prerequisite: opaque radiance snapshot

`RendererDeferred` derives `lighting -> transparentForward -> taa` using
`passModifies(litColor)`, with an additional depth read and `litCopy` output.
`TransparentForward.copyOpaqueRadiance` encodes a texture copy through the
WebGPU bindings and submits on the same queue as lighting. It leaves radiance
unchanged. #843 added the blend pipeline (src-alpha / one-minus-src-alpha,
depth test Greater = reverse-Z, depth write OFF, cull none, one target in the
HDR format lighting wrote) and the draw: it reuses the geometry pass's Frame
uniform + DrawU draws storage and vertex layout, so a client packs an
instance once (`addTransparentInstance(cache, mesh, transform, r, g, b,
alpha)` — alpha rides in the record's metallic slot, unused by an unlit blend)
and the pass issues one instanced DrawIndexed per frame into `gb_lit_view`
(loadOp Load) with the G-buffer depth attached read-only. Unlit by design;
shading is the client's (water #830; particles landed in #844). Verified on hardware
(not just non-blank): translucent cubes blend over the walker/terrain, are cut
off by opaque geometry in front, and are clipped where they sink into the
terrain. Note for clients: readiness is `gbMeshIcount > 0`, NOT `gbMeshReady`
— the latter is 0 outside the geometry pass.

The copy is full resolution, single-sampled, and uses the selected HDR format
(RG11B10Ufloat or RGBA16Float). Its C-owned texture/view follow the deferred
target generation, resize and shutdown lifecycle. The three new `rae_gb_*`
accessors only borrow handles. A client caching `gbLitCopyView()` must rebuild
its bind group whenever `gbTargetsGen()` changes. Do not release borrowed handles.
This currently adds one full-frame GPU copy and one HDR texture even when the
scene has no transparent objects; there is no CPU readback or compute pass.

Focused verification (from the repository root, one command at a time):

```sh
TEST=572 RAE_TEST_NO_HISTORY=1 perl -e 'alarm shift; exec @ARGV' 180 bash compiler/tools/watch-tests.sh
TEST=580 RAE_TEST_NO_HISTORY=1 perl -e 'alarm shift; exec @ARGV' 180 bash compiler/tools/watch-tests.sh
MAKEFLAGS='TEST_RUNNER=tools/run_examples.sh' RAE_EXAMPLE_FILTER='112_metaballs_deferred 114_walker_character' perl -e 'alarm shift; exec @ARGV' 420 bash compiler/tools/watch-tests.sh
sh tools/webgpu-c-surface-gate.sh
```

The screenshots check continued rendering, not refraction or alpha blending.

Two visual targets were asked for:

1. **Stylized / toon (cel-shaded) water that runs well on mobile.**
2. **Fully realistic, Unreal-Engine-class water** for desktop.

This document surveys what exists in the open (Unity, Godot, Unreal, Bevy,
raw WebGPU), states what Rae's renderer can and cannot do today, and recommends
a phased implementation that fits the ECS architecture and the WebGPU (WGSL)
renderer. The short version is at the end under **Recommendation**.

---

## 1. What Rae's renderer has today (the constraints that decide everything)

Facts from the current tree, not aspirations:

| Fact | Where | Why it matters for water |
|---|---|---|
| Deferred G-buffer: albedo, oct-normal, roughness/metal, depth; **no alpha blending in the G-buffer pass** | `lib/Gbuffer.rae`, `lib/Particles.rae` ("fade == shrink") | Water is a **transparent, refractive** surface. It cannot be a G-buffer material. It needs a pass that runs *after* lighting and reads scene colour + depth. |
| Pass order today: shadow → gbuffer → depthPyramid → ssao → lighting → taa → composite → present | `RenderTag.*` in `lib/Rendergraph.rae` / `RendererDeferred.rae` | There is **no forward / transparent pass**. The render-graph plan (`docs/render-graph-and-gi-plan.md` §6) draws a `forwardLighting → hdrColor` node, but it is a plan, not code. |
| Scene depth is readable by later passes (`gbViewDepth`, used by SSAO / depth pyramid) | `lib/Gbuffer.rae` | Depth is the one input every water technique needs: shoreline foam, depth-tinted colour, refraction offset masking, intersection fade. Already available. |
| TAA exists | `RenderTag.taa` | Realistic water shimmers without temporal filtering; toon water does not need it. |
| The sky is designed as *background now, reflection source later* — `radiance(dir)` / `irradiance(n)` | `lib/Sky.rae` header | Cheapest possible water reflection is `sky.radiance(reflect(view, normal))`. The interface is already the right shape; no planar reflection or cubemap machinery needed for a first version. |
| Compute-shader pattern exists: grass is generated and drawn by a WGSL compute pipeline built from a string asset | `lib/GrassCompute.rae` + `lib/grass_compute.wgsl` | An FFT ocean is a compute job. The plumbing (module from WGSL string, pipeline, bind groups, dispatch inside a pass) is proven. |
| Terrain height is a CPU function (`terrainHeightAt`) and a GPU heightfield | `examples/114.../terrainSystem` | Shore foam and river carving want water-vs-ground depth; both sides are reachable. |
| Renderer C-surface gate: new features must go through the WebGPU bindings, not new `rae_ext_Gbuffer_*` C | `AGENTS.md`, `tools/webgpu-c-surface-gate.sh` | Water must be Rae + WGSL over the bindings. That is fine — every technique below is shaders + buffers. |
| Targets: wgpu-native on macOS, an iOS xcframework build (`tools/ios/`), quality tiers "mobile / web / desktop" are a stated design goal | `docs/webgpu-3d-renderer.md` | One water system with a **tier switch**, not two systems. |

**The single hard prerequisite is a transparent forward pass** (read depth +
lit scene colour, write to the HDR colour target, run after `lighting` and
before `taa`). Everything else is shader work on top of it. This pass also
unblocks alpha-blended particles and glass, so it is not water-specific cost.

---

## 2. Survey of open water systems

Legend for **Fit**: how directly the *design* transfers to Rae. Code never
transfers — every project below is GLSL/HLSL/ShaderLab/Godot-shader, and Rae
is WGSL over its own render graph — so "fit" means the technique and the
data model, not the files.

### 2.1 Unity

| Project | Technique | License | Mobile | Fit for Rae |
|---|---|---|---|---|
| **Roystan / IronWarrior `ToonWaterShader`** — [repo](https://github.com/IronWarrior/ToonWaterShader), [tutorial](https://roystan.net/articles/toon-water/) | The canonical toon water: two-colour depth gradient from the depth buffer, shoreline foam band = depth difference + panned noise, "toon ripples" = a noise texture animated and *posterised* with a threshold, surface distortion via a panned normal/distortion map. One pass, one quad, no compute. | Tutorial source (permissive; check the repo file before copying any line — the *recipe* is what we want, not the ShaderLab) | **Yes** — this is the mobile-proven stylized recipe | **Highest.** Every ingredient maps to inputs Rae already has (depth, time, a noise texture). This is Phase 1. |
| **Unity `boat-attack-water`** (URP demo water) — [repo](https://github.com/Unity-Technologies/boat-attack-water) | Gerstner waves (vertex), depth-based colour + foam, screen-space refraction, planar reflection *or* SSR, caustics, simple buoyancy. Built to ship the Boat Attack demo on phones and consoles. | **Unity Companion License — usable only in Unity projects.** Design reference only; do not copy. | Yes (it targeted mobile) | High as a *design*: it is the best public example of a mid-tier, one-system-with-quality-toggles water. |
| **Crest** — [repo](https://github.com/wave-harmonic/crest), [docs](https://crest.readthedocs.io/) | Class-leading: FFT (and Gerstner) wave spectra, **multi-resolution LOD cascades** ("clip-map" style, cheaper far from camera), dynamic wave simulation (ripples from objects), foam sim, shoreline/depth cache, underwater, buoyancy queries. Heavy on GPU LOD strategy. | **MIT** on GitHub (a paid URP build exists on the Asset Store) | Desktop/console first; works on high-end mobile only with everything turned down | High for Phase 2 *architecture*: the LOD-cascade idea and the "query water height from gameplay" API are the two things worth copying. Too big to port wholesale. |

### 2.2 Godot

| Project | Technique | License | Mobile | Fit |
|---|---|---|---|---|
| **2Retr0 `GodotOceanWaves`** — [repo](https://github.com/2Retr0/GodotOceanWaves) | FFT ocean in compute shaders: TMA spectrum × Hasselmann directional spreading, **Stockham** FFT (no bit-reversal), butterfly texture computed once per resolution, row FFT + transpose + reuse for columns; outputs displacement + normal + **foam from negative Jacobian**; several **cascades** with independent tile sizes; an **update-rate** knob with load balancing (only one cascade updates per frame when the frame is short). Sea spray via GPU particles. | README does not state a license (credits CC0/MIT parts) — treat as **reference only** | Not addressed | **Highest for Phase 2 technique.** This is the cleanest public description of a compute-FFT ocean with cascades and a cost knob, and every piece is expressible in WGSL compute. |
| **tessarakkt `godot4-oceanfft`** — [repo](https://github.com/tessarakkt/godot4-oceanfft) | Tessendorf FFT + **buoyancy** in compute | see repo | — | Useful second reference for the buoyancy/readback side. |
| **Arnklit `Waterways`** — [repo](https://github.com/Arnklit/Waterways) | **Rivers**: bezier-curve-generated river meshes with baked **flow maps and foam maps**, flow-map-driven UV animation in the shader. | **MIT** | Yes (it is just a mesh + flowmap shader) | High for Phase 3 (rivers). The flow-map idea is also what makes lake/ocean shorelines look alive cheaply. |
| Godot Shaders site: "Toon Water", "Absorption Based Stylized Water", "Toon Style 3D Water (no textures)" | Variants of the Roystan recipe; the no-texture one uses procedural noise (matches Rae's `noise.wgsl`) | Per-shader (mostly CC0/MIT) | Yes | Confirms the recipe is engine-independent. |

### 2.3 Unreal Engine

| Project | Technique | License | Mobile | Fit |
|---|---|---|---|---|
| **UE5 Water plugin** — [docs](https://dev.epicgames.com/documentation/unreal-engine/water-system-in-unreal-engine) | Water *bodies* as first-class objects: infinite **ocean**, polygon **lakes**, spline **rivers** that carve the landscape; **Gerstner** wave sets (cheap, time-only, deterministic — chosen precisely because they need no simulation state), a wave-height query for buoyancy, underwater post-process. | Source-available under the Unreal EULA — **not** permissive; design reference only | Gerstner is cheap; the plugin's material stack is desktop/console-oriented | The **data model** is the thing to copy: *ocean / lake / river are three kinds of one WaterBody*, waves are a parameter set on the body, and "height at (x,y,t)" is a queryable function. That is exactly an ECS component + a system + a query. |

"Unreal-level realistic" in practice means: FFT or many-Gerstner surface,
PBR shading with Fresnel, screen-space refraction with depth absorption, SSR
plus sky reflection, foam, spray, caustics, underwater fog, TAA. All of that is
reachable in WGSL; none of it needs Unreal's code.

### 2.4 Bevy (the closest architectural analogue: ECS + wgpu)

| Project | Technique | License | Mobile/Web | Fit |
|---|---|---|---|---|
| **Neopallium `bevy_water`** — [repo](https://github.com/Neopallium/bevy_water) | A water *material* + tiles: wave sum evaluated in the shader (height, and **normals derived from wave height**), tileable for endless ocean, uses the engine's PBR lighting so it gets shadows for free; ships WebGPU **and** WebGL builds. Roadmap: heightmap support, masks, depth buffer for submerged objects. | MIT/Apache (Rust crate) | Runs in a browser | **High as a structural model**: water as a component on an entity, a plugin (system) that steps time and uploads uniforms, tiles as entities. This is what Rae's version should look like organisationally. |

### 2.5 Raw WebGPU / WGSL references

| Project | What it proves |
|---|---|
| **`inkwell-webgpu-water`** — [repo](https://github.com/siliconjungle/inkwell-webgpu-water) | A standalone **raw-WebGPU** FFT ocean: per-frame frequency-domain evolution, **Stockham inverse FFTs**, four complex fields (displacement + derivatives) transformed **in parallel per cascade**, shoreline and underwater sand. The most direct WGSL evidence that Phase 2 is feasible on WebGPU as-is. |
| **`WebTide`** (Babylon, WebGPU) — [repo](https://github.com/BarthPaleologue/WebTide), [write-up](https://barthpaleologue.github.io/Blog/posts/ocean-simulation-webgpu/) | Tessendorf on WebGPU with a readable blog post — good for on-boarding whoever writes the compute shaders. |
| **`threejs-water-free`** — [site](https://baditaflorin.github.io/threejs-water-free/) | MIT FFT ocean for three.js WebGPU — spectrum code worth reading. |
| **`webgpu-water`** (Evan Wallace port) — [repo](https://github.com/jeantimex/webgpu-water) | Interactive **ripples + caustics** in a small pool — the technique for *local* dynamic waves (splash from a character) if we ever want them. |
| **`WebGPU-Ocean`** (matsuoka-601) | A real **fluid** (particle) simulation — impressive, and the wrong tool for game water. Not recommended. |

---

## 3. The two targets as technique stacks

### 3.1 Stylized / toon water (mobile-first)

Every item below is one texture read or one arithmetic step in a single
fragment shader over a flat grid; the whole thing is one draw call.

| Ingredient | How | Cost |
|---|---|---|
| Colour by depth | `waterDepth = sceneDepth − surfaceDepth` (from the G-buffer depth), `mix(shallowColour, deepColour, saturate(waterDepth / depthMaxDistance))` | 1 depth read |
| Shoreline / intersection foam | `foam = step(noise + waterDepth * k, foamCutoff)` with the noise **panned in the vertex shader** (UV scroll is free there) | 1 noise read |
| Toon ripples | the same noise, **posterised**: `step(threshold, noise)` gives the flat Wind-Waker-style highlight blobs; optionally 2 thresholds for 2 bands | 0 extra reads |
| Surface distortion | offset the noise UV by a panned distortion map (or a second octave of `noise.wgsl`) | 1 read |
| Waves | 1–2 **Gerstner** waves in the vertex shader (displaces vertices, gives a normal) — deterministic, no state | vertex-only |
| Reflection | **none** (Wind Waker) or `sky.radiance(reflect(v, n))` tinted — no planar/SSR | 0–1 sky eval |
| Specular | stepped toon highlight: `step(cutoff, pow(max(dot(n, h), 0), gloss))` | arithmetic |
| Refraction | **off on mobile**; desktop tier: cheap normal-offset read of the lit scene colour | 0 / 1 read |
| Shadows | receive from the existing shadow map if wanted; usually skipped for toon | optional |

Budget target: **≤ 1 ms GPU on a mid phone at native resolution**, one draw,
no compute, no extra render targets beyond the forward pass's own. This is the
same profile as Wind Waker, Genshin's lakes, and every Godot/Unity toon
shader above.

### 3.2 Realistic (Unreal-class) water (desktop; scaled down by tier)

| Ingredient | How | Reference |
|---|---|---|
| Surface | **FFT ocean** in compute: TMA/JONSWAP-style spectrum with directional spreading → Stockham iFFT → displacement (x,y,z) + normal + Jacobian-foam maps; **3 cascades** (e.g. 250 m / 60 m / 8 m tiles) sampled and summed in the vertex/fragment shader | GodotOceanWaves, inkwell-webgpu-water, Tessendorf |
| Mesh | a camera-centred **clip-map / projected grid** so vertex density follows the camera (Crest's LOD idea) | Crest |
| Shading | PBR microfacet + **Fresnel** (Schlick) between reflection and refraction | any |
| Reflection | `sky.radiance(reflect)` always + **SSR** from depth/normal on desktop; planar reflection is *not* recommended (a second scene render is the one thing mobile/tiled GPUs cannot afford, and SSR + sky covers 90% of shots) | Boat Attack chose the same split |
| Refraction + absorption | read lit scene colour offset by the normal; **Beer–Lambert** absorption by water depth (`exp(−depth · absorbColour)`); mask the offset with depth so objects above the surface do not bleed | Boat Attack, Crest |
| Foam | Jacobian-based whitecaps from the FFT + shoreline foam from depth | GodotOceanWaves |
| Spray | existing `lib/Particles.rae` spawned where Jacobian < 0 | GodotOceanWaves |
| Buoyancy / gameplay | a `waterHeightAt(x, y, t)` query. Two options: CPU-side Gerstner mirror (exact for Gerstner, approximate for FFT) or read back the displacement map at low resolution once per N frames | UE5 plugin, godot4-oceanfft |
| Temporal | TAA on (already exists) | — |
| Cost knobs | cascade count, FFT size (128²/256²/512²), **update rate** (skip frames, one cascade per frame), SSR on/off, refraction on/off | GodotOceanWaves' load balancing |

Rough budget on an Apple-Silicon-class desktop GPU for 3 × 256² cascades:
well under a millisecond of compute per update (estimate from the size of the
FFT, not a measurement), plus the surface draw. On the mobile tier the same
system runs the **toon path**, or 1 cascade at 128² updated every other frame.

---

## 4. Fit to Rae's ECS and render graph

### Data model (ECS, folder-per-system)

```
lib/water/Water.rae            # package main: WaterBody, WaterStyle, WaterWaves types
lib/water/WaterSystem.rae      # steps wave time, updates FFT / uniforms, draws
lib/water/waterSurface.wgsl    # the surface shader (toon + realistic paths)
lib/water/waterFft.wgsl        # Phase 2: spectrum + Stockham FFT compute
```

- **`WaterBody` component** on an entity with a `Transform3D`: `kind`
  (ocean / lake / river), extent, `style` (toon / realistic), colours,
  `waves: WaterWaves` (Gerstner set, or FFT spectrum params + cascades),
  foam/absorption parameters. This is UE5's "water body" and bevy_water's
  material, as a plain Rae struct — no `@attributes`, no inheritance.
- **`WaterSystem`** (`app.waterSystem: WaterSystem`, per the #822 convention)
  owns the GPU state (pipelines, cascade textures, uniform buffer) as a
  resource, and its update function takes the narrow state it needs: the
  world's `WaterBody` table, time, camera. It never receives `App`.
- **Buoyancy** is a query, not a callback: `loop let entity, transform: mod
  Transform3D, buoyant: view Buoyant in query2(...)` samples
  `waterHeightAt(...)` and sets Z. Exactly the #807 query-loop shape.
- **Rivers** (Phase 3): a `RiverSpline` component whose system bakes the mesh +
  flow map once (Waterways' approach), then it is just another `WaterBody`
  with a flow-map UV animation.

### Render graph

- New node **`transparentForward`** (name per the plan's `forwardLighting`):
  runs after `lighting`, before `taa`; reads `depth` and a **copy of hdrColor**
  (refraction needs to read the very target it writes — the copy is the
  standard answer), writes `hdrColor` with alpha blending. Water is its first
  client; alpha particles and glass follow for free.
- Phase 2 adds a **`waterFft` compute node** before `transparentForward`
  producing the cascade textures, exactly like the grass compute node.
- No new C: pipelines, textures and dispatches go through `lib/webgpu/*`
  bindings; the C-surface gate stays green.

### Quality tiers

One `WaterBody`, one system; the tier picks the path:

| Tier | Surface | Waves | Reflection | Refraction |
|---|---|---|---|---|
| mobile | toon | 1–2 vertex Gerstner | none / sky | off |
| web | toon or realistic | Gerstner or 1 FFT cascade @128² | sky | on (no SSR) |
| desktop | realistic | 3 FFT cascades @256² | sky + SSR | on |

---

## 5. Recommendation

Implement in this order. Each phase is independently shippable and testable
by the example gate (screenshot cases), and each is a plain Rae package over
the existing bindings.

**Phase 0 — the transparent forward pass (prerequisite, not water).**
DONE: the `transparentForward` node (#842) and its GPU pass (#843) — read
depth + hdrColor copy, alpha-blend into hdrColor, between `lighting` and
`taa`. The walker's splash particles are its first client (#844):
`lib/Particles.rae` now queues each droplet as a constant-size cube with
alpha = remaining-life fraction, drawn at the transparentForward tag instead
of shrinking in the G-buffer — the #829 validation. `examples/117_transparent_pass`
(#845) proves the pass in isolation: a lit deferred scene with a row of alpha
cubes 0.1 -> 0.9 and one half-sunk into the ground, gated in run_examples
(log line + non-blank); the blend itself remains a human check on hardware.

**Phase 1 — stylized toon water (mobile-first). Do this first; it is the
biggest visible win for the least code.** Port the Roystan recipe to WGSL:
one grid mesh, depth-gradient colour, depth-based shoreline foam, posterised
noise ripples, vertex-panned UVs, 1–2 Gerstner waves, sky-radiance tint and a
stepped sun highlight. No compute, no refraction on mobile. Land it as a lake
at the walker's `groundZ` in example 114 (the terrain and sky are already
there) and as a screenshot case. Expose everything as `WaterBody` fields.

**Phase 2 — realistic ocean (desktop tier).** Add the FFT compute
(spectrum → Stockham iFFT → displacement/normal/Jacobian foam, 3 cascades,
update-rate knob), PBR + Fresnel shading, screen-space refraction with
Beer–Lambert absorption, sky + SSR reflection, Jacobian spray through the
existing particle system, and a `waterHeightAt` buoyancy query. Use
GodotOceanWaves and inkwell-webgpu-water as the technique references; use
Crest for the LOD-cascade and gameplay-query API shape.

**Phase 3 — rivers and underwater.** Spline-baked river meshes with flow
maps (Waterways), then an underwater fog/caustics post step.

**Do not:** port Unity or Unreal code (licenses and engine coupling), build
planar reflections (a second scene render is what mobile cannot afford and
SSR + sky replaces it), or reach for particle-fluid simulation for game water.

### Why toon first, and why one system

Toon water is the mobile answer *and* the fallback tier of the realistic
system: the realistic path is the toon path plus an FFT surface, PBR shading
and screen-space effects. Building the toon path first therefore builds the
forward pass, the `WaterBody` data model, the WaterSystem, the shoreline-depth
logic and the example — everything Phase 2 then extends instead of replacing.
That is also why this must be **one** system with a tier switch, not a "mobile
water" and a "desktop water": the ECS data (`WaterBody`) is identical, and
only the shader path and the compute node differ.

### Suggested queue breakdown (not filed — decide first)

1. `[renderer] transparentForward pass` — Phase 0, with the splash-fade test.
2. `[water 1/toon] lib/water package + WaterBody + toon surface WGSL + example 114 lake + screenshot case`.
3. `[water 2/fft] waterFft compute node, cascades, foam, spray`.
4. `[water 2/shading] PBR + Fresnel, refraction + absorption, SSR, buoyancy query`.
5. `[water 3/rivers] RiverSpline bake + flow maps`.

---

## Sources

- Roystan / IronWarrior — Toon Water Shader: <https://github.com/IronWarrior/ToonWaterShader>, tutorial <https://roystan.net/articles/toon-water/>
- Unity — Boat Attack water package (Unity Companion License): <https://github.com/Unity-Technologies/boat-attack-water>
- Crest Ocean System (MIT on GitHub): <https://github.com/wave-harmonic/crest>, docs <https://crest.readthedocs.io/>
- 2Retr0 — GodotOceanWaves (FFT, Stockham, cascades, Jacobian foam): <https://github.com/2Retr0/GodotOceanWaves>
- tessarakkt — godot4-oceanfft (FFT + buoyancy): <https://github.com/tessarakkt/godot4-oceanfft>
- Arnklit — Waterways (MIT; river meshes + flow/foam maps): <https://github.com/Arnklit/Waterways>
- Unreal Engine 5 — Water System docs: <https://dev.epicgames.com/documentation/unreal-engine/water-system-in-unreal-engine>, Gerstner waves: <https://dev.epicgames.com/documentation/unreal-engine/simulating-waves-using-the-water-waves-asset-in-unreal-engine>
- Neopallium — bevy_water (ECS + wgpu, WebGPU/WebGL builds): <https://github.com/Neopallium/bevy_water>
- siliconjungle — inkwell-webgpu-water (raw WebGPU FFT ocean): <https://github.com/siliconjungle/inkwell-webgpu-water>
- BarthPaleologue — WebTide (Tessendorf on WebGPU) and write-up: <https://github.com/BarthPaleologue/WebTide>, <https://barthpaleologue.github.io/Blog/posts/ocean-simulation-webgpu/>
- Popov72 — OceanDemo (Babylon.js WebGPU): <https://github.com/Popov72/OceanDemo>
- threejs-water-free (MIT FFT for three.js WebGPU): <https://baditaflorin.github.io/threejs-water-free/>
- jeantimex — webgpu-water (ripples + caustics): <https://github.com/jeantimex/webgpu-water>
- Godot Shaders — toon / absorption-based stylized water: <https://godotshaders.com/shader/toon-water-shader/>, <https://godotshaders.com/shader/absorption-based-stylized-water/>
- 80.lv — stylized water shader design (mobile variant notes): <https://80.lv/articles/how-to-build-stylized-water-shader-design-implementation-for-nimue>
