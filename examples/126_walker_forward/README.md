# 126 — Walker, forward rendering

The forward twin of [114](../114_walker_character), the way
[111](../111_metaballs_forward) is the forward twin of 112. It has the same
walker and crowd, controls, camera, terrain, grass, props, Box3D physics cubes,
shadow cascades, bloom, UI and dynamic resolution. The scene is shaded in one 4x
MSAA pass (`createForwardRendererWithUi`, `lib/RendererForward.rae`,
`lib/ForwardMain.rae`), with the same `shadeSurface` the deferred lighting runs.
Every material 114 uses has a forward version (`lib/ForwardMaterial.rae`,
`lib/forward_entry.wgsl`).

## What differs from 114

| | 126 (forward) | 114 (deferred) |
|---|---|---|
| main pass | one 4x MSAA pass; colour and depth memoryless, resolved into the lit image | G-buffer pass, then a fullscreen lighting pass |
| antialiasing | 4x MSAA on triangle edges; the settings pane's TAA switch does nothing | SMAA by default, TAA from the settings pane |
| SSAO | opt-in, `RAE_FORWARD_SSAO=1` (forward-rae-6): a 1x depth + normal pre-pass (the G-buffer pass, only depth and normals kept) feeds the deferred SSAO, which the forward pass reads. Off by default because it is slower than 114 on the iPhone (below, and docs/forward-msaa-study.md §8) | half-resolution SSAO |
| water | none: no lake, river or sea, no water FFT, no underwater fog (the water surface reads the scene depth) | toon lake, meadow river, FFT sea, underwater fog |
| splash droplets | none (they draw in the transparent pass with the water) | when a character wades |
| shallow water on the ground | still painted by the terrain, and still a wall when deep (it is the island's biome, not the water renderer) | the same, under the water surface |
| terrain colours | 114's palette and grass style (`assets/terrain_palette.wgsl`, `assets/terrain_grass.wgsl`, copied) in a forward composition (`rendererSetForwardTerrainShader`) | the same parts in the G-buffer composition |
| everything else | the same: walker, crowd, controls, camera, grass, props, physics, shadows, bloom, UI, hot-reload state (its own app id), dynamic resolution | |

## Duplication

126 is a copy of 114's program, like 111/112. Of its 5,190 lines of Rae, 5,097
are identical to 114's; the rest drop the water, splashes and TAA settling and
draw the scene in the forward step. The two terrain WGSL parts (46 lines) are
copied because a shader composition resolves its parts next to the declaring
file. The binary assets (4.6 MB: glTF, textures, fonts, UI scenes) are not
copied: 126 reads them from `examples/114_walker_character/assets/`, which the
iOS bundler stages like any cross-example asset path. A change to 114's
gameplay has to be made in both.

## Measured (2026-10-15, iPhone 15 Pro)

`RAE_FRAME_TIMING=1`, dynamic resolution held still (`RAE_DRS=off`), the
phone's default render ceiling (1600x720 long side), last 10 windows of 65 s
runs, alternating, the same thermal state per row:

| display | 114 deferred | 126 forward |
|---|---|---|
| 60 Hz (the shipped build) | 16.7 ms (60 fps, ~10.8 ms idle) | 16.7 ms (60 fps, ~10.3 ms idle) |
| 120 Hz (`RAE_IOS_HIGH_REFRESH=1` build) | 16.4 / 16.0 ms | 11.0 / 11.0 ms |

The 5 ms difference at 120 Hz is the work 126 does not do (SSAO, the water and
SMAA). With the same work the two main passes cost the same; see
docs/forward-msaa-study.md §7. The image was checked on the Mac and from a
screenshot saved on the iPhone.

With SSAO (`RAE_FORWARD_SSAO=1`) on the same phone, alternating with 114 at the
same thermal state: 19.6 / 20.2 ms at 120 Hz (114: 16.0 / 16.0 ms), and 20.4 ms
(~49 fps) on the 60 Hz build, where 114 and 126 without SSAO hold 60 fps. The
pre-pass draws the whole scene a second time, so forward with SSAO is slower
than deferred.
