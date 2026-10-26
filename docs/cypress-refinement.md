# Mediterranean cypress refinement

**Shared kind 2 remains Mediterranean cypress, Cupressus sempervirens.**
The original thin trunk, narrow column and placement mapping are preserved.
Forest width multiplier remains 0.85 and height remains 8–11.5 m. The six
fixed 138 specimens retain their original transforms: 7.5, 8.2 and 8.9 m,
plus 22.5, 24.6 and 26.7 m tall variants whose widths remain those of the
shorter trees. No tree count or neighbouring species was changed.

[Oregon State](https://landscapeplants.oregonstate.edu/plants/cupressus-sempervirens)
describes the cultivated ascending column and dark scale foliage.
[North Carolina Extension](https://plants.ces.ncsu.edu/plants/cupressus-sempervirens/)
describes slender upright stems and grey to grey-brown furrowed bark.
[Florida Extension](https://ask.ifas.ufl.edu/publication/ST225) describes the
fine-textured scale leaves and upright branching. These guide the species;
the private T04/T05/T07 crops guide quiet connected foliage masses, not its
botanical identity. Unchanged botanical photos and their attribution accompany
the private recovery presentation.

## Matched outline and lighting

| Original opaque patches | Selected upright scale sprays |
|---|---|
| ![Original](images/cypress-refinement/control-front.png) | ![Selected](images/cypress-refinement/front.png) |

Both captures isolate the same original first specimen, seed 73, 7.5 m,
same camera, 10:00 light and zero wind. The old mesh and material remain
available with `RAE_CYPRESS_CONTROL=1`; `fluffyLeavesControl` and
`fluffyWoodControl` remain unchanged. The finer edge replaces the conspicuous
large leaf plates. It does not widen the tree to cover gaps.

| Original tall group | Same tall group, selected |
|---|---|
| ![Original tall](images/cypress-refinement/control-tall.png) | ![Selected tall](images/cypress-refinement/tall.png) |

| Existing evergreen view, original | Same view, selected |
|---|---|
| ![Original grove](images/cypress-refinement/control.png) | ![Selected grove](images/cypress-refinement/named.png) |

The first rendered candidate had an insufficiently tapered, cylindrical body.
It was rejected before the selected geometry. Its matched isolated frame is
retained here so the iteration can be checked:

![Rejected cylinder](images/cypress-refinement/rejected.png)

## Geometry and material

`procgen/Cypress` combines a small continuous inner column with 128 upright
sprays arranged in 16 staggered tiers. Each spray has two crossed, curved
four-ring strips. These stay fixed to their branch; they do not rotate towards
the camera. Their radial normals describe broad foliage volume. The original
512 × 512 alpha texture supplies fine scale-shoot edges with restrained
internal contrast. Three close colour tones avoid individual bright leaves.

The 17-ring inner surface is only 74% of the spray envelope. It uses a constant
UV sampling an opaque quiet part of the same image, making a continuous dark
interior. This avoids card holes without increasing girth, but can expose
smooth vertical strips in close views. The geometry is a deliberate mix of
retained volume and cutout sprays, not the broadleaf camera-facing technique.

The rigid stem retains its original 0.035 unit radius, mild lean and buried
base. Twelve tightly ascending secondary stems reuse the existing curved tube
helper. An original grey-brown albedo adds shallow interrupted vertical bark
ridges. It is painted relief, not displaced geometry. The wood carries no
wind data. Each 16-vertex spray shares a fixed attachment pivot and 0.12
flexibility; the inner column stays rigid.

| Close | Side | Upward |
|---|---|---|
| ![Close](images/cypress-refinement/close.png) | ![Side](images/cypress-refinement/side.png) | ![Upward](images/cypress-refinement/under.png) |

| Bare wood | Backlight | Night |
|---|---|---|
| ![Wood](images/cypress-refinement/bare.png) | ![Backlight](images/cypress-refinement/backlit.png) | ![Night](images/cypress-refinement/night.png) |

The shared surface-image path supplies coverage-preserving mips and the same
0.5 cutoff for colour, shadow and atlas. Existing two-sided foliage lighting
remains in use. No emission, global exposure change or shadow exemption was
added. [Texture provenance](../lib/assets/trees/cypressAssets.md) records the
original images. A six-second fixed forward walk with wind 1 is in the private
presentation; sampled frames retain attached foliage and a stable trunk.

## Seeds, shared forests and lower detail

| Seed 1229 | Seed 2189 |
|---|---|
| ![Seed 1229](images/cypress-refinement/seed1229.png) | ![Seed 2189](images/cypress-refinement/seed2189.png) |

| Actual 143 tree, original | Same tree, selected |
|---|---|
| ![Forest old](images/cypress-refinement/forest-control-front.png) | ![Forest new](images/cypress-refinement/forest-candidate-front.png) |

| Mixed stand | Late afternoon | Night |
|---|---|---|
| ![Mixed stand](images/cypress-refinement/forest-candidate-group.png) | ![Forest afternoon](images/cypress-refinement/forest-candidate-backlit.png) | ![Forest night](images/cypress-refinement/forest-candidate-night.png) |

143 still has 31,090 trees. The target is the existing kind-2 instance at
(-44.0972, 48.8294, 0.6033), height 10.0013 m; the group view retains its
spruce, maple and ginkgo neighbours. 136, 137 and 143 use the same generator,
four shared mesh parts, image layers and atlas subject through `GroveTrees`.

| Prototype cost | Selected |
|---|---:|
| Wood vertices / triangles | 1,042 / 1,632 |
| Foliage vertices / triangles | 2,252 / 1,920 |
| Total vertices / triangles | 3,294 / 3,552 |
| Original foliage triangles, excluding wood | 10,368 |
| Vertex/index payload, calculated | 220,096 bytes |
| Two 512 RGBA images with full mips, calculated | about 2.67 MiB |

These figures exclude allocator overhead and the shared atlas. They are not
CPU/GPU timings. There is no entity, upload or material per spray. Near and
middle levels share their four handles; middle distance does not reduce the
new geometry further. Far impostors are baked from the same textured subject.
The CPU silhouette fixture reports 100% near/middle coverage and 99–100%
coverage for the existing tested billboard mips, within its unchanged tolerance.
The hardware atlas check validates alpha/depth/normals across capture directions.

![Cypress atlas](images/cypress-refinement/atlas.png)

## Reproduction and verification

```sh
python3 tools/capture-spruce-studies.py /tmp/cypress-study --species cypress
python3 tools/capture-spruce-studies.py /tmp/cypress-walk --species cypress --view front --motion
```

The tool keeps its existing spruce default. Cypress study views isolate the
first normal specimen; `named`, `control`, `tall` and `control-tall` show the
ordinary populated grove. `RAE_CYPRESS_BARE=1`, `RAE_CYPRESS_SEED` and
`RAE_CYPRESS_ISOLATE=1` use the same generator for inspection. Static captures
use 60 frames at 1/60 s, wind 0, exposure 1 and a 1280 × 800 window; PNGs are
downsampled from the display screenshot. Normal views use 10:00, backlight
17:15 and night 23:00. Deferred toon, CSM, SSAO, SMAA and bloom remain enabled.
The walk uses 180 frames at 1/30 s and moves 2.5 m. Large sequences stay outside
git. [Grove manifest](images/cypress-refinement/manifest.json) and
[forest manifest](images/cypress-refinement/forest-manifest.json) record cameras,
clocks and hashes. Forest front/group comparisons use an unchanged baseline
binary; grove controls use the preserved old mesh/material in the study binary.

Validation:

- Ten narrow fixtures: nine importing `app3d/GroveTrees`, plus
  `1141_fluffy_foliage`. New `1251_cypress` checks 32 seeds, finite vertices,
  valid indices, unit normals, the actual 0.61-unit culling sphere including
  foliage motion allowance,
  stable pivots, rigid wood, original kind/size mapping and no widened envelope
  beyond the old three-tone union (0.002 unit tolerance).
- Hardware `leafWind`, `surfaceImages` and `impostorBake` pass.
- All four consumer example gates pass: 136 deferred, 137 forward, all fifteen
  138 inspection views and 143 with its 31,090-tree forest.

## Remaining visual limits

This is finer and less segmented than the old opaque plates, but not a claim
of a complete reference-quality match. Close and upward views expose smooth
inner strips and repeated upright spray motifs. Seed changes remain subtle;
the silhouette is more regular than real old cypresses. Secondary wood uses
ordinary tube intersections, mostly hidden inside foliage. Bark is painted,
and middle LOD shares near geometry. Night is naturally almost silhouetted.
The walk and side views do not establish that all edge shimmer is eliminated.
