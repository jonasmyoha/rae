# 3D Renderer — Step 1

This example is Rae's first minimal 3D renderer category entry. It uses the
current production-ready pieces:

- Compiled (C backend)
- WebGPU compute through `lib/gpu.rae`
- SDL3 window/presentation through `lib/sdl3.rae`
- data-oriented scene packing from Rae ECS-like component lists
- WGSL procedural raymarching with noise, PBR-inspired materials, AO, shadowing,
  and interactive camera controls

It is intentionally not the final raster 3D engine. The future renderer should
extend the WebGPU render binding with depth buffers, MSAA, raster pipelines,
mesh/material resources, SSAO passes, and a shared 2D/3D render graph as
described in `docs/webgpu-3d-renderer.md`.

Run:

```sh
rae run --target compiled examples/107_gpu3d_raymarch/main.rae
```

Controls: W/S forward/back, A/D strafe, E/Q up/down, arrows or left-drag to
look, keys 1/2/3 change render resolution, F12 saves a PNG.
