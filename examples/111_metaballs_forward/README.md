# 111 — Metaballs, forward rendering

The forward twin of [112](../112_metaballs_deferred). It has the same three scenes,
the same panel and the same controls, so the only difference is the main pass.
112 writes a G-buffer and lights it in a fullscreen pass. 111 shades every
surface in one 4x MSAA pass (`createForwardRendererWithSsao`,
`lib/RendererForward.rae` and `lib/ForwardMain.rae`; docs/forward-msaa-study.md).
Both call the same `shadeSurface` (`lib/deferred_shade.wgsl`), so a surface is
lit the same way in both.

Until forward-rae-2 this example drew through the C forward renderer
(`runtime_gpu3d*.c` via `Gpu3d` / `Renderer3d`). It is now Rae over the WebGPU
bindings, like 112.

## What differs from 112

| | 111 (forward) | 112 (deferred) |
|---|---|---|
| main pass | a 1x pre-pass for the SSAO, then one 4x MSAA pass; colour and depth memoryless, resolved into the lit image | G-buffer pass (3 targets + depth), then a fullscreen lighting pass |
| antialiasing | 4x MSAA on triangle edges | SMAA by default; TAA from the settings pane |
| metaball silhouettes | not antialiased: the raymarch runs once per pixel and writes one depth for all 4 samples | smoothed by SMAA / TAA |
| SSAO | half-resolution SSAO over a 1x depth + normal pre-pass (the scene is drawn twice) | half-resolution SSAO over the G-buffer |
| settings pane TAA switch | no effect | turns TAA on |
| sky | drawn last, only where nothing was drawn | computed by the lighting pass where the G-buffer is empty |
| shadows, bloom, render ceiling, composite, UI, present | the same | the same |
| target memory (scene size) | 4 bytes a pixel for the lit image (the 4x targets are memoryless) | G-buffer 16 + lit 4 + SSAO and post targets |

The renderer still allocates the G-buffer and the deferred post targets, because
the G-buffer sets the scene size. So 111 does not yet save that memory.

## Measured (2026-10-15, forward-rae-2, before the SSAO pre-pass)

`RAE_FRAME_TIMING=1`, both at their default render ceiling (2560x1440 on the
desktop, 1600x720 on a phone), scene 1:

| | 111 forward | 112 deferred |
|---|---|---|
| Mac (M1 Max), headless, 2560x1600 window | 6.1 ms a frame (GPU wait 4.8) | 6.7 ms (GPU wait 5.3) |
| iPhone 15 Pro, landscape app, warm | 28.4 ms (35 fps; GPU wait 20.9) | 32.3 ms (31 fps; its GPU frame passes three 120 Hz refreshes, so frames land on 33 ms) |

Mac: average of two runs each. iPhone: last 10 windows of two 75 s runs each.
The phone image was checked from a screenshot saved on the device
(`RAE_SDL_HEADLESS_MS=5000 RAE_GPU2D_SCREENSHOT=shot.bmp`; the iOS launch shim
writes it to the app's Documents folder).
