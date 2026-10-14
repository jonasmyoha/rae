# Dynamic resolution & thermal mitigation (#530)

Phones sustain only ~50–65% of their peak GPU: a frame that fits the budget when
cool misses it once the SoC heats and the OS throttles the clock, and fps
collapses (measured on 114: `present` tripled from ~15 ms to ~50 ms after ~18 s,
dropping 60→16 fps — see `docs/profiling.md`). The fix is to keep the *sustained*
frame under budget by shedding work automatically. This is that system.

It is a quality **ladder** driven by a controller (`lib/app3d/RenderScale.rae`),
plus the renderer plumbing that makes each rung cheap.

## The ladder (highest quality first)

1. **Dynamic resolution** — render the deferred scene (g-buffer, depth pyramid,
   lighting, SSAO, TAA) at a fraction of native and upscale at composite. Scale
   band **1.0 → 0.5** in 0.1 steps. Biggest fragment-cost lever; UI stays full-res.
2. **Shadow-map resolution** — scaled with the render scale (2048 → 1536 → 1024).
   Shadows are 114's *dominant* sustained cost and DRS alone doesn't touch the
   shadow map (a separate fixed-size depth texture), so it gets its own rung.
3. **30 fps frame cap** — only when resolution has bottomed out and frames still
   miss. Halves the GPU duty cycle so the SoC cools; a clean divisor also paces
   smoother than a throttled ~20.

Recovery walks back up: restore the frame rate first, then the resolution, one
step at a time, never above the current thermal ceiling.

## The controller

`createRenderScaleController(targetFps:)` once; `updateRenderScale(drs:,
deltaTime:, profiler:)` each frame; `capFrameSleep(drs:, frameStartSec:)` at the
end of the render branch; `drsShadowRes(drs:)` feeds
`fitShadowCascades(resolution:)`. The decisions are one pure function,
`stepRenderScale(drs:, deltaTime:, therm:)`, which `updateRenderScale` calls with
the OS thermal state; a test drives it frame by frame (fixture 1117).

Two inputs drive it:

- **Frame time (`dt`)** — reactive. Under vsync `dt` is coarse (≈16.7/33.3 ms), so
  the loop reacts to *dropped frames*: drop a rung after 4 consecutive frames
  >25% over the target period, raise after ~3 s of good frames, 30-frame cooldown
  between moves so it can't oscillate. When real per-pass **GPU ms (#528)** lands,
  swap `dt` for measured GPU time for a tighter, predictive loop.
  **Under the 30 fps cap a frame is judged by its WORK time**, not by `dt`:
  `capFrameSleep` pads every capped frame to 33 ms, so the padded `dt` is always
  over budget, and judged by it the cap could never lift (an app that dipped once
  stayed at 30 fps until restarted). `capFrameSleep` records the time from the
  frame's start to just before its padding sleep, and the next update judges the
  frame by that: 180 frames of light work restore 60 fps.
- **`ProcessInfo.thermalState`** — proactive. `thermalState()` (Apple: nominal /
  fair / serious / critical) caps the scale *ceiling* before frames drop —
  serious → 0.7, critical → 0.5 + forced 30 fps. Reacting only to dropped frames
  means the device is already hot; the OS tells us sooner. A cap the heat forced
  lifts when the state falls below critical again (after the cooldown), not on
  frame times — the frames did not cause it.

## The renderer plumbing

- The scale lives in Rae (`lib/RenderResolution.rae`, state on the renderer's
  `GbufferCache`): the app's controller hands its scale to the renderer
  (`setDynamicRenderScale(renderer:, scale: drs.scale)`) each frame, and
  `RAE_RENDER_SCALE` forces it (it is also the controller's maximum).
  `ensureTargets` sizes every deferred target to the surface
  (`rae_g2d_surface_width/height`, the only C involved) at the scene scale and
  re-fits through the existing generation check. The frame's **output
  stays full-size**: the composite upscales into the window's drawable (or the
  full-size offscreen fallback, render-perf-11b).
- **The render-resolution ceiling** (render-perf-3): the most scene pixels an
  app wants. EVERY deferred renderer starts at the device's default
  (`RenderResolution.defaultRenderCeiling`: a mobile OS gets 1600x720, a
  desktop 2560x1440, so a Retina / UHD drawable is not shaded at full size;
  1080p and smaller stay native). An app sets another with
  `setRenderCeiling(renderer:, ceiling:)` or opts out with
  `renderAtNativeResolution(renderer:)`. Its long side is matched to the
  window's long side, so a rotation keeps the scale; an iPhone 15 Pro's
  2556x1179 renders at 0.61 (1561x720). The scene scale is the dynamic scale
  times the ceiling's, in [0.25, 1]; `RAE_RENDER_CEILING=1600x720` forces a
  ceiling for screenshots. Measured headless at 2560x1600 (scale 0.45): 114
  5.6 -> 2.4 ms, 112 9.5 -> 2.7 ms, 119 6.6 -> 3.2 ms per frame. Fixture 1127.
- The **composite** fullscreen pass upscales: `GB_COMPOSITE_WGSL` samples the
  reduced lit/TAA source with a linear sampler at `uv = pos.xy*scale/sourceDims`
  (identical to a 1:1 fetch at scale 1.0); the scale is the cache's
  `sceneScale`. See `lib/GbufferPasses.rae`.
- **FXAA runs before the upscale** (render-perf-4), as its own `antialias`
  pass at the scene size (`lib/deferred_antialias.wgsl`, `antialiasPass`): it
  reads the lit image and writes `targets.antialiased`, which the composite
  then samples. The composite only tone-maps, grades and upscales. FXAA used to
  run inside the composite, after the bilinear upscale: the edges it looked at
  were already smeared, so stair steps survived, and it ran at native size.
  With TAA on, the pass is skipped. Measured headless at 2560x1600, scale 0.6:
  112 4.05 -> 3.8 ms, 119 3.65 -> 3.5 ms, 114 2.8 -> 2.9 ms per frame, and
  the cube and boat edges in 112/119 lose their visible stair steps.
- Aspect is computed from the full drawable, and targets scale uniformly, so the
  projection is unchanged.

## Verifying it

- **Profile capture** (`docs/profiling.md`): the controller emits `render.scale`
  (percent), `render.capFps`, and `thermal.state` counters — plot them alongside
  `fps` and per-pass GPU ms to watch the ladder engage as the device heats.
- **Fixed scale**: `RAE_RENDER_SCALE=0.6 ./compiler/bin/rae run examples/114_walker_character`
  forces a scale for eyeballing the quality/perf trade (validated headless: 0.6
  upscales the scene cleanly, UI crisp, no validation errors).

## Reusing in another app

```rae
open app3d/RenderScale
var drs: RenderScaleController = createRenderScaleController(targetFps: 60.0)   # once
# each frame, after computing deltaTime and before rendering:
updateRenderScale(drs: drs, deltaTime: deltaTime, profiler: profiler)
# feed drsShadowRes(drs: drs) to fitShadowCascades(resolution:) if you cast shadows
# at the end of the render branch:
capFrameSleep(drs: drs, frameStartSec: frameStart)
```

## Not yet done (roadmap)

- **#528/#529** GPU timestamps → replace `dt` with measured GPU ms (blocked on a
  wgpu-native Metal timestampWrites issue; see `docs/profiling.md`).
- **MetalFX temporal upscale** in place of the bilinear composite upscale, for
  sharper output at low scale (FSR2-class; device-gated).
- Vegetation density / LOD scaling and half-rate effects as further rungs.
