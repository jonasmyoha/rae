# Harebell recovery

The harebell now has curved wiry stems, continuous arched flower attachments,
rounded open bells and quiet lilac shading. Small unequal groups replace the
regular patch spiral. The original generator and materials remain unchanged
as a selectable control; an old plant stands 0.60 m left of the new single
plant in Autumn Grove. Both use seed 2203 and the same height law and scale.

## Reference and choice

The species remains **harebell, Campanula rotundifolia**. The
[Burke Herbarium description and photographs](https://burkeherbarium.org/imagecollection/taxon.php?Taxon=Campanula+rotundifolia)
show slender stems, narrow upper leaves and nodding blue flowers on long
pedicels. Their five short lobes belong to one fused bell. Ben Legler's
[2004 photograph](https://burkeherbarium.org/imagecollection/photo.php?Photo=wtu004368&SourcePage=taxon&Taxon=Campanula+rotundifolia)
particularly shows the curved attachments, broad shoulders and unequal flower
angles used here. G02 informs sparse lilac accents among grass and irregular
groups; it is a composition reference, not a species identification.

The private [recovery presentation](../../../peli_ideoita/environmentReferenceStudy/recoveryReview/index.html#harebell-recovery)
puts actual old/new renders beside G02 and that credited botanical photograph.
The photograph remains outside git and is not a production asset. All new
surfaces are original procedural geometry in Rae.

The candidate uses a thin, two-sided, smoothly shaded bell with a real open
mouth. It retains five broad lobes rather than making separate daisy-like
petals or a solid cone. A cubic curve turns each pedicel downward into the
flower. Its starting tangent follows the stem, including the terminal head.
The transported tube frame comes from the existing shared foliage helpers.
Narrow curved leaves reuse the flower framework's leaf surface at different
proportions; no broad daisy rosette was copied onto this species.

The existing 30–65 cm height law, two-to-five heads per plant, seed and plant
count remain. Bell length is deliberately enlarged to roughly **4–6 cm**
(plus the lips) for readability at the walker camera; the botanical source
records **1.5–3 cm**. This is a stylized scale choice, not botanical accuracy.

## Same-camera comparisons

Single seed **2203**, eleven-plant patch **2371**, shared nine-plant forest
prototype **941**. Each old/new pair has identical camera, placement, seed,
height law, clock, lighting, exposure, ground and grass. The candidate changes
stem curvature, head shape/tilt, materials and grouping within the same patch
envelope. It does not substitute different specimens to disguise those changes.

![Unchanged old plant left, candidate right](images/harebell-recovery/138-candidate-pair.png)

| View | Unchanged rejected control | Candidate |
|---|---|---|
| Close | ![Old close](images/harebell-recovery/138-control-head.png) | ![New close](images/harebell-recovery/138-candidate-head.png) |
| Side | ![Old side](images/harebell-recovery/138-control-side.png) | ![New side](images/harebell-recovery/138-candidate-side.png) |
| Rear, low sun | ![Old rear](images/harebell-recovery/138-control-backlit.png) | ![New rear](images/harebell-recovery/138-candidate-backlit.png) |
| Facing the low western sun | ![Old backlight](images/harebell-recovery/138-control-backlight.png) | ![New backlight](images/harebell-recovery/138-candidate-backlight.png) |
| Patch | ![Old patch](images/harebell-recovery/138-control-patch.png) | ![New patch](images/harebell-recovery/138-candidate-patch.png) |
| Grove eye height | ![Old eye height](images/harebell-recovery/138-control-eye.png) | ![New eye height](images/harebell-recovery/138-candidate-eye.png) |
| Patch, low sun | ![Old patch low sun](images/harebell-recovery/138-control-patch-backlit.png) | ![New patch low sun](images/harebell-recovery/138-candidate-patch-backlit.png) |
| Night | ![Old night](images/harebell-recovery/138-control-night.png) | ![New night](images/harebell-recovery/138-candidate-night.png) |

The side view includes the fixed old plant behind the candidate. Its thick
straight stalk and pointed heads are the control, not extra candidate geometry.
The first patch camera was obstructed by a bush and was discarded. Both versions
were recaptured from the north; no plant or bush moved for that correction.

The candidate's lilac heads have broad gradients and darker interiors. The
night frames darken the entire flower with the scene. Materials have roughness
0.94 and zero emission, with linear green (0.042, 0.135, 0.025) and corolla
(0.31, 0.27, 0.65). Their physical shading stays independent of terrain's toon
switch. No exposure change, global shadow disable or bloom trick was used.

## Shared forest

| View | Old control | Candidate |
|---|---|---|
| Walker eye height | ![Old forest](images/harebell-recovery/143-control-eye.png) | ![New forest](images/harebell-recovery/143-candidate-eye.png) |
| Night | ![Old forest night](images/harebell-recovery/143-control-night.png) | ![New forest night](images/harebell-recovery/143-candidate-night.png) |

This is the existing streamed flower placement near (-18.1647, 5.39762), not an
injected showcase patch. The camera is about 1.7 m above its ground. The same
`WalkerFlowers` implementation serves 136, 137 and 143. Daisies, biome settings,
terrain streaming, flower capacities and tree populations are unchanged.

## Geometry, ownership and shadows

`procgen/HarebellGeometry` creates 13-ring stems, 17-ring curved pedicels and
30-segment, 8-ring bells with smooth surface normals, an inner surface and thin
joined lip. The upper opening carries a small calyx. The model has no
alpha texture or cutout threshold, so there are no new texture/mip resources.
Thin stems still depend on raster coverage and can stipple at distance.

Every green and corolla vertex of a plant uses the same buried root pivot and
flexibility. The existing wind shader rotates the plant coherently; attachments
remain together. It is whole-plant sway, not an elastic stem simulation or
independent petal flutter. Shared walker sway remains 0.62, with per-plant
flexibility 0.18–0.26. No new wind shader or physics system was introduced.

A complete patch still owns exactly two meshes/materials and two bounded
instance batches, regardless of flower count. Meshes are generated once,
uploaded through the existing renderer owner and released at shutdown. Requests
are capped at twenty plants. The fixed old study adds two showcase-only mesh
entities. No entity or material exists per petal or flower head.

Harebell batches now enter the walkers' deferred shadow pass, adding at most
two submissions per cascade. Visible and shadow passes share the same wind
metadata. Both comparison variants use this shadow submission. The forward
walker uses the same caster collection and retains its existing forward lighting
path. The walker gates separate harebell caster instances from the old
400–800 range and require exactly two harebell batches. This preserves the
previous ceiling for other props instead of loosening it for the added shadows.

| Prototype | Control vertices / triangles | Candidate vertices / triangles |
|---|---:|---:|
| Single, seed 2203, five heads | 2,346 / 782 | 3,367 / 5,934 |
| Eleven plants, seed 2371, forty heads | 19,200 / 6,400 | 27,857 / 48,744 |
| Forest nine plants, seed 941, thirty heads | 14,484 / 4,828 | 21,123 / 36,876 |

The extra triangles describe round shoulders, curved attachments and inner
surfaces. This is a close-view quality cost. Flower LOD and wider meadow
composition remain separate work; this pass does not claim distant efficiency.

## Continuous motion and measured cost

The private presentation contains four **six-second, 180-frame, 30 fps** clips:
old and candidate in the grove, old and candidate at forest eye height. Wind is
1.5 with a stationary camera. All frames were captured consecutively; these
are not interpolations between a few stills. Frame strips show the buried root
remaining fixed and the complete head/stem moving together. Motion stays small;
this pass does not add independently bending bells. The videos stay outside
git, with [grove](images/harebell-recovery/138-pair-motion.json) and
[forest](images/harebell-recovery/143-eye-motion.json) manifests in the repo.

Reproduce a clip with the capture command plus `--view pair --reuse-binary
--motion --frames 180 --wind 1.5 --tag=-motion`. For the forest, use `--forest
--view eye`. The shared camera study now accepts `RAE_LANDSCAPE_DISTANCE=0`
for stationary evidence; its existing default remains a 12 m path. No normal
input or camera behaviour changes. `--travel` can request a short camera move.

In paired 900-frame forest runs, post-warm-up whole-frame GPU windows had a
median of **20.101 ms control / 20.607 ms candidate**. Process CPU time was
**10.36 / 10.37 seconds**, peak RSS **270.4 / 272.2 MiB**, and wall time
**18.52 / 18.96 seconds**. Both variants use the new shadow submissions.
These short runs include the frame limiter and desktop load; they are not an
isolated flower GPU benchmark. The [raw measurement summary](images/harebell-recovery/costs.json)
retains per-pass observations. Repeat with `python3 tools/measure-daisy-recovery.py
<forest-binary> /tmp/harebell-cost --species harebell`.

## Reproduce

Use `python3 tools/capture-daisy-recovery.py /tmp/harebell-review --species harebell`
for the grove, and add `--forest` for the existing forest patch. The script's
historical filename remains compatible with the daisy workflow. `--view side
--reuse-binary` repeats a selected angle without recompiling. Rebuild after any
source change. `RAE_HAREBELL_CONTROL=1` restores the old geometry and materials
in both apps and all shared walkers.

Captures use a 1280 × 800 point window (2560 × 1600 native bitmap on this display),
1280 × 800 review PNGs, sixty fixed 1/30-second steps, wind zero, exposure 1,
DRS off, scale 1 and the existing deferred shadows/SSAO/SMAA/bloom. Sun hours
are 10, 16, 17 and 23. The [western backlight pair](images/harebell-recovery/138-backlight.json) uses the same side-facing camera at 17:00. Each process has a timeout; saved window geometry is restored.
Full bitmaps, binaries and logs stay outside git. The manifests record cameras,
source hashes and image hashes: [grove](images/harebell-recovery/138-manifest.json),
[forest](images/harebell-recovery/143-manifest.json).

## Verification

Passed fixtures **1239, 1240 and 1241**: shared grass exclusions, unchanged
daisy coverage, and harebell finite vertices, valid indices, nondegenerate
triangles, unit normals, deterministic arrays, conservative rest bounds,
matching buried root/flexibility across both materials, count cap and zero
emission. Harebell geometry/root checks run over seeds -19, 0, 941, 2203 and
2371 at the twenty-plant cap. The fixture runner checks owned-allocation cleanup.
The preserved control function is byte-identical to its pre-recovery body.

The production **leafWind** GPU check passed. Example gates **136, 137, 138 and
143** passed, including the 136 live-memory test, forward billboards, fourteen
grove views and the 31,090-tree forest. The initial 136/137 failures were the
old shadow-count ceiling; the revised check reports the two harebell batches
separately and retains the previous range for the rest. These functional checks
do not certify the art quality.

The full official `compiler/tools/watch-tests.sh` run finished with **905
passed, 0 failed**. This includes format-check-tree (1,821 canonical files),
example-copy, banned-terms, compiler ownership/cleanup coverage, stress cases
and the suite's ASan/UBSan checks. The visual gates were run separately as
described above. Report links, listed source hashes, image hashes and motion
clip hashes were verified.

Both 900-frame cost runs have identical reported resource counts: initially
962 buffers, 56 textures, 114 views and 150 bind groups; at frame 840, 964,
56, 114 and 155 respectively. The geometry uses larger existing buffers, not
additional materials or texture resources. This short observation is not a
replacement for the separate live-memory gate.

## Remaining visual limits

The bell interiors lack a modeled stigma and stamens. Five lobes and simple
calyces still reveal their procedural construction at very close range. Heads
are larger than the botanical species, and the forest repeats one nine-plant
prototype. Fine stems still stipple when they become subpixel. The surrounding
ground, bushes and mixed-species placement remain much less rich than G02.
Improving this plant does not finish the whole meadow.
