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
Autumn colours should move from soft green through lime to warm gold.
These identifiers follow [Kew's species account](https://powo.science.kew.org/taxon/urn%3Alsid%3Aipni.org%3Anames%3A295174-1/general-information)
and the [Woodland Trust's illustrated silver-birch guide](https://www.woodlandtrust.org.uk/trees-woods-and-wildlife/british-trees/a-z-of-british-trees/silver-birch/).

The revised art direction uses rounded leafy groups with smooth lighting,
a slightly crooked leader and bright white bark with sparse, thick black scars.
The original individual-leaf geometry was rejected after review.
The three supplied screenshots favour broad foliage masses and leafy outlines.
They are visual references; still images do not identify an engine or wind algorithm. A uniformly filled sphere, cone or identical stack of canopy discs would
lose the species even with a good texture. The tree should look pleasant in
side light, against the sky and from underneath, not only in its thumbnail.

## 2. What current rendering approaches offer

| Approach | Close-up strengths | Costs and failure modes | Rae decision |
| --- | --- | --- | --- |
| Opaque shaped leaves | Real silhouette, folds, depth and stable shadows; no transparent corners | More vertices and tiny triangles; layered geometry still overdraws | Rejected for this birch after visual review |
| Trimmed alpha-tested branch cards | Efficient authoring and fewer vertices; detailed leaf veins from an atlas | Cutout overdraw, flat views, edge aliasing, mip thinning, shadow-mask matching | Use curved patches with clump normals for this LOD0 |
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

The revised choice is a curved 3 x 3 vertex patch carrying a leaf-cluster
colour/alpha texture. Twelve patches surround each branch clump. Normals are
computed from the clump envelope with an upward bias; they are not the flat
normals of the cards. Reverse-wound indices make both sides visible without
flipping the lighting normal. Alpha below 0.5 is discarded in colour, shadow
and impostor-bake passes. There is no sorted transparent blending.

The image uses simple overlapping leaf shapes rather than photographic veins
or baked shadows. The initial textured render was too busy, so it was replaced
with a simpler generated texture before validation. The clump lighting follows
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
its shadow vertex stage. Birch clumps share a branch attachment as their pivot; all patches in a clump
rotate together, with their normals. Their signed flexibility selects a slow,
small branch response: fixed per-tree phase, fixed per-branch phase, and no
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
  has one rounded leaf clump, split across three colour meshes. Each clump has
  twelve curved patches: 312 patches, 2,808 leaf vertices and 4,992 leaf
  triangles including both sides. Each clump shares one fixed branch pivot.
- The shared forest registers one prototype. Grove placements vary yaw and
  scale. A future set of two or three seed variants can improve repetition
  without making every tree unique on the GPU.
- Five parts separate pale bark, dark outer twigs and three leaf tones.
  Leaves use the shipped optional
  pivot stream; the wood uses the ordinary static vertex layout.

### Bark texture and materials

`lib/assets/trees/birchBark.png` and `birchLeaves.png` are original generated
512 x 512 PNGs. The bark revision has sparse near-black horizontal scars on
white, with much less fine noise. Bark V repeats 0.34 times per metre rather
than 1.4, making the markings about four times longer vertically. The leaf
texture has broad flat green shapes, transparent margins and small holes.

Generation briefs (built-in image generator; originals retained outside the
repo, resized for runtime):

- Bark: seamless silver-birch albedo; white covering about 90%; about eight
  large sparse near-black horizontal scars, 15–28% width and 2–5% height;
  no hairlines, speckle, lighting, shadows or text.
- Final leaves: dense rounded cluster of about 45 overlapping large simple
  oval pointed leaves; similar medium green shades; no veins, realistic
  surface shading or strong shadows; filled centre, transparent background
  and a few small gaps; margins on every side.

`SurfaceImages` owns a renderer-local RGBA image array, view and repeating
trilinear sampler. RGB is decoded to linear light before mip generation.
Mip colour is weighted by alpha to avoid dark borders from clear texels;
alpha is rescaled to keep the covered fraction at cutoff 0.5 as closely as
the lower resolution permits. It cannot preserve an arbitrary fraction in
a one-pixel mip. RGB is stored as sRGB for hardware decode; alpha stays linear.

`rendererLoadSurfaceImage(renderer, path)` returns the layer;
`Material3d.colorTextureLayer = -1` keeps plain colour. Forward and deferred
colour, foliage shadows, and the existing impostor bake use the same alpha
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

The revised prototype has 7,943 vertices and 12,084 triangles across five
parts, including reverse-wound patch faces. That is fewer vertices than the
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

## 6. Patch revision validation

The updated geometry fixture checks seeded stems, valid indices and unit
normals, UV seams, 312 patches, shared clump pivots and alpha-weighted mip
colour/coverage. The wind hardware check exercises both patch responses.
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
