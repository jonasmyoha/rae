# Dynamic resolution & thermal mitigation (#530)

Phones sustain only ~50–65% of their peak GPU: a frame that fits the budget when
cool misses it once the SoC heats and the OS throttles the clock, and fps
collapses (measured on 114: `present` tripled from ~15 ms to ~50 ms after ~18 s,
dropping 60→16 fps — see `docs/profiling.md`). The fix is to keep the *sustained*
frame under budget by shedding work automatically. This is that system.

It is a quality **ladder** driven by a controller (`lib/app3d/render_scale.rae`),
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

`createRenderScaleController(targetFps:)` once; `updateRenderScale(drs:, dt:)`
each frame; `capFrameSleep(drs:, frameStartSec:)` at the end of the render branch;
`drsShadowRes(drs:)` feeds `fitShadowCascades(resolution:)`.

Two inputs drive it:

- **Frame time (`dt`)** — reactive. Under vsync `dt` is coarse (≈16.7/33.3 ms), so
  the loop reacts to *dropped frames*: drop a rung after 4 consecutive frames
  >25% over the target period, raise after ~3 s of good frames, 30-frame cooldown
  between moves so it can't oscillate. When real per-pass **GPU ms (#528)** lands,
  swap `dt` for measured GPU time for a tighter, predictive loop.
- **`ProcessInfo.thermalState`** — proactive. `thermalState()` (Apple: nominal /
  fair / serious / critical) caps the scale *ceiling* before frames drop —
  serious → 0.7, critical → 0.5 + forced 30 fps. Reacting only to dropped frames
  means the device is already hot; the OS tells us sooner.

## The renderer plumbing

- `rae_gb_set_render_scale(s)` / `RAE_RENDER_SCALE` env — the scale knob.
  `rae_gb_offscreen_w/h` return `drawable × scale`, so every deferred target
  re-fits through the existing generation check. The **presentable offscreen
  stays full-size** (the present is a same-size `CopyTextureToTexture`).
- The **composite** fullscreen pass upscales: `GB_COMPOSITE_WGSL` samples the
  reduced lit/TAA source with a linear sampler at `uv = pos.xy*scale/sourceDims`
  (identical to a 1:1 fetch at scale 1.0). See `lib/gbuffer_passes.rae`.
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
import app3d/render_scale
var drs = createRenderScaleController(targetFps: 60.0)   # once
# each frame, after computing dt and before rendering:
updateRenderScale(drs: drs, dt: dt)
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
