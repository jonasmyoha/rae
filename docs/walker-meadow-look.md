# Walker meadow material

Example 114 uses a local palette and a local grass-colour shader, selected in
`examples/114_walker_character/App.rae`. The terrain still uses the shared biome
field, road overlay, shoreline blending and deferred lighting. The split-sum
ambient BRDF remains in place.

## Material hook

The existing two-argument `rendererSetTerrainPalette(renderer:, path:)` keeps
its behaviour. Its three-argument overload also accepts `grassStylePath:`.
An empty style path selects `lib/terrain_grass.wgsl`, which reproduces the
original scalar grass variation. A supplied file defines:

```wgsl
fn terrainGrassColor(position: vec2<f32>, variation: f32) -> vec3<f32>
```

`position` is world XY in metres; `variation` is the existing detail value in
[0, 1]. Return **linear albedo**, without lighting. Shared noise functions and
palette constants are available. Call the setter before drawing; changing it
invalidates the terrain pipeline. Package the custom WGSL beside the palette.
This hook changes grass colour only, including the grass fringe; it does not
change biome classification, roads, water or textured-material overrides.

114's `assets/terrain_grass.wgsl` blends cooler and warmer greens across broad
patches, then adds soft elongated mottling. Fine variation is intentionally
subtle. It uses three additional noise octaves per terrain fragment and no
new texture or render pass. The default material does not pay for those octaves.
This is a shader-work estimate, not a measured GPU timing result.

## Blades

The shared placement shader rejects `raeBiomePath(position) > 0.10` before
appending a blade. Roads overlay grass, so biome weights alone cannot exclude
them. The small remaining fringe is in the outer, predominantly grass part of
the painted edge; road rejection never scales blade height. The existing
beach/non-grass rejection and distance LOD remain in place.

`GrassViewSettings.clusterStrength` defaults to zero, preserving the existing
uniform distribution. Values toward one use world-space noise to thin blades
between tufts. The parameter uses a previously unused uniform lane; the
uniform buffer size is unchanged. Only apps enabling clustering evaluate that
extra noise. 114 uses:

| Setting | Before | Meadow |
|---|---:|---:|
| Height scale | 1.0 | 0.55 |
| Width scale | 1.0 | 1.8 |
| Density scale | 1.0 | 0.55 |
| Cluster strength | 0.0 | 0.8 |
| Tip multiplier | 1.3 | 1.25 |
| Wind strength | 1.0 | 0.45 |

The root colour in `walkerGroundColor()` matches the palette's base grass
albedo, `(0.080, 0.245, 0.015)`. The procedural ground varies around that base.
Blades remain simple tapered cards. Trees, rocks and character assets are
unchanged; this material pass does not replace the placeholder silhouettes.

## Screenshot comparison

Captured the normal initial camera with the overlay disabled and a three-second
headless lifetime. Mean sRGB samples use normalized image rectangles:

| Region (left, top, right, bottom) | Before | Meadow |
|---|---|---|
| Distant meadow (.23, .30, .40, .34) | (60, 148, 70) | (69, 136, 64) |
| Near meadow (.31, .62, .40, .69) | (73, 134, 60) | (76, 122, 58) |
| Road (.54, .46, .72, .52) | (131, 101, 69) | (131, 101, 69) |

These are shaded, mixed-material image samples, not albedo values or an exact
match to the reference grass `(73, 133, 54)`. In the road blade strip
(.08, .38, .42, .425), the fraction of green pixels (`G > 1.35R` and
`G > 1.4B`) fell from 60.85% to 0%. Animation and exposure settling can cause
small differences between captures. No additional fog or colour grade was added.

Validation: the 111, 112, 114, 115 and 119 example gates pass (five passed,
zero failed), including 119's tier/cadence/invalidation/shutdown checks. Runs
used a 600-second outer timeout; individual screenshot runs used 180 seconds.
The touched Rae files pass `rae format --check`; renderer C-surface and
banned-terms gates pass. No C runtime changes were needed.
