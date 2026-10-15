# 126 — Walker, forward rendering

The forward twin of [114](../114_walker_character), the way
[111](../111_metaballs_forward) is the forward twin of 112. It has the same
walker and crowd, controls, camera, terrain, grass, props, Box3D physics cubes,
shadow cascades, SSAO, bloom, UI and dynamic resolution. The scene is shaded in one 4x
MSAA pass (`createForwardRendererWithUi`, `lib/RendererForward.rae`,
`lib/ForwardMain.rae`), with the same `shadeSurface` the deferred lighting runs.
Every material 114 uses has a forward version (`lib/ForwardMaterial.rae`,
`lib/forward_entry.wgsl`).

## What differs from 114

| | 126 (forward) | 114 (deferred) |
|---|---|---|
| main pass | one 4x MSAA pass; colour and depth memoryless, resolved into the lit image | G-buffer pass, then a fullscreen lighting pass |
| antialiasing | 4x MSAA on triangle edges; the settings pane's TAA switch does nothing | SMAA by default, TAA from the settings pane |
| SSAO | from a 1x depth + normal pre-pass (forward-rae-6): the scene is drawn once more into the G-buffer pass, of which only the depth and normals are kept; the deferred SSAO runs over it and the forward pass reads the AO | half-resolution SSAO from the G-buffer |
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
phone's default render ceiling, last 10 windows of 65 s runs, alternating with
114 at the same thermal state:

| display | 114 deferred | 126 forward with SSAO |
|---|---|---|
| 60 Hz (the shipped build) | 16.8 ms (60 fps) | 20.4 ms (~49 fps) |
| 120 Hz (`RAE_IOS_HIGH_REFRESH=1` build) | 16.0 / 16.0 ms | 19.6 / 20.2 ms |

126 is slower than 114: its SSAO needs the scene drawn a second time (the
pre-pass), which costs about 9 ms here; without the SSAO the forward frame took
11.0-11.4 ms (docs/forward-msaa-study.md §8). The image was checked on the Mac
and from a screenshot saved on the iPhone.
