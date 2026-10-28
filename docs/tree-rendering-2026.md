# Close-up tree rendering: 2026 review and Rae birch

Research date: 2026-10-23. Scope: an attractive silver birch at LOD0 in
Autumn Grove and the shared walker forests. This is a design for Rae's
existing WebGPU renderers, not a claim that one method wins on every GPU.
The sources below are engine documentation, developer publications and
botanical references. Published demonstrations are evidence of those engines'
capabilities, not performance measurements for Rae.

## 1. What makes a birch read as a birch

A slim pale stem must remain visible through a light crown. Main branches rise;
slender outer twigs hang. Leaves are small, pointed diamonds, with irregular
spacing. Bark needs horizontal dark lenticels and occasional larger scars,
not evenly spaced black rings. Older bases can be darker and rougher.
Autumn colors should move from soft green through lime to warm gold.
These identifiers follow [Kew's species account](https://powo.science.kew.org/taxon/urn%3Alsid%3Aipni.org%3Anames%3A295174-1/general-information)
and the [Woodland Trust's illustrated silver-birch guide](https://www.woodlandtrust.org.uk/trees-woods-and-wildlife/british-trees/a-z-of-british-trees/silver-birch/).

The current art direction uses camera-facing leafy clusters with broad smooth lighting,
a slightly crooked leader and bright white bark with sparse, thick black scars.
The original individual-leaf geometry was rejected after review.
The three supplied screenshots favor broad foliage masses and leafy outlines.
They are visual references; still images do not identify an engine or wind algorithm. A uniformly filled sphere, cone or identical stack of canopy discs would
lose the species even with a good texture. The tree should look pleasant in
side light, against the sky and from underneath, not only in its thumbnail.

## 2. What current rendering approaches offer

| Approach | Close-up strengths | Costs and failure modes | Rae decision |
| --- | --- | --- | --- |
| Opaque shaped leaves | Real silhouette, folds, depth and stable shadows; no transparent corners | More vertices and tiny triangles; layered geometry still overdraws | Rejected for this birch after visual review |
| Trimmed alpha-tested branch cards | Efficient authoring and fewer vertices; detailed leaf veins from an atlas | Cutout overdraw, flat views, edge aliasing, mip thinning, shadow-mask matching | Use camera-facing clusters with crown normals for this LOD0 |
| Broad opaque canopy patches | Small geometry budget, calm painted masses | Reads as generic clumps; loses birch twigs and airiness close up | Keep for other species; not this birch's main form |
| Geometry plus cards | Shape at silhouette, cheap dense interior | Two material paths, harder lighting and shadow consistency | Useful later if profiling justifies it |
| Hierarchical virtualized geometry and voxels | Detailed nearby leaves, compact repeated parts, distant aggregate coverage | Requires specialized build, streaming, culling and rasterization infrastructure | Research direction, not this implementation |
| Whole-tree impostors | Very small distant geometry cost | Parallax, silhouette and lighting errors up close | Existing forest fallback only; not LOD0 |

### 2.1 Modeled leaves versus masked cards

Epic's [Nanite content guidance](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content)
explains why foliage is difficult: separated leaves form aggregate geometry,
and many layers can cover a pixel before they occlude the background. Opaque
geometry removes transparent card corners but does not make a dense crown free.
Its guidance also warns that excessive split normals increase vertex cost.

The [engine team's 5.1 rendering announcement](https://www.fortnite.com/news/drop-into-the-next-generation-of-fortnite-battle-royale-powered-by-unreal-engine-5-1)
describes individually modeled vegetation in a shipping game. That is evidence
that geometric leaves can be a practical production choice, not a budget to
copy into an ordinary WebGPU renderer. Rae should start with a bounded mesh
and count both vertices and shadow draws.

The current choice is a four-vertex camera-facing quad carrying a foliage-cluster
color/alpha texture. Four to six offset clusters follow each twig group.
Normals follow the broader crown and rotate with the wind, independently of
the viewing camera. This replaces the rejected fixed curved cards, which
looked like hanging sheets when seen from the side. Alpha below 0.5 is
discarded in color, shadow and impostor-bake passes. There is no sorted
transparent blending. See §8 for the complete geometry and shadow contract.

The image uses nearly merged painted leaf shapes with an irregular cluster
silhouette, transparent gaps and little internal contrast. Interior detail
must not become a separate bright/dark pattern for every leaf. The clump lighting follows
the general strategy described in [the SpeedTree rendering chapter](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering).
This is an implementation of that general technique, not a reconstruction of
the unknown renderer in the supplied screenshots.

### 2.2 The newer virtualized foliage approach

Epic's [5.8 Nanite Foliage documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)
still labels the feature Experimental. It combines repeated geometry parts,
voxel representations for distant aggregate detail, and skinning for animation.
The important design lesson is that a forest's geometry, storage, animation
and distance representation must be designed together. It is not simply
"increase the leaf count and enable a checkbox" in an unrelated engine.

Rae already pools complete prototypes. Reusing branch submeshes inside a tree
could further reduce authoring/storage duplication, but would need a renderer
that batches those parts without multiplying draws. Do not add millions of
leaf triangles in anticipation of infrastructure Rae does not have.

The [5.8 release notes](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes?lang=en-US)
also document experimental growth, seed generation, grafting, trimming and
trunk-material tools. Those are useful authoring ideas. Rae's deterministic
branch graph and texture-mapped tubes are a smaller implementation of the
relevant authoring requirements, not an implementation of that engine.

### 2.3 Lighting leaves without making them glow at night

[SpeedTree's card-generator documentation](https://docs9.speedtree.com/modeler/doku.php?id=card_generator)
describes adjusting normals towards an ancestor/cluster envelope to improve
lighting of real-time leaf groups. Excessive adjustment makes a canopy look
like a balloon. Keep the bend small enough that individual folds remain
visible; rotate shading normals with the geometry during wind animation.

Epic documents light transmission in its
[two-sided foliage model](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models?application_version=4.27),
and the current [Substrate overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine?application_version=5.8)
provides two-sided wrap as one scattering option. The practical problem is
black leaf backs under a conventional opaque lighting model.

This birch uses the existing toon material with mildly upward-biased smooth
leaf normals, identical on the two geometric sides. That is an art-directed
approximation, **not physical transmission**. No emissive light is added;
leaves still respond to the scene sun and ambient light. A later thin-leaf
material should store transmission/thickness explicitly, multiply backlighting
by the sun and shadow visibility, and work in deferred and forward shading.
A constant green emission term would be the wrong solution.

### 2.4 Wind and temporal stability

The [GPU Gems vegetation chapter](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)
separates main structural bending from leaf detail and describes independent
stiffness/phase controls. Rae now has intact leaf-pivot rotations, shared with
its shadow vertex stage. Birch sprays share a twig attachment as their pivot; all patches in a spray
rotate together, with their normals. Their signed flexibility selects a slow,
visible branch response: fixed per-tree phase, fixed per-branch phase, and no
noise sampled at a coordinate multiplied by changing wind direction. Wood
stays rigid. Other species retain their current wind response. This follows
[SpeedTree's wind guidance](https://docs8.speedtree.com/modeler/doku.php?id=wind_vfx)
on restrained bending and independent branch response; it is not a physical
branch simulation. GPU checks cover rigidity, calm wind, fixed attachments,
normal rotation and short time-step continuity for both response modes. Reintroducing independent displacement
of each leaf vertex would undo the recent wind improvement.

A future branch system needs actual parent-child transforms and conservative
animated bounds. Motion vectors must evaluate the same branch pose at the
previous frame time. Reducing animation with distance is also a shadow-budget
choice: [Epic's shadow documentation](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine)
notes that deforming geometry invalidates cached shadow pages. Rae uses a
different shadow system, but should measure animation and shadow work together.

### 2.5 Forests are a placement and ownership problem too

[Guerrilla's procedural placement presentation](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)
describes rule-driven environments populated around the player. For Rae the
useful idea is stable world-space rules, not copying that engine's GPU system.
Birches should appear in small stands mixed into suitable terrain, with shared
mesh/material resources. No per-tree texture allocation or per-frame mesh
regeneration is needed. Existing PropField cells, pooled colliders and instance
batches provide the appropriate ownership and culling structure.

## 3. Implementation contract chosen here

### Geometry and data

- `procgen/Birch.rae` generates `BirchTree { wood, twigWood, twigs }` from a seed and
  height. A twig has three curve control points. Construction is ordinary Rae.
- Continuous ring strips form the slender stem and curved branches. Ring
  vertices share radial normals; the UV seam has duplicate vertices at U=0/1.
  Bark V follows branch length, avoiding a new stretched texture per triangle.
- Twenty-six irregularly phased limbs carry five hanging twigs each. Each limb
  has one or two unequal hanging sprays, with seeded omissions on outer twigs.
  Sprays vary in width, depth, length and patch count (four to six). Each spray
  shares a fixed twig pivot. Three color meshes keep the existing batching.
- The shared forest registers one prototype. Grove placements vary yaw and
  scale. A future set of two or three seed variants can improve repetition
  without making every tree unique on the GPU.
- Five parts separate pale bark, dark outer twigs and three leaf tones.
  Leaves use the optional pivot and card-center streams; the wood uses the
  ordinary static vertex layout.

### Bark texture and materials

`lib/assets/trees/birchBark.png` and `birchLeaves.png` are original generated
512 x 512 PNGs. The bark revision balances medium dark lenticels, occasional
larger scars and subtle pale papery detail on white. It is a visual midpoint
between the original fine texture and the overly sparse eight-mark revision. Bark V repeats 0.34 times per meter rather
than 1.4, making the markings about four times longer vertically. The leaf
texture has low-contrast painted green masses, irregular margins and gaps.

Generation briefs (built-in image generator; originals retained outside the
repo, resized for runtime):

- Bark edit: use the original detailed and the sparse bold texture as equal
  references; halfway in detail density and realism; roughly 35–45 varied
  horizontal scars, bright white base, deep charcoal marks, subtle papery
  peeling; no beige cast or baked light; tileable. The output has naturally
  varied marks rather than a mathematically exact 50% pixel blend.
- Current leaf generation: a broad asymmetrical cloud fragment with uneven
  lobes, a missing bite, small gaps and a finely broken silhouette of small
  pointed diamond leaves. Merge the interior into matte mid-green paint;
  no veins, stems, outlines, highlights, baked shadow or ball shading.
  Transparent background and margins. Replace the earlier hanging-spray image.
- Both were made with the built-in image generator and resized to 512 square.
  Final assets are the two `lib/assets/trees/` paths above.

`SurfaceImages` owns a renderer-local RGBA image array, view and repeating
trilinear sampler. RGB is decoded to linear light before mip generation.
Mip color is weighted by alpha to avoid dark borders from clear texels;
alpha is rescaled to keep the covered fraction at cutoff 0.5 as closely as
the lower resolution permits. It cannot preserve an arbitrary fraction in
a one-pixel mip. RGB is stored as sRGB for hardware decode; alpha stays linear.

`rendererLoadSurfaceImage(renderer, path)` returns the layer;
`Material3d.colorTextureLayer = -1` keeps plain color. Forward and deferred
color, foliage shadows, and the existing impostor bake use the same alpha
cut. Images load once and are shared by every instance. Shadow bindings track
image view/sampler identity when the array is replaced; old objects retire
through GpuResources. No per-tree textures or per-frame image uploads. Cutout shadows use the
foliage vertex layout: upload leaf cards through `uploadFoliage`, including
when their wind strength is zero. Ordinary bark meshes stay opaque casters.

The bounded API accepts at most 16 square 512-pixel PNGs. The layer belongs to
its renderer. Failure preserves the previous array. A renderer without images
binds a one-texel white fallback. Existing untextured impostor callers retain
that fallback through `bakeImpostorAtlas`; textured callers pass their image
owner explicitly to `bakeTexturedImpostorAtlas` for the synchronous startup bake.

### Population and scope

- Autumn Grove has a small birch stand and a `birches` inspection view (key 9).
- `app3d/GroveTrees` adds the species to shared deterministic placement, so
  deferred walker 136, forward walker 137 and huge forest 143 receive it.
- Existing forest distance machinery remains active. The middle representation
  initially reuses the close-up birch shape; the existing automatic
  impostor bake now samples bark and leaf images, including their alpha masks.
  This keeps the running forest correct; it is not a finished birch LOD project.
- A dedicated reduced middle mesh and further aggregate-canopy tuning are
  deferred. Never render LOD0 for every tree to the horizon.
- No new C runtime logic, language feature, per-leaf entity or per-frame CPU
  leaf update is required.

## 4. Verification and risks

Check the generated mesh's deterministic output, indices, dimensions, normals,
UV seam and pivot count. Check independent seeds produce different stems and
that species selection actually includes birch. Capture the new grove view,
a close bark view, and forest integration in both rendering paths. Check night
lighting through normal materials rather than a brightness hack. Run the
existing affected example gates and the compiler suite with explicit timeouts.

Count meshes, vertices, triangles and material batches. Compare a fixed camera
and fixed population when profiling; a changed distribution is not a valid
isolated performance comparison. Texture memory for one 512-square RGBA8 mip
chain is about 1.33 MiB, shared by all birch instances in a renderer.

The first textured prototype had 7,943 vertices and 12,084 triangles across
five parts, including reverse-wound patch faces. The subsequent hanging-spray
revision varies the patch count by seed and remains within the fixture budget.
Both use fewer vertices than the
17,875-vertex individual-leaf version, but alpha-tested overdraw can still be
expensive. Vertex/index storage is about 0.42 MiB per shared prototype. Both
512-square RGBA8 mip chains total about 2.67 MiB, shared by every instance.
These are data budgets, not a claimed frame-time improvement.

Main risks are cutout overdraw, mip thinning, regular clump placement, a bark
wrap seam, flat backlighting and self-shadow detail. There is no new
transmission pass, branch simulation or custom distant representation.

## 5. Original implementation validation (before this revision)

- Compiler suite through `watch-tests.sh`, with a 900-second timeout:
  **896 passed, 0 failed**, including `1235_birch` and the existing nine-species
  LOD coverage case. Birch is deliberately outside that reduced-LOD test;
  its deterministic geometry, normals, indices, UV seam and leaf attachments
  have their own fixture.
- `RAE_CHECK_FILTER=surfaceImages bash checks/run.sh`, bounded to 120 seconds:
  passed on hardware. Checks all 256 sRGB round trips, linear-light midpoint
  encoding, fallback size, two array layers and ten mips, failed replacement,
  the layer limit and retirement. This checks resource metadata and lifecycle;
  the render captures check the actual sampled appearance.
- Example gates 136, 137, 138 and 143, bounded to 900 seconds: **4 passed**.
  Includes the forward path, nine grove views, walker physics and memory-growth
  checks, and 31,090 trees in 829 far-canopy clusters in the large forest.
- Inspected the grove before and after the bark/twig refinement, both walker
  renderers and the deferred walker's night lighting. White bark and leaves darken with the scene;
  neither material emits light. The grove preview is stored at
  `examples/138_procgen_showcase/references/Birches.png`.

To visit the stand, run example 138 and press **9**, or start with
`RAE_GROVE_VIEW=birches`. Both walkers and the huge forest receive the species
through `app3d/GroveTrees`; no separate generator copies are kept in examples.

## 6. First patch revision validation (before hanging sprays)

The updated geometry fixture checks seeded stems, valid indices and unit
normals, UV seams, 312 patches, shared clump pivots and alpha-weighted mip
color/coverage. The wind hardware check exercises both patch responses.
The material-image check loads distinct bark and leaf layers. The impostor
hardware check compares the same plane with and without the leaf alpha mask,
and bakes textured birch in both ordinary and foliage vertex layouts.

The final compiler run passed **896 tests, 0 failed** through `watch-tests.sh`
with a 900-second timeout. The first run found a missing explicit renderer
import; that was corrected before this successful rerun. The four example
gates (136, 137, 138, 143) passed with a 900-second timeout, including all
nine grove views, the walker's live memory check, and 31,090 forest trees.
The hardware image and wind checks passed with explicit timeouts. The GPU
bake checks include masked coverage and the original untextured fallback;
the final bake run also passed AddressSanitizer and UndefinedBehaviorSanitizer
(leak detection disabled). A separate repeated-list-insertion compiler bug
found in the test setup is recorded in QUEUE; each test owner now receives
a freshly constructed subject.

Inspected final captures in Autumn Grove and both walker render paths. The
grove preview is `examples/138_procgen_showcase/references/Birches.png`;
key **9** opens the birch stand. Its camera now frames the full crowns.

## 7. Animation-background refinement

The round clumps and separately colored leaves were too artificial. Shape,
texture and shading need different scales: the silhouette can show leaf tips,
but large groups should share their light and dark values.

[Harry Alisavakis’s foliage breakdown](https://halisavakis.com/my-take-on-shaders-stylized-tree-leaves/)
uses masked leaves and custom stylized lighting, and removes normal-map detail
and specular to reduce visual noise. It also discusses UV-based motion and
backlighting. We adopt the low-noise albedo/normal direction; UV warping would
work against our fixed twig attachment and is not used here. This does not
copy that shader or introduce emission that could glow at night.

[Ascalon’s tree-shader study](https://www.aversionofreality.com/blog/2022/8/7/stylized-tree-shader)
separates custom normals, world lighting, color variation and density. Our
choice is to derive most normal direction from the broader crown, then add
only a small local bend. Geometry becomes elongated, offset sprays along
actual twigs, with uneven dimensions and missing outer sprays. This keeps
full areas alongside open areas without making a set of shaded balls.

Wind uses the existing negative-flexibility path, now with a stronger slow
swell and independent twig motion. Flexibility varies from 0.8 to 1.2 per
spray. The angular response is capped at 0.42 radians (24 degrees), with
ordinary breezes below that limit. Every patch in a spray keeps the same
pivot, motion and rotated normal. Wood stays fixed. Positive-flexibility
species keep their existing response. This remains art-directed motion,
not a branch-physics simulation.

The references are low-resolution stills; they do not identify their engine
or establish a wind algorithm. This revision aims for their broad painted
value groups while retaining Rae's day/night lighting and alpha-tested
shadow path. A dedicated foliage light-transmission model could improve
backlighting later, but is outside this asset and generator refinement.

Validation for this refinement: cases `1228_tree_levels_cover_alike`,
`1232_wind` and `1235_birch` passed with a 120-second bound. Hardware checks
`leafWind`, `surfaceImages` and `impostorBake` passed with 120/150-second
per-check bounds. Example gates 136, 137, 138 and 143 passed (two bounded
runs, 900 and 600 seconds), including the walker's running memory check and
the forest's 31,090 trees. The example-copy gate and formatting checks passed.
Five timed grove captures were taken across about five seconds and inspected
for silhouette changes. The revised wind check requires a 70 cm spray tip
to move more than 5 cm in at least 12 of 32 sampled times, alongside the
existing rigidity, attachment, normal and continuity checks. These are
correctness and visual checks, not a frame-time performance comparison.

## 8. Camera-facing clusters replace fixed sheets

The fixed patches still exposed thin edges and conspicuous hanging planes.
The replacement is a shared renderer feature, used by the same birch generator
in the grove and all three walker variants. Existing bark is unchanged.

`MeshData.leafCards` holds center xyz and an enabled flag per vertex. The rest
corner is stored in the mesh's local X/Z plane and UV remains 0..1. The existing
wind pivot stays separate: wind moves the cluster center about the twig root,
then the vertex shader expands its corners along camera right/up. This avoids
both edge-on cards and stretching a textured leaf shape with spatial noise.

The forward and deferred paths derive axes from the current view-projection.
Motion vectors expand the previous corner using the previous camera and model,
with the previous wind. Lighting normals describe the crown, not the billboard
plane; rotating the camera alone therefore does not rotate the lighting normal.
The sun shadow pass receives the viewing camera's axes through `ShadowCascades`
and expands the same cards before projecting into the light. It must not turn
them toward the sun. Cascade fitting currently uses the renderer's Z-up camera
convention. A future rolled camera needs its actual view axes supplied there.
The existing distant atlas bake poses cards toward each atlas view, and subject
bounds include the card corner radius plus the maximum capped wind travel.

Foliage vertices are now 64 bytes: position/normal/UV (32), wind pivot and
response (16), card center/enabled (16). Ordinary meshes remain 32 bytes.
Other foliage has a zero enabled flag and retains its rigid-patch motion,
but pays the extra 16 bytes per vertex. Birch reduces each patch from nine
vertices to four, so its vertex storage falls from 432 to 256 bytes per patch.
Draw batching and per-instance ownership are unchanged. Cutout overdraw and
camera-dependent shadow silhouettes remain trade-offs; no frame-time speedup
is claimed. Dedicated reduced birch geometry is still future work.

The shared GPU wind check also verifies 64 viewing bases: quad dimensions,
camera-facing orientation and axis recovery under unequal projection scales.
The generator fixture checks matching centers, pivots and valid UVs per quad.
The atlas check preserves the ordinary-species coverage comparison and checks
that camera-facing birch coverage stays within its bounded range.

The grove's front, side (yaw 0.9) and elevated (yaw -1.2, pitch 0.5) captures
were inspected, along with five timed wind frames. `RAE_CAMERA_AT` now uses
the existing shared camera override in the grove, so those views can be
repeated without editing its scene. The main reference is `Birches.png`.

Validation: full suite 896 passed under a 900-second timeout; GPU checks
`leafWind`, `surfaceImages` and `impostorBake` passed with 150-second per-check
bounds; visual gates 136, 137, 138 and 143 passed under a 900-second bound.
This includes the walker's running-memory check, the forward path, all nine
grove views and the large forest. These are correctness checks, not a GPU
performance comparison.

## 9. Rejected birch hybrid-card prototype (historical)

**Rejected in review on 2026-10-25.** This section records the
prior implementation, not the current recommendation. See section 11 and
[the recovery review](landscape-recovery-review.md).

The current proof keeps the shipped twig pivots, card centers and crown
normals. It changes only silver birch. Every spray now follows its source twig
and mixes two orientations: an interior card remains in the branch plane while
one or two smaller silhouette cards face the camera. The branch-plane cards
hold depth during an orbit; the view-facing cards prevent the outer contour
from collapsing to a line. Both use the same broad crown normal, so rotating a
card does not turn its painted light pattern or make each quad light as an
unrelated leaf.

The original 512-square atlas has four independently painted cells: broad
asymmetric, narrow drooping, sparse forked and loose edge sprays. A 32-pixel
transparent cross isolates the cells before mip generation. The albedo keeps
large low-contrast green masses and reserves small marks for the alpha edge.
It contains no emission. The ordinary surface-image path supplies
alpha-weighted, coverage-preserving mips, and the octahedral bake supplies its
own coverage-preserving distant mips.

The selected seed produces 113 quads: 69 camera-facing edge cards and 44
branch-plane interior cards, or 452 vertices and 226 triangles across the three
tone meshes. The atlas's complete RGBA8 mip chain is about 1.33 MiB and remains
shared by every birch. Foliage pipelines now render both card sides in deferred,
forward and shadow passes. This is a foliage-pipeline capability; the hybrid
placement itself remains birch-only. No shadow pass is disabled and the leaves
keep their ordinary sun, ambient and shadow response.

The remaining close-view limits are visible scalloping inside some painted
masses, repeated branch hints where the atlas overlaps real twigs, and card
intersections at exact grazing angles. There is no leaf transmission model or
dedicated middle-distance birch mesh. Wind still rotates a whole spray around
its fixed twig pivot. These are inputs to the later silver-birch species pass,
not reasons to copy this prototype unchanged to every tree.

The 1235 fixture validates four atlas cells, the transparent cell gutter,
coverage-preserving mips, finite geometry, card pivots and conservative bounds
across four seeds. Hardware checks `surfaceImages`, `leafWind` and
`impostorBake` passed. Fixed front, side, elevated and backlit grove captures
and three forest-walk positions were inspected for disappearing sheets,
rotation and shimmer. Example gates 136, 137, 138 and 143 passed. These sampled
views did not show a new pop, but they do not replace a future continuous-motion
capture when the final species and its middle-distance level are tuned.

## 10. Rejected silver-birch finish pass (historical)

**The finished claim was wrong.** Technical checks did not establish the desired
appearance. This result was rejected on 2026-10-25; the captures and
mesh counts below describe that historical implementation.

The species keeps the hybrid foliage decision from section 9 and replaces the
old straight, intersecting scaffold. Its stem now uses the shared sculptural
fork's smooth path, root flare, stable transported frames and open junctions.
Two unequal primary branches reuse the trunk opening vertices instead of
placing a cylinder through the bark. Sixteen smaller seeded limbs establish an
asymmetric crown; their lengths and rise shrink toward the top. Fine hanging
twigs are distributed unevenly, and fourteen percent of eligible leaf sprays
are omitted to preserve visible wood and clear sky sectors.

The bark albedo was repainted at an intermediate scale. It keeps a clean white
paper base, fewer near-black lenticels, several thicker scars and restrained
gray paper curls. The image's opposite borders differ by less than the bounded
low-contrast seam threshold in fixture 1235. Minor limbs duplicate their
texture seam exactly. Seeded U/V offsets prevent a scar from appearing at the
same branch position throughout a tree. The joined primary mesh retains the
shared fork topology; the repeat sampler and quiet image borders prevent a
visible wrap in the inspected side and grazing views.

Only fine twig wood moves. Its vertices carry the same pivot and negative
response as the attached foliage spray. The twig and leaves therefore turn as
one branch-supported group while its root stays fixed. The stem, joined
primaries and other structural limbs use the ordinary rigid vertex layout.

Autumn Grove now shows five independent seeds (1437, 311, 4201, 827 and 2189)
at 6.6, 7.1, 7.6, 8.0 and 8.4 meters. The walker forests retain their stable
kind number and use the same generator, bark, leaf atlas, material colors and
impostor bake. Their current middle level deliberately shares the close mesh;
this task does not invent an unreviewed reduced silhouette.

The fixed species study covers daylight, profile, underside, warm late-day,
cool morning and moonlit conditions. The foliage remains non-emissive at night,
white bark stays legible in daylight without clipping, and the underside keeps
the joined scaffold visible. Remaining limits are card crossings at exact
grazing angles, repeatable painted spray details where several cards overlap,
and the missing dedicated middle-distance birch mesh.

For seed 1437 at the shared 5.5-meter reference size, the finished close model
contains 3,033 vertices and 3,942 triangles: 1,223/1,984 in rigid structural
wood, 1,350/1,728 in animated fine twigs, and 460/230 in 115 foliage cards.
The shared forest instances those five parts; it does not regenerate geometry
per tree. Fixture 1235 passed for four seeds from 4.8 to 8.6 meters, including
finite meshes, joined-junction normals, transported frames, buried roots,
duplicated fine-branch seams, bark-edge contrast, wind pivots and conservative
foliage bounds. Hardware checks `surfaceImages`, `leafWind` and
`impostorBake` passed. Example gates 136, 137, 138 and 143 passed, and two
fixed side captures were byte-identical. These checks establish compatibility
and repeatability; they do not claim a frame-time improvement.

## 11. Recovery control: original fine-edge texture and crown coverage

The 25 October review restores the original `birchLeaves.png` from `e86992db`
and uses its entire UV rectangle. Four or five unequal overlapping cards per
twig spray face the camera, with centers distributed along/across the same
supporting twig. Crown normals remain independent of the camera. The newer
scaffold, bark, twig pivots, species colors and five study seeds stay unchanged
so this foliage repair is a controlled comparison against the rejected result.

The first recovery restores much of the lost fullness and fine perimeter. It
still has distinct clumps, dark pockets and repetitive leaf marks. It is not an
approved species treatment and must not be rolled out to other trees. Larger
coverage and more cards also increase potential alpha overdraw; no performance
improvement is claimed. Shared textures, batches, wind anchors and existing
surface/shadow/atlas paths remain in use.

[Original, rejected and repaired frames](landscape-recovery-review.md) explain
the visual differences and link the private reference presentation. The new
fixture checks camera-facing modes, full-image UVs, alpha coverage, mips and
bounds; none of these assertions measures visual quality.
