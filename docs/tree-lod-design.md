# Tree levels of detail: matching volumes and a smooth transition

Status: design, 2026-10-21. Queued as the "tree LOD n/6" tasks in QUEUE.md.
Target: the walker examples, above all 143 (31k trees in a PropField, mostly
billboards). Quality first on PC; the same pipeline must run on an
iPhone 15 Pro class phone with a lighter setting.

## 1. What happens today

`lib/app3d/GroveTrees.rae` gives each species three levels in a PropField
(`lib/app3d/PropField.rae`):

| level | what | drawn to (143) |
|---|---|---|
| near | full crown: 48 leaf patches per cluster (500 leaves for the autumn trees and the palm) | 30 m |
| middle | sparser crown: 14 bigger patches (110 leaves) | 60 m |
| far | billboard: one picture of the near crown baked from one side on the CPU (`app3d/SpriteBake`, 192x128, alpha cut at 0.5, box-filtered mips), a cylindrical quad (`lib/sprite_billboard.wgsl`) | 140 m |

Each level ends at a hard distance, with nothing drawn past 140 m.

### Measured (side view, share of the near crown's covered area)

A bake of every species at 768 px for the near crown and the middle crown,
and at 128 px for the billboard picture through its mips, after the 0.5 cut:

| species | middle | billboard mip 0 / 1 / 2 / 3 / 4 |
|---|---|---|
| broadleaf | **113%** | 99 / 101 / 101 / 99 / 93% |
| umbrella | **114%** | 100 / 102 / 102 / 102 / 100% |
| cypress | **118%** | 99 / 102 / 100 / 97 / **112%** |
| spruce | 91% | 100 / 105 / 103 / 94 / 87% |
| autumn trees, palm | 101-106% | 99-104% to mip 3, 84-92% at mip 4 |

So the silhouette is not the whole story, but it is part of it.

**Step 1 done (2026-10-21).** Measured fairly (areas in the tree's own
units, so a taller level is not hidden by the bake's framing) and from 8
sides with `app3d/TreeCoverage`, the middle crowns were further off than the
side view above showed: broadleaf 122%, umbrella 128%, cypress 118%, spruce
94%, the autumn trees 105-106%, the palm 103%. Now every species' middle
level covers 97-103% from every side (case 1228 holds it):

* leaf-patch crowns (`procgen/FluffyFoliage fluffyCrown`): a bigger patch is
  pulled inward by its extra half size, so it reaches no further than the
  near crown's, and keeps 90% of its extra size (62% for the flat umbrella
  crown, whose rim patches overhang above and below its thin layer);
* the spruce's skirts (`procgen/Spruce`): a polygon of fewer sides is widened
  to its circle's area;
* the autumn trees' and palm's leaves (`procgen/Foliage leafCrown`): the
  same pull, 65% of the extra half size.

The near level is the outline every coarser level is built to: GroveTrees'
`nearPatches` and `nearLeaves` are `fluffyOutlinePatches` and
`outlineLeafCount`.

### Why far trees look bigger than near ones

1. **The middle crown grows 13-18%** (the leaf-patch species). Fewer patches
   are made bigger to keep the leaf area (`fluffyCrown`: patch size ~ 1/sqrt
   of the count), but each patch is a flat disc CENTERED on the crown's shell,
   so a bigger one sticks further out: the outline grows at 30 m.
2. **Billboards are lit differently.** The picture's color is baked with a
   fixed light, and the G-buffer then lights it again with its normal facing
   straight up (`gbufferSurface`). A far tree is brighter and flat, with no
   dark interior, no self-shadow and no ambient occlusion, while a near crown
   is dark inside. A bright, flat shape reads as bigger and nearer; in a
   frame of 143 the far trees are a saturated wall.
3. **Billboards are solid.** The picture's alpha is 0 or 255 and its mips are
   averaged then cut at 0.5: the sky gaps a near crown shows close up and
   vanish. At mip 4 (the farthest trees) the coverage drifts 84-112%.
