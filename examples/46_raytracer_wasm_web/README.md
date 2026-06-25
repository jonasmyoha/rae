# Raytracer — WASM web spike (new-stack step W1)

The first step toward the new cross-platform stack (see
`docs/execution-targets-and-deployment.md` and
`docs/tech-stack-and-dependencies.md`): prove the **Rae → C → WASM** path end to
end with the raytracer's raylib-free compute core, running in a browser tab.

This is **CPU** path tracing compiled to WebAssembly. No raylib, no WebGPU yet
(that's W2/W3). The same `.wasm` runs headless under Node WASI (validation) and
in a browser (canvas), because the program just writes its RGB framebuffer to
stdout and the harness captures it.

## Files

- `scene.rae`, `camera.rae`, `render.rae` — copied verbatim from step 3
  (`42_raytracer_3_materials`); the compute core is already raylib-free.
- `main.rae` — headless render loop; emits each pixel via `fbPixel(r,g,b)`.
- `fb_out.c` — the `fbPixel` extern: writes raw RGB bytes to stdout.
- `web/index.html` — browser harness: a minimal WASI shim captures stdout and
  `putImageData`s it onto a `<canvas>`.

## Toolchain

Needs **wasi-sdk** (a self-contained clang + wasm-ld + wasi-sysroot +
compiler-rt). Download the build for your platform from
<https://github.com/WebAssembly/wasi-sdk/releases> and extract it; point
`WASI_SDK` at it (default `~/.local/wasi-sdk`). `node` is used for headless
validation.

## Build & run

```sh
# from the rae root — build the .wasm (emits C via the compiled backend, then
# wasi-sdk clang -> wasm32-wasip1):
compiler/tools/wasm_build.sh examples/46_raytracer_wasm_web
# -> examples/46_raytracer_wasm_web/build/app.wasm

# headless validation (size + average luminance), the W1 smoke gate:
compiler/tools/wasm_smoke.sh

# in a browser: serve the example dir and open web/index.html
#   (a static server is needed; file:// won't fetch the .wasm)
python3 -m http.server -d examples/46_raytracer_wasm_web 8000
#   then open http://localhost:8000/web/
```

The build output (`build/`) is git-ignored.

## What this validates

- The Rae compute core compiles to WASM through the C backend (the runtime was
  made wasm-aware: signals / threads / flock / fork-exec are guarded out under
  `__wasm__`).
- It runs and produces a correct image (the iconic step-3 materials scene).
- A reusable build script + headless smoke gate exist so the path can't rot.

## Next (W2 / W3)

- **W2**: replace the canvas `putImageData` with a WebGPU texture + a WGSL blit
  shader, behind a minimal Rae Render API (browser-provided WebGPU, no
  Dawn/wgpu dependency).
- **W3**: port `rayColor` into a WGSL **compute** shader (GPU path tracer,
  example step 6).
