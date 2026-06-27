# Raytracer — WASM web spike (new-stack steps W1–W2)

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

## Measured: WASM vs native (the W1 perf question)

Same compute core, same scene (480×270, 24 spp, depth 12), Apple M-series,
Node v25 (V8). Render wall-clock (best of 3):

| build | time | notes |
|---|---|---|
| **native** (Apple clang `-O2`) | ~1.18 s | `cc -O2`, linked with frameworks |
| **WASM, single-thread** (wasi-sdk clang `-O2 -msimd128`, Node WASI) | ~1.0 s | ≈1.06 s incl. ~0.06 s node startup |
| **WASM, threaded** (`48_raytracer_wasm_spawn`, `WASM_THREADS=1`, 4 `spawn` band-workers over SharedArrayBuffer) | ~0.61 s | ~1.7× over single-thread |

**Verdict:** WebAssembly runs this float-heavy path tracer at **parity with
native** (within noise; SIMD-vectorised), and Rae `spawn` gives **real
parallelism in the sandbox** (wasm threads on shared memory, no JS-side
band-splitting). The WASM-first deployment thesis holds on the perf axis.

(Reproduce: `compiler/tools/wasm_smoke.sh`; for threaded,
`WASM_THREADS=1 compiler/tools/wasm_build.sh examples/48_raytracer_wasm_spawn`
then `node compiler/tools/wasm_run_threads.mjs …/build/app.wasm`.)

## W2 (done): present via browser WebGPU

`web/index.html` no longer `putImageData`s onto a 2D canvas — it uploads the
WASM framebuffer to a `GPUTexture` and blits it with a WGSL fullscreen-triangle
shader through the browser's own WebGPU (no Dawn/wgpu dependency; the C/WASM
module is unchanged — it still just produces RGB). A canvas2d path remains as a
fallback where WebGPU is absent.

Verified headless on Chrome/Metal: adapter acquired, status reports
`presented via WebGPU blit`, and the screenshot shows the correct scene. Open
`web/` in any WebGPU browser to see it.

## W3 (done): GPU path tracer via WGSL compute

`rayColor` is now a WGSL **compute** shader running in the browser — see
`examples/49_raytracer_webgpu/` (`web/index.html`). The scene is authored in Rae
and emitted to WASM stdout; the browser uploads it to a storage buffer and runs
`raytrace.wgsl` (the *same* shader the native builds use, examples 50/53) as a
compute pipeline through the browser's own WebGPU, then blits `outBuf` to the
canvas. Verified headless on Chrome/Metal. WebGPU-everywhere holds on the
compute axis too: one WGSL, browser + native.
