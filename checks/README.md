# Hardware checks

A check is a small program that tests one part of the runtime on real
hardware: a GPU, or a browser. It prints what it tested and exits 0 when every
test in it passed. It is not an example (there is nothing to look at) and not
a unit test case (the unit suite does not need a GPU device or a browser).

| check | tests |
|---|---|
| `canopyCoverage` | `CanopyBake` + the sprite pass: a far canopy cluster of each tree species covers as many pixels as its trees drawn as single impostors (from 150, 700 and 1,100 m, four directions; within 13%, the phone settings 25% with `RAE_IMPOSTOR_PHONE=1`); `RAE_CANOPY_BENCH=1` times a whole far forest of clusters on the GPU, marched and not |
| `leafParticles` | `LeafParticles` (docs/leaf-particles-design.md §4): two crowns release 280 leaves over 20 frames (under the frame cap) into the GPU ring in a wind; read back, they fall and drift downwind in their crowns' colors, most land on the ground (the swirls keep some aloft), lie, and are gone after a leaf's life; a leaf past the reach dies at once; a ground lift's leaves rise off the ground; `RAE_LEAF_BENCH=1` times the compute of a full ring in a gale and a breeze instead |
| `leafWind` | production wind shader: rigid patch distances, fixed pivot, rotated normals, exact calm and actual motion over 64 times |
| `windGust` | the gust field (docs/leaf-particles-design.md §2.3): lib/wind.wgsl `windGustAt` on the GPU equals lib/WindGust.rae's at 256 points in a calm, a breeze, a gale and near the drift's wrap (within 0.001) |
| `surfaceImages` | opaque material image owner: sRGB conversion, white fallback, two-layer mip array, failed replacement preserving the previous image, resource retirement |
| `gpuArgs` | `gpu/GpuArgs`: a packed argument struct reads back the same through WGSL's own layout |
| `gpuLifetime` | `gpu/GpuLifetime`: recordings, submissions, completion polling, retirement behind in-flight work |
| `gpuManagerTiming` | `gpu/GpuTiming`, the one GPU timing module: per-pass timestamps read back without blocking, named passes, and the last sample published as profiler counters (gpu.<pass>, gpu.total) |
| `gpuReadback` | `gpu/GpuReadback`: concurrent manager-owned readbacks, reuse, cancellation, shutdown |
| `gpuRender` | `gpu/GpuRender`: a manager render pipeline draws into a texture that is read back and compared |
| `gpuResources` | `gpu/GpuResources`: two managers, known buffer contents, stale ids, exhaustion, cleanup |
| `impostorBake` | `ImpostorBake` and `CanopyBake`: every tree species baked into an octahedral impostor atlas on the GPU, then a far canopy of four 40 m cells baked from those impostors; each covers part of its layer and every mip keeps level 0's coverage within 3%; writes the atlas as PNGs to `RAE_IMPOSTOR_PNG_DIR` (default `/tmp/rae-impostor-atlas`; `RAE_IMPOSTOR_PHONE=1` bakes the phone's 4 x 4) |
| `localNotification` | lib/LocalNotification on a real iPhone: rings with the app closed, the same id replaces, a cancel holds (docs/local-notifications.md). Off iOS it passes with nothing to test |
| `uiAppear` | `Appear` and `Stagger` from a scene alone, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a true middle state. Pictures: docs/images/ui-appear |
| `uiLayoutText` | `AnimateLayout` and `AnimateText` from a scene alone, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of rows gliding, a text fading and a clock rolling (`docs/images/ui-layout-text/`) |
| `uiDialogs` | dialog motion from a card with none of its own, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a dialog opening, closing (`RAE_DIALOG_CHECK=close`), one over another (`over`) and a full page (`page`) (`docs/images/ui-dialogs/`) |
| `uiLists` | list motion through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a ghost, a new row and a glide, or with `RAE_LIST_CHECK=sideways` a change of all the data (`docs/images/ui-lists/`) |
| `uiPageTransitions` | a page transition with a new world per screen, with a `SharedElement` bar that stays and a tile that morphs, through the real pipeline and `pageTransitionDraw`, and `RAE_ANIMATION_HOLD` pictures by `RAE_PAGE_KIND` and `RAE_PAGE_BACK`; `RAE_PAGE_DRAG` runs it from a held finger's share of the way (`docs/images/ui-pages/`) |
| `uiZoom` | a `TransformFx` scale on a whole subtree, painted: a card with a stroke, a shadow, a gradient, text, an icon and a clipped list, at `RAE_UI_ZOOM`. Pictures: docs/images/ui-zoom |
| `webPackageAsset` | a package's shader and declared file, natively and on a page (`compiler/tools/wasm_webgpu_smoke.sh` runs it in headless Chrome) |
| `readRequest` | `webgpu/ReadRequest`: independent buffer reads (docs/webgpu-readback.md) |
| `webRelay` | a player reaches a relay session over WebSocket and gets 100 messages echoed in order by a UDP host: natively (its own relay and host) and in the browser (`compiler/tools/wasm_relay_smoke.sh`, the page's own WebSocket) |
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
