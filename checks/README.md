# Hardware checks

A check is a small program that tests one part of the runtime on real
hardware: a GPU, or a browser. It prints what it tested and exits 0 when every
test in it passed. It is not an example (there is nothing to look at) and not
a unit test case (the unit suite does not need a GPU device or a browser).

| check | tests |
|---|---|
| `canopyCoverage` | `CanopyBake` + the sprite pass: a far canopy cluster of each tree species covers as many pixels as its trees drawn as single impostors (from 150, 700 and 1,100 m, four directions; within 13%, the phone settings 25% with `RAE_IMPOSTOR_PHONE=1`); `RAE_CANOPY_BENCH=1` times a whole far forest of clusters on the GPU, marched and not |
| `leafWind` | production wind shader: rigid patch distances, fixed pivot, rotated normals, exact calm and actual motion over 64 times |
| `surfaceImages` | opaque material image owner: sRGB conversion, white fallback, two-layer mip array, failed replacement preserving the previous image, resource retirement |
| `gpuArgs` | `gpu/GpuArgs`: a packed argument struct reads back the same through WGSL's own layout |
| `gpuLifetime` | `gpu/GpuLifetime`: recordings, submissions, completion polling, retirement behind in-flight work |
| `gpuManagerTiming` | `gpu/GpuTiming`, the one GPU timing module: per-pass timestamps read back without blocking, named passes, and the last sample published as profiler counters (gpu.<pass>, gpu.total) |
| `gpuReadback` | `gpu/GpuReadback`: concurrent manager-owned readbacks, reuse, cancellation, shutdown |
| `gpuRender` | `gpu/GpuRender`: a manager render pipeline draws into a texture that is read back and compared |
| `gpuResources` | `gpu/GpuResources`: two managers, known buffer contents, stale ids, exhaustion, cleanup |
| `impostorBake` | `ImpostorBake` and `CanopyBake`: every tree species baked into an octahedral impostor atlas on the GPU, then a far canopy of four 40 m cells baked from those impostors; each covers part of its layer and every mip keeps level 0's coverage within 3%; writes the atlas as PNGs to `RAE_IMPOSTOR_PNG_DIR` (default `/tmp/rae-impostor-atlas`; `RAE_IMPOSTOR_PHONE=1` bakes the phone's 4 x 4) |
| `readRequest` | `webgpu/ReadRequest`: independent buffer reads (docs/webgpu-readback.md) |
| `webReadback` | a nonblocking readback completes in a `mainLoop`, natively and in the browser |
| `webTask` | a spawned task that sleeps runs between the frames of a `mainLoop`, natively and in the browser |

## Running

```sh
bash checks/run.sh                              # every check, natively
RAE_CHECK_FILTER="gpuArgs webTask" bash checks/run.sh
make -C compiler checks                         # the same
```

`make test-examples` (the visual examples gate) runs them too when it runs
every example. The two `web*` checks are also built for the browser and run in
headless Chrome by `compiler/tools/wasm_webgpu_smoke.sh`.

A new check is a folder here with a `Main.rae` whose `main` returns 0 only
when it passed. The devtools page lists the checks in their own Checks tab.
