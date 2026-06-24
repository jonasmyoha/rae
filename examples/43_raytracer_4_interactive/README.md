# Raytracer — Step 4: Interactive camera + continuous progressive rendering

A different architecture from steps 1–3 (which render once). Here the image is
**refined over time**: every frame each pixel gets one more path-traced sample
added into a floating-point **accumulation buffer**, and what you see is the
running average. Hold still and it converges to a clean image; **move the camera
and the accumulator resets**, converging again from the new view. This is the
core loop of the `rae_ui` C++ raytracer.

## Controls (Blender Z-up world — see `../../docs/coordinate-system.md`)

| Key / input | Action |
|---|---|
| `W` / `S` | move forward / back |
| `A` / `D` | strafe left / right |
| `E` / `Q` | move up / down (world +Z) |
| arrow keys | look (yaw / pitch) |
| left-drag | mouse look |

## How it works

- Each frame's sample pass is split across **four band workers** (`spawn
  renderSampleBand`), one real OS thread each in the Compiled backend, returning
  linear-RGB floats that are merged into the accumulator.
- Moving the camera sets `sampleCount = 0`; the next pass overwrites the
  accumulator (no separate clear) and accumulation restarts.
- Internal resolution is modest (640×360, shown 2×) so movement stays
  interactive; it sharpens as samples accumulate. **Step 5** adds a
  preview-vs-final quality toggle and emissive lights.

```bash
rae run --target compiled main.rae   # recommended — real threads
```

See `../40_raytracer_1_basic/README.md` for the full step roadmap.
