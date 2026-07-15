# Assembly 2026 realtime demo plan

**Status:** Active exploration. The first engine seed is
`examples/107_gpu3d_minimal`.

## Readiness decision

Rae is ready to start a realtime demo, with a deliberately narrow first
vertical slice. The Compiled backend already drives SDL3, native WebGPU compute,
WGSL, persistent GPU buffers, interactive input, PNG output, and data-oriented
scene extraction. That is enough to establish visual direction and measure the
language under a real frame loop.

Rae does not yet have the full raster WebGPU surface required for a conventional
mesh engine. In particular, `lib/gpu.rae` lacks render passes, vertex/index
buffers, depth/MSAA attachments, render pipelines, textures/samplers, and bind
groups. The C-to-WASM browser deployment path is also not complete. These are
project gates, not reasons to defer all demo work.

## Technical direction

Use one data-oriented scene model with two renderer phases:

1. **Compute seed now.** Procedural SDF geometry and noise in WGSL, driven by
   Rae ECS-style components and SDL3 input. This validates iteration speed,
   camera, materials, GPU data extraction, quality tiers, screenshots, and the
   visual language while raster bindings are built.
2. **Forward raster engine next.** Generated vertex/index meshes, depth-tested
   forward PBR, one sun, environment lighting, HDR target, MSAA, tonemap, and UI
   composition. Extend this into a render graph for SSAO and post effects.

The renderer should remain forward-first. It maps well to MSAA and mobile/tiled
GPUs, handles transparency naturally, and avoids committing the first engine to
a high-bandwidth G-buffer. SSAO consumes depth plus reconstructed or explicit
normals in a separate half/full-resolution node. SSAO kernel samples and the
small rotation-noise texture are generated in Rae and uploaded through the thin
WebGPU boundary.

## Scene and ECS model

Ordinary Rae data owns renderer policy:

- `Transform3d`: position, orientation, scale, parent/world transform revision.
- `Camera3d`: projection, exposure, viewport, active state.
- `MeshRenderer`: generated/imported mesh handle, material handle, visibility.
- `Material3d`: base color, metalness, roughness, emissive, texture handles.
- `Light3d`: directional/point/spot data and shadow policy.
- `Environment`: sky, fog, ambient/IBL resources.
- `PostProfile`: AA, SSAO, bloom, tonemap and quality settings.

Systems update simulation/component data first. An O(n) extract pass builds
stable GPU instance/material/light buffers. Render-graph nodes consume those
buffers without reaching back into mutable app state. GPU handles follow the
opaque ownership model documented in `runtime-native-handle-ownership.md` and
`webgpu-resource-management-in-rae.md`.

## Procedural content

The demo should be authored primarily from deterministic seeds so iteration is
fast and the executable remains self-contained:

- Rae generates mesh topology: grids, cubes, UV/icospheres, ribbons and tubes.
- WGSL evaluates inexpensive coherent noise for displacement and materials.
- Rae owns reusable noise tables/seeds and CPU-side generation where mesh
  topology depends on noise.
- Compute handles large particle fields and optional simulation textures.
- Imported assets remain possible later, preferably through an offline glTF
  conversion step into a small Rae-owned runtime format.

Noise should start with hash/value noise, simplex-style or gradient noise, FBM,
ridged FBM, curl noise and domain warping. Each implementation needs deterministic
CPU/GPU tests or reference images; "modern" means useful spectral behavior and
good derivatives, not merely adding more octaves.

## Antialiasing and image quality

Quality tiers share one scene and graph:

- Low/web: dynamic resolution plus FXAA or SMAA, half-resolution SSAO.
- Standard: 4x MSAA forward color/depth, half/full-resolution SSAO, filmic
  tonemap.
- High/desktop: MSAA plus optional temporal accumulation for shader/specular
  aliasing, full-resolution SSAO, bloom and grading.

The compute seed uses deterministic 2x2 supersampling at its highest tier. That
is useful validation, but it is not the final renderer's AA architecture.

## Native and web deployment

Native desktop is the production-first path for the compo: Rae Compiled to C,
SDL3 for window/input, wgpu-native for Metal/Vulkan/D3D12, and WGSL shaders.
This path must remain deterministic and screenshot-testable.

Web uses the same Rae scene/ECS policy and WGSL, but needs the C-to-WASM pipeline,
browser WebGPU bindings, canvas/input integration, asset loading, and a deployment
harness. Web parity is a milestone after the native raster vertical slice; it
must not block visual prototyping, and it must not be claimed until tested in a
browser.

## Examples and demo progression

- **107 GPU3D procedural scene:** the small engine seed. Interactive compute
  raymarching, procedural geometry/noise, component extraction and quality tiers.
- **Raster mesh example:** generated meshes, one camera/light, depth, PBR-ish
  material, HDR and MSAA. This becomes the renderer correctness fixture.
- **Assembly scene:** an intentionally authored, impressive procedural sequence
  with camera choreography, transitions, particles, post effects, audio-reactive
  parameters, and an interactive camera/debug mode.

The impressive example should not become the renderer test. Keep the small
example deterministic and boring enough to diagnose; let the demo optimize for
art direction and spectacle.

## Principal risks

- Expanding raw WebGPU bindings faster than the Rae-owned resource model can
  make lifetime and resize behavior coherent.
- CPU readback in the compute seed limiting frame rate; raster presentation must
  stay on GPU.
- Treating compute AO as SSAO or raymarched SDFs as generated meshes, which would
  conceal missing engine stages.
- Browser deployment consuming the schedule before native visuals are proven.
- Building many rendering features without a coherent art direction or music
  timeline. The actual compo piece needs a visual concept early.

## Definition of a viable compo foundation

Before production on the final scene, Rae must render generated meshes through a
depth-tested HDR forward pass, sustain the target frame rate on the compo machine,
run deterministic screenshots, recover cleanly from resize/resource recreation,
and expose materials, camera, lights and post settings as Rae-owned component
data. WASM is desirable, but not a blocker for the native Assembly submission.
