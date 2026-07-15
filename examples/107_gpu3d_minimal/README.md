# 107 — GPU3D procedural scene

This is Rae's first example in the **3D Renderer** category. It is the smallest
honest engine seed supported by the current API: SDL3 owns the desktop window,
WebGPU runs a WGSL compute renderer, and Rae owns scene/component data, camera
input, extraction, resource handles, quality selection, and presentation.

Run from the `rae` repository root:

```sh
compiler/bin/rae run --target compiled examples/107_gpu3d_minimal/main.rae
```

Controls: `W/S/A/D/E/Q` move, arrow keys or left-drag look, `1/2/3` select
640x360, 960x540, or 1280x720 with 2x2 supersampling, and `F12` saves a PNG.

## What it proves

- A new `.raepack` category creates the **3D Renderer** tab in tooling.
- The Compiled backend can drive an interactive SDL3 + WebGPU frame loop.
- Scene state is data-oriented: identity/renderable, transform, and material
  arrays are extracted into a stable GPU buffer in one O(n) pass.
- WGSL supplies procedural sphere/box/torus geometry, quintic value noise and
  FBM, soft shadows, SDF ambient occlusion, metal/rough PBR-inspired lighting,
  HDR-ish shading, filmic tonemapping, and scalable supersampling.

## Deliberate boundary

This is a compute raymarcher, not yet the final mesh raster engine. The current
`lib/gpu.rae` API does not expose render passes, vertex/index buffers, depth or
MSAA attachments, render pipelines, textures/samplers, or bind groups. Calling
the current AO "SSAO" or the procedural shapes "generated meshes" would hide
that missing foundation.

The next engine stages retain this example's scene extraction boundary while
adding the thin WebGPU raster API, generated mesh assets, forward PBR, depth,
MSAA/post AA, actual depth/normal SSAO, and a render graph. Native desktop is
the first deployment target. The same WGSL and ECS policy are intended for the
web, but Rae's C-to-WASM + WebGPU browser binding is still a separate gate.
