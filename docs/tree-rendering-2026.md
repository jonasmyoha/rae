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

The art direction exaggerates leaf size enough to read on screen. It keeps
open spaces between hanging sprays, a slightly crooked leader and warm ivory
bark. A uniformly filled sphere, cone or identical stack of canopy discs would
lose the species even with a good texture. The tree should look pleasant in
side light, against the sky and from underneath, not only in its thumbnail.

## 2. What current rendering approaches offer

| Approach | Close-up strengths | Costs and failure modes | Rae decision |
| --- | --- | --- | --- |
| Opaque shaped leaves | Real silhouette, folds, depth and stable shadows; no transparent corners | More vertices and tiny triangles; layered geometry still overdraws | Use for this LOD0 |
| Trimmed alpha-tested branch cards | Efficient authoring and fewer vertices; detailed leaf veins from an atlas | Cutout overdraw, flat views, edge aliasing, mip thinning, shadow-mask matching | Keep as a future measured alternative |
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

Our choice is a folded diamond with six outline vertices and a raised midrib.
It has an actual pointed silhouette and a small cup rather than a rectangular
leaf image. Reverse-wound triangles make the leaf visible from below with the
existing back-face-culling pipelines. This duplicates indices, not vertices.
There is no leaf opacity texture, discard or alpha blending in this variant.

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
its shadow vertex stage. Birch leaves use the twig attachment as their pivot;
wood stays rigid in this implementation. Reintroducing independent displacement
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
- Twenty-six irregularly phased limbs carry five hanging twigs each. Fourteen
  leaves per twig are divided across three colour meshes: 1,820 leaves total.
  Each seven-vertex leaf has six triangles per side and a fixed attachment.
- The shared forest registers one prototype. Grove placements vary yaw and
  scale. A future set of two or three seed variants can improve repetition
  without making every tree unique on the GPU.
- Five opaque parts separate pale bark, dark outer twigs and three leaf tones.
  Leaves use the shipped optional
  pivot stream; the wood uses the ordinary static vertex layout.

### Bark texture and materials

`lib/assets/trees/birchBark.png` is an original generated 512 x 512 bark albedo.
The image-generation tool produced the source; the shipped PNG is resized for
runtime use. It is warm ivory with broken horizontal charcoal marks. It has no
intentional directional lighting. Generation does not prove perfect edge
periodicity; the final wrapped trunk must be inspected for a visible seam.

The built-in image-generation prompt was: a flat orthographic, seamlessly
tileable silver-birch bark albedo, warm ivory covering most of the square,
irregular sparse horizontal charcoal-brown lenticels, occasional peeling scars,
subtle paper layers, neutral diffuse lighting, no trunk silhouette, text,
shadows, specular highlights or background. No external artwork was copied.

`SurfaceImages` owns a renderer-local opaque image array, view and repeating
trilinear sampler. It creates mips by averaging decoded linear RGB, then stores
sRGB texels for hardware decode. `rendererLoadSurfaceImage(renderer, path)`
returns its layer; `Material3d.colorTextureLayer = -1` keeps plain colour.
Images are loaded once at startup, cached by resolved path, and multiplied by
the material tint in the shared static surface shader. Forward and deferred
paths consume the same sample. Texture/view/sampler lifetimes use GpuResources.
A renderer without images binds a one-texel white fallback, not a full atlas.

This first API accepts at most 16 square 512-pixel opaque PNGs. The layer index belongs to
its renderer and is not a transferable global texture handle. Rebuilding the
array increments its generation and rebuilds consumer bind groups. Load failure
preserves the previous array and reports the path. Alpha-cutout foliage is not
silently added to this API: it would need matching shadow and mip semantics.

### Population and scope

- Autumn Grove has a small birch stand and a `birches` inspection view (key 9).
- `app3d/GroveTrees` adds the species to shared deterministic placement, so
  deferred walker 136, forward walker 137 and huge forest 143 receive it.
- Existing forest distance machinery remains active. The middle representation
  initially reuses the close-up birch shape; the existing automatic colour-only
  impostor bake supplies the distant silhouette. This is compatibility for the
  running forest, **not a finished birch LOD or bark-aware billboard project**.
- A dedicated reduced middle mesh, bark-aware impostor and aggregate canopy
  validation are deferred. Never render LOD0 for every tree to the horizon.
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

The implemented prototype has 17,875 vertices and 28,932 triangles across five
parts, including the reverse-wound leaf faces. The uploaded vertex/index data
is approximately 1.07 MiB (32-byte wood vertices, 48-byte leaf vertices and
32-bit indices), shared by its instances. This is a geometry budget, not a
measured GPU frame cost. The first grove capture exposed stretched bark marks
and overly pale outer twigs; the final version increases the bark's vertical UV
repeat and separates those twigs into a dark wood part.

Main risks: tiny-leaf aliasing, overly regular hanging sprays, a visible bark
wrap seam, bright bark clipping under exposure, flat backlighting, and expensive
shadow geometry in dense nearby stands. The initial solution deliberately has
no new transmission pass, branch simulation or custom distant representation.
Those should be separate measured changes with visual references.

## 5. Recorded validation

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