4. **One view.** Every billboard of a species shows the same side, whatever
   the tree's yaw, and the same view from above or below.
5. **Hard switches**, at 30, 60 and 140 m, and the forest ends at 140 m.

## 2. The technique: octahedral impostors with dithered cross-fades

What current engines use for dense forests (Fortnite's and Unreal's
impostors, Unity's AmplifyImpostors, SpeedTree's billboards): a tree's far
level is a quad showing the tree **baked from many directions**, with
**normals and depth**, so it is lit like the mesh and looks right from every
side; neighboring levels **cross-fade** with a dither the temporal
antialiasing resolves, so nothing pops.

### 2.1 Levels that match by construction

* **The middle crown keeps the near crown's envelope.** Pull a bigger patch's
  center inward by the extra radius (the patch's outer edge stays on the
  shell), and pick its size so its covered area from 8 directions matches the
  near crown within 3%. A Rae test computes the coverage (the bake used for
  the table above) and fails when a level drifts.
* **The impostor is baked from the near crown**, so its silhouette is the
  near crown's from every baked direction.
* **Coverage-preserving mips** (Castano, "Computing alpha mipmaps", 2010):
  per mip level, scale alpha so that the share of pixels over the cutoff is
  the same as at mip 0. The farthest trees keep the near crown's coverage.
* **Hashed alpha test** (Wyman and McGuire, 2017) instead of the fixed 0.5:
  sub-pixel leaves stay soft and stable at distance, and TAA resolves them.
  On the forward MSAA path, alpha to coverage does the same.

**Step 2 done (2026-10-21).** The billboards' mips keep their coverage:
`app3d/SpriteBake spriteMipChain` scales each level's alpha (bisection, with
a fixed sub-step jitter so the few alpha values a halved 0/255 picture has
do not cross the cut all at once) so a cut at 0.5 keeps the full picture's
share; every species stays at 99-101% over its 4 levels (128 rows down to
16; a smaller level cannot hold a crown, a pixel being several percent of
it). The hashed test and Castano's scaling pull against each other (a hash
over the whole 0..1 range keeps the AVERAGE alpha, which the scaling
changes), and TAA is off by default (FXAA) and absent in the forward pass,
where a hash is left as grain. So the threshold is hashed in a band round
the cutoff (0.5 wide) only in a deferred frame with TAA on, and a plain cut
otherwise; the band, centered on the cutoff, stays close to the scaled
coverage. The shadow pass samples mip 0, whose alpha is 0 or 255, so its
cut was left as it is. Alpha to coverage on the forward path is not done.

### 2.2 The impostor

* **Bake on the GPU**, once per species at startup: a hemi-octahedral grid
  of views (8 x 8 on PC, 4 x 4 on the phone; trees are seen from the side
  and above, never from below the ground), each view an orthographic picture
  into an atlas with **albedo + alpha**, **normal** and **depth**. A WebGPU
  render pass over the near meshes (the meshes and materials the near level
  already draws), not the CPU rasteriser, so it costs milliseconds.
