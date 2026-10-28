# Walker in a huge forest

The deferred walker of example 136 on a huge generated terrain instead of an
island: 31,090 trees for the current fixed seeds, in stands over a 1.2 km
square, with lakes and meadows between the woods. Walk with WASD (Character
mode), jump on Space or F; the camera bar's buttons switch to the orbit and
free cameras.

The program is the walker app shell (lib/app3d/WalkerApp); `Main.rae` is
only its settings (`WalkerLand.forest`, the deferred renderer).

Run from the repository root:

```sh
compiler/bin/rae run examples/143_walker_forest/Main.rae
```

## What is different from 136

- **A stand of the hand-made reference tree.** 30 m ahead of where the
  walker starts, the forest leaves a patch 22 m wide to the tree being made
  by hand to match reference crop T02 (lib/treeGen,
  docs/reference-tree-plan.md): about one grid spot in three there gets one,
  each turned and sized 85-110%, so it can be seen in motion among the old
  trees (`referenceTree: true`, lib/app3d/ReferenceTree, app3d/WideForest).
  They have no physics bodies yet: the walker passes through their trunks.
- **No island.** The lib's biome field (lib/WorldBiome) fades the land into
  the sea at its island radius; this example gives it a radius far past
  anywhere the walker can go (`wideForestBiome`, lib/app3d/WideForest), on the
  CPU and in the terrain and grass shaders, so the noise terrain
  goes on with its hills, valleys and lakes.
- **A wider ground that wraps.** The ground is a 17 x 17 window of 18 m tiles
  (about 300 m) round the walker. The tile and small-prop slots wrap: when the
  walker crosses a cell border only the row or column that left the window
  moves to the new edge (about 1 ms), instead of every tile being reshaped.
  The rocks and bushes cast their shadows as instanced batches.
  Ground flowers use the same slots as the former orange flower mesh: each slot
  draws one shared seven-plant oxeye-daisy patch and one shared nine-plant
  harebell patch. The two species use five fixed instanced material batches in
  total, with no entity or draw per plant, petal or bell.
- **The forest is one PropField** (lib/app3d/WideForest,
  lib/app3d/PropField), as the downstream game's nature system does it: every
  tree is a placement, sorted into 40 m cells. Each frame only the cells and
  trees within the draw distance and in view are gathered, each at its level
  of detail. `lib/app3d/GroveTrees` uses full near meshes to 30 m, reduced
  middle meshes to 60 m, and baked impostor sprites to 140 m; the first two
  levels cast shadows. Birch currently reuses its near mesh for the middle
  level. The renderer submits one instanced batch per species, level and part.
  The field's `batchLimit` keeps each batch at 1,024 instances, so the instance
  memory does not grow with the forest.
- **Far trees to the horizon** (lib/app3d/FarSprites). Past 140 m, where
  the field's own billboards end, every tree still stands as a far billboard
  of the same picture, out to 1,800 m (the camera's far plane is 2,000 m):
  the trees in 64 m blocks, the nearest blocks first, at most 8,192 a frame,
  fewer kept past 300 m by the square of the distance and drawn bigger, so the
  whole forest shows from its edge for about 5,600 billboards (0.35 ms of
  gather on a desktop). They stand on a far ground: 5 x 5 tiles of 400 m, a
  vertex every 10 m, from the same height field, drawn 1 m under the near
  ground, which covers it round the walker.
- **The trees** stand on a jittered 3.5 m grid, thinned by a forest-density
  noise (woods and meadows a few hundred meters across) and kept off lakes,
  beaches and bare rock. Each 36 m block has one main species, so the forest
  grows in stands; three trees in ten are mixed in from any species. The
  kinds are those of Autumn Grove (138) without the giant camphor.
- **How big it is.** The terrain has no edge: its noise goes on in every
  direction (`wideForestWorldRadius` is 1,000 km). The planted forest is 1.2 x
  1.2 km round the origin; the near ground drawn round the walker about 300 m
  across, the far ground 2 x 2 km, the physics ground 240 m.
- **The crowd walks the whole land** (app3d/Wander): to spots anywhere within
  2.5 km of the origin (a circle 5 km across), a stop, another spot. Past the
  physics ground round the walker the terrain's own height holds them up
  (WalkerPhysics `holdMoversOnTerrain`), so they walk on far away.
- **Pooled tree physics** (lib/app3d/WalkerPhysics' trunk pool). A tree is not an
  entity. Physics has a fixed pool of 96 static trunk capsules; when the
  walker has moved 2 m, the trees within 16 m are looked up in the forest's
  cells and the nearest get a trunk each, the rest of the pool waits far
  below the ground. Physics costs the same however big the forest is.
- **The ground follows the walker.** The physics height field
  (240 m) is made again round the walker when it is 50 m from the field's
  center (`followGround`, about 4 ms; the lib's
  `removeVenueHeightField` frees the old one).
- **The lakes are the realistic sea.** `lib/water/IslandSea` (the same as
  136's): the FFT ocean on a grid that follows the camera, its waves fading
  in shallow water. It is given the forest world's biome, so the shore damping
  knows the forest world's coast, on the CPU and in the surface shader.

## Checks

`RAE_WALKER_PHYSICS_TEST=1` runs the physics check and quits: the cube stacks
settle, the hero walks into one and jumps, a pooled trunk blocks a ray and
stops the hero at its bark, the pool moves onto the trees round a spot 60 m
away, and the ground made round a spot 150 m away is at the height the
renderer draws. The example gate (`compiler/tools/run_examples.sh`) runs it
and a screenshot.

`RAE_FRAME_TIMING=1 RAE_GPU_PASS_TIMING=1 RAE_DRS=off` print the frame and
GPU pass times; `RAE_CAMERA_AT` (or the older `RAE_WALKER_CAMERA`) and `RAE_WALKER_HOUR` set the start
camera and hour, as in 136.

The fixed landscape cameras, current counts and capture command are recorded in
[`docs/landscape-comparison-views.md`](../../docs/landscape-comparison-views.md).
Run `tools/capture-landscape-comparisons.sh captures` from the repository root
to write full-size temporary evidence under `/tmp`.
