# Raytracer — WebGPU compute (the GPU path tracer)

The per-pixel path tracing runs as a **WGSL compute shader** (`raytrace.wgsl`)
on the GPU — the same shader the native builds run (examples 50/53). The scene
(camera + spheres) is **authored in Rae** (`scene.rae` / `camera.rae` — the same
`buildScene` / `buildCamera` as the CPU steps); this program's only job is to
emit it as a flat little-endian f32 buffer the host uploads to the GPU.

This is the **compute** side of the WebGPU-everywhere thesis: one WGSL shader,
two hosts — native (wgpu-native) and the browser (W3).

## Scene buffer layout (matches `raytrace.wgsl`'s `scene` binding)

```
camera: origin(3) lowerLeft(3) horizontal(3) vertical(3) right(3) up(3) lensRadius(1)  = 19 floats
then per sphere: center(3) radius(1) albedo(3) kind(1) fuzz(1) ior(1)                   = 10 floats
```

The host derives `sphereCount` from the byte length and passes it (with
width/height/samples/maxDepth) in the `Params` uniform. `outBuf` is `W*H` packed
RGBA8 (`R | G<<8 | B<<16 | A<<24`).

## W3: same shader in the browser

`web/index.html` is the browser harness. `main.rae` compiles to WASM (it just
streams the scene floats to stdout via `emitFloat`); the page captures that with
a minimal WASI shim, uploads it to a storage buffer, runs `raytrace.wgsl` as a
compute pipeline through the browser's own WebGPU, copies `outBuf` into a
`rgba8unorm` texture, and blits it to the canvas — no Dawn/wgpu dependency.

```sh
# from the rae root — build the scene-emitter WASM (needs wasi-sdk; see
# examples/46_raytracer_wasm_web/README.md for the toolchain):
compiler/tools/wasm_build.sh examples/49_raytracer_webgpu
# -> examples/49_raytracer_webgpu/build/app.wasm

# serve + open in any WebGPU browser:
python3 -m http.server -d examples/49_raytracer_webgpu 8049
#   then open http://localhost:8049/web/
```

Verified headless on Chrome/Metal: adapter acquired, `raytrace.wgsl` compiles as
a compute pipeline, and the screenshot shows the Rae scene (glass + diffuse +
metal spheres with depth-of-field) rendered entirely on the GPU.

`build/` is git-ignored.