* **Draw:** one quad per tree, facing the camera. The vertex shader turns the
  view direction (with the tree's yaw folded in, so trees keep their turn)
  into the octahedral grid and picks the three nearest views and their
  weights; the fragment shader samples all three, offset by their depth
  (parallax) so they line up, and blends. On the phone, one view with no
  blend.
* **Lit like the mesh:** the impostor writes its baked normal and the depth
  it moved into the G-buffer, so the sun, the moon and the day cycle, the
  shadows and SSAO treat it as the mesh. The albedo is baked unlit.
* **Shadows:** the shadow pass draws the impostor from the sun's direction
  (the view nearest the sun), alpha-tested with the same coverage.

**Step 4 done (2026-10-21).** `ImpostorBake bakeImpostorAtlas` bakes every
species at startup in one GPU submission (91 ms for the 9 species on an
M-series Mac, 43 ms with the phone's views): one render pass per species
draws the near level's meshes (the ones the renderer already holds) into
every view cell at once, an instance per view, into two RGBA8 texture
arrays, unlit albedo + alpha and normal + depth (`ImpostorAtlas`). PC is
8 x 8 views of 128 pixels (1024 x 1024 a species), the phone 4 x 4
(`defaultImpostorSettings`, by `Target.isMobileOs`). The grid is
hemi-octahedral with the horizon on its border (`impostor_octahedral.wgsl`).
The mips are coverage-preserving on the GPU (`impostor_mips.wgsl`): per view
cell a histogram of the averaged alpha, the scale that keeps level 0's share,
then the write; every species keeps 100% through its 4 levels. Each level is
made straight from level 0, and a fixed per-texel tie-break orders equal
averages. `checks/impostorBake` checks this and writes the atlas to PNGs.
The far level draws from the atlas instead of the CPU picture. The CPU bake
(`app3d/SpriteBake`) stays for other sprites and the coverage test.

**Step 5 done (2026-10-21).** The sprite pass's impostor mode
(`sprite_billboard.wgsl`) draws each tree as the sphere's square facing the
eye. The direction to the eye, turned into the tree's frame by its yaw
(sprite instances now carry it in their model matrix,
`addSpriteInstance(yaw:)`), picks the three nearest views and their
barycentric weights (`impostorThreeViews`). The fragment reads each view
where the eye's ray meets the tree (one parallax step on the baked depth)
and blends them by weight and alpha. It writes the baked albedo, the baked
normal turned to the world and the depth of the point it shows into the
G-buffer, so the sun, the moon, shadows and SSAO light it like the mesh. The
old double lighting (baked color, lit again with an up normal) is gone. The
phone setting (`blendViews: false`) reads the nearest view only. The sprite
shadow pass draws the view nearest the sun, in that view's plane.

Writing the depth turns off the early depth test, which made every hidden
far tree run the full shader (+2 ms). Two things bring that down: a fragment
where all three views are empty at their plane leaves after three reads (no
parallax can move it onto the tree), and the far billboards past the field
(`app3d/FarSprites`, to 1,800 m) use a second pipeline that keeps the quad's
depth (`drawSpriteInstances(movedDepth: false)`). Frame time on an M-series
Mac, 2304 x 1440: the walking view 9.3 ms -> 10.0 ms, a view from 28 m up
over the forest 9.7 ms -> 10.9 ms (run-to-run noise about 0.4 ms).

### 2.3 Cross-fades instead of switches

* Each level has a fade band (about 10% of its distance, at least 4 m). In
  the band both levels are drawn; each instance gets a fade value (0..1) in
  its instance data, and the fragment discards against a per-pixel dither
  (blue noise, offset by a per-tree hash, so neighbors do not fade in step),
  the near level as `fade`, the far level as `1 - fade`. TAA turns the dither
  into a smooth blend; with no TAA, a fixed pattern still never pops.
* A little distance hysteresis is not needed: the fade is continuous.
  (Superseded: fades are timed now, with a quarter-band hysteresis; see
  the timed fades below.)
* The same fade at the far end: past the impostors' distance, trees fade out
  into the far canopy (2.4) instead of vanishing.

**Step 3 done (2026-10-21).** `app3d/PropFade propLodPickFrom` gives each
level a band of max(10%, 4 m) centered on the distance it ends at; inside it
`app3d/PropFieldFrame` draws the prop at both levels with the same threshold
and a seed hashed from its position (the last level fades out alone, so the
forest's edge is a fade, not a line). The instance record grew to 44 floats
(`GbufferRecord` [40..43]: threshold, fading in, seed) and
`lib/fade_dither.wgsl` holds the dither: interleaved gradient noise (cheap,
spread like blue noise), moved by the TAA jitter when TAA is on. The
G-buffer and forward mesh passes, the billboards and both shadow passes
discard with it; the mesh shadow pass draws the fading casters with a second
pipeline (`ShadowMapsRecords`), so the solid ones keep the depth-only path.
A scripted 8 m camera move through 143 shows trees changing gradually, with
no pop; the frame time did not grow (19.3 ms before, 18.5 ms after, within
the run-to-run noise).

**Timed fades (2026-10-30).** A fade that followed the distance stopped
wherever the eye stopped: a tree inside a band stayed half one level and
half the next, dithered, for as long as the eye stood there. Now a switch is
a short animation that always finishes (`app3d/PropFade`): each placement
keeps the level it shows, the level it fades from, the fade's progress and
the gather frame it was last drawn in (`PropLodStates`, on the field and on
each group). When its distance passes the end of the level it shows by a
quarter of that level's fade band (the hysteresis, so a tree at a boundary
does not flip), it fades to the new level over `propFadeSeconds` (0.6 s),
whatever the eye does after, with the same threshold and seed on both
levels as before. Past the last level it fades out alone, and fades back
in the same way. A tree the gather did not draw in the frame before (out of
view, out of reach, the first frame) takes its level at once.
`gatherPropField` and `gatherTreeForest` take the frame's `deltaTime`.
Case 1136 checks the hold, the start, the halfway point with the eye
stopped, the finish, the fade out and the jump after a frame unseen. The
far canopy's hand-over (2.4) still follows the distance (queued).

### 2.4 Past 140 m: the far canopy

Per PropField cell (40 m), one **cluster impostor**: the cell's trees baked
together from a few directions at load, drawn as one quad per cell out to
1-2 km, fading in where the single-tree impostors fade out. With the existing
aerial perspective and fog, the forest reaches the horizon instead of ending.

**Step 6 done (2026-10-21).** By the time this step ran, 143's far trees
already reached 1,800 m as single-tree billboards (`app3d/FarSprites`), but
thinned with distance and drawn up to twice their size to hide the gaps.
`app3d/FarCanopy` replaces them past 300 m: every non-empty 40 m cell of the
forest's PropField is one cluster (829 clusters of all 31,090 trees in 143).
`CanopyBake` bakes them at load in one GPU submission (66 ms) from the
trees' own impostors, not their meshes: each tree is one quad of its atlas
view nearest the cluster view (`canopy_bake.wgsl`), with its baked depth, so
the trees of a cluster hide each other. The clusters are pages of the same
kind of atlas: many pages share a layer (`subjectsPerSide`, past the 256
layer limit), and each view is framed to the cluster's box, not its sphere
(`impostorExtents`), in a wide cell (128 x 32 pixels), so a low, wide
cluster fills its side views. The sprite pass draws a cluster as it draws a
tree (three views blended, lit through the G-buffer), with the quad's
depth. Between 270 and 330 m a cell's trees fade out as its cluster fades
in, both measured from the cluster's center with its seed, so the dither
shows each pixel from one of them; the far trees now stop at 370 m and are
no longer thinned. PC: 3 x 3 views (8 sides and the top), 13 layers of
3072 x 768, about 330 MB; the phone setting (`RAE_SIMULATE_MOBILE=1` on a
desktop): 2 x 2 views of 64 x 16, the nearest one drawn, about 36 MB, baked
in 27 ms. Frame time on an M-series Mac, 2304 x 1440: the walking view
unchanged (10.0 ms), a view from 28 m up over the forest 10.4 -> 10.9 ms.
`checks/impostorBake` also bakes a canopy of four cells and checks its mips
keep their coverage; the 143 gate checks every planted tree is in a cluster.

**Follow-up: clusters that keep their trees (2026-10-22).** A downstream
game's sparse hill forest showed far clusters with far fewer trees than the
trees themselves, flat umbrella pines and palms nearly gone.
`checks/canopyCoverage` now measures it: one 40 m cell of each species,
drawn as a cluster and as its trees with the sprite pass's own shader, at
150, 700 and 1,100 m from four directions between views. The clusters kept
0-77% of their trees. Five causes, fixed in order:

* The canopy bake read the trees' frames two vec4 apart after they became
  three (the box framing): most species baked with another's sphere and
  page, or not at all (0%).
* A filtered read at a prop's edge mixed in the empty texels' depth (0, the
  far back), so edges looked far behind the prop. Depth and the new
  thickness are now stored times coverage and divided by the filtered alpha
  (`impostorDepthOf`).
* A cluster is 40 m deep and its views were 45 degrees apart: seen between
  two views, a tree lands up to 13 m from where a view drew it, and the one
  parallax step only moves a pixel that already hits. Box framed atlases now
  march each view along the fragment's own ray through the box
  (`impostorMarch`): the first point within the depth baked behind the
  surface there (the normal is stored octahedral to make room for that
  thickness; a tree's is its front mirrored about its center, about a crown,
  nothing at a trunk). The PC canopy has 4 x 4 views of 96 x 24 (same
  memory, twelve sides about 30 degrees apart), the phone 3 x 3 of 48 x 12.
* Averaging three views' alpha lost what the views disagree on; a fragment
  now marches one view, picked by its weight with a dither.
* A cluster's quad stood at its center, so on a slope the ground in front of
  the center hid its front trees: the quad now stands at the box's front.

Clusters now keep 88-104% of their trees' pixels on PC (the phone 91-120%).
The march costs about 1.5 ms in views across the forest (10.3 -> 11.6 ms
from 28 m up, 9.3 -> 11.0 ms from 1.1 km away; the walking view unchanged),
after drawing the clusters front to back, sizing the march to the texels
crossed (at most 12 steps) and skipping rays a coarse level shows empty.

**Follow-up: the march's cost (2026-10-22).** `RAE_CANOPY_BENCH=1
checks/canopyCoverage` times a 1.2 km forest of 900 clusters on the GPU,
marched and unmarched, steady where the app's frame time swings with the
machine. It showed the five halvings after each candidate step were half
the march. Now a step already within the prop is a hit without halving;
otherwise three halvings find the entry, and the test at the point found
allows for the depth the remaining interval spans (fewer halvings without
that allowance lost a tenth of the trees). The ray's atlas point and depth
are linear along it, worked out once. Bench, from 500 m outside the forest
and from 28 m up in it: 3.1 -> 2.0 ms and 1.9 -> 1.3 ms (unmarched 0.4 and
0.3 ms); coverage unchanged (89-103%, the phone's cypress better). In the
app the two views across the forest are back at their unmarched frame
times (9.2 and 10.3 ms). Tried and dropped: a coarser level for far
clusters (cost lay in the near ones, where it blurs), narrowing the march
to where a coarse level shows something (a forest cluster is rarely empty
at that level), 10 steps (7%, less coverage).

### 2.5 Performance

* PC: near meshes to 30 m, middle to ~70 m, impostors to ~250 m, canopy past
  that. The cost moves from triangles to one quad per tree; three atlas
  samples per pixel.
* Phone: 4 x 4 views, one sample, a smaller atlas (and fewer near trees).
* When the PropField's per-frame gather becomes the cost (well past 31k
  trees), move culling, level choice and fade to a compute pass writing
  indirect draws (WebGPU `drawIndirect`). Measure before doing it.

## 3. Steps (the queued tasks)

1. A coverage test for every level of every species (8 directions, the
   impostor at each mip); the middle crown made to match the near one.
2. Coverage-preserving mips and a hashed alpha test for the current
   billboards.
3. Dithered cross-fades between levels, in the G-buffer and the shadow pass.
4. The GPU octahedral impostor bake: albedo, normal, depth atlas per species.
5. The impostor draw: three-view blend with parallax on PC, one view on the
   phone, lit through the G-buffer, shadows from the sun's view.
6. The far canopy: per-cell cluster impostors past 140 m, faded in.

Each step is checked against before and after screenshots of 143 from a
fixed camera (`RAE_CAMERA_AT`), and steps 3-6 by the walker gates' frame
time on PC.
