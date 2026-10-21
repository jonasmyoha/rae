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
   of the count), but each patch is a flat disc CENTRED on the crown's shell,
   so a bigger one sticks further out: the outline grows at 30 m.
2. **Billboards are lit differently.** The picture's colour is baked with a
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
side; neighbouring levels **cross-fade** with a dither the temporal
antialiasing resolves, so nothing pops.

### 2.1 Levels that match by construction

* **The middle crown keeps the near crown's envelope.** Pull a bigger patch's
  centre inward by the extra radius (the patch's outer edge stays on the
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
otherwise; the band, centred on the cutoff, stays close to the scaled
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

### 2.3 Cross-fades instead of switches

* Each level has a fade band (about 10% of its distance, at least 4 m). In
  the band both levels are drawn; each instance gets a fade value (0..1) in
  its instance data, and the fragment discards against a per-pixel dither
  (blue noise, offset by a per-tree hash, so neighbours do not fade in step),
  the near level as `fade`, the far level as `1 - fade`. TAA turns the dither
  into a smooth blend; with no TAA, a fixed pattern still never pops.
* A little distance hysteresis is not needed: the fade is continuous.
* The same fade at the far end: past the impostors' distance, trees fade out
  into the far canopy (2.4) instead of vanishing.

### 2.4 Past 140 m: the far canopy

Per PropField cell (40 m), one **cluster impostor**: the cell's trees baked
together from a few directions at load, drawn as one quad per cell out to
1-2 km, fading in where the single-tree impostors fade out. With the existing
aerial perspective and fog, the forest reaches the horizon instead of ending.

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
