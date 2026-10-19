# Walker in a huge forest

The deferred walker of example 136 on a huge generated terrain instead of an
island: about 32,000 trees in stands over a 1.2 km square, with lakes and
meadows between the woods. Walk with WASD (Character mode), jump on Space or
F; the camera bar's buttons switch to the orbit and free cameras.

Run from the repository root:

```sh
compiler/bin/rae run examples/143_walker_forest/Main.rae
```

## What is different from 136

- **No island.** The lib's biome field (lib/WorldBiome) fades the land into
  the sea at its island radius; this example gives it a radius far past
  anywhere the walker can go (`forestBiomeConfig`), on the CPU and in the
  terrain and grass shaders (`forestBiomeConstants`), so the noise terrain
  goes on with its hills, valleys and lakes.
- **A wider ground that wraps.** The ground is a 17 x 17 window of 18 m tiles
  (about 300 m) round the walker. The tile and small-prop slots wrap: when the
  walker crosses a cell border only the row or column that left the window
  moves to the new edge (about 1 ms), instead of every tile being reshaped.
  The rocks and bushes cast their shadows as instanced batches.
- **The forest is one PropField** (`terrainSystem/Forest.rae`,
  lib/app3d/PropField), as the downstream game's nature system does it: every
  tree is a placement, sorted into 40 m cells. Each frame only the cells and
  trees within the draw distance and in view are gathered, each at its level
  of detail (`terrainSystem/WalkerTrees.rae`: full crowns to 30 m, sparser
  crowns of bigger leaf patches to 60 m, a few big patches to 140 m; the
  first two cast shadows), into one instanced draw per species, level and
  part. The field's `batchLimit` keeps each batch at 1,024 instances, so the
  instance memory does not grow with the forest.
- **The trees** stand on a jittered 3.5 m grid, thinned by a forest-density
  noise (woods and meadows a few hundred metres across) and kept off lakes,
  beaches and bare rock. Each 36 m block has one main species, so the forest
  grows in stands; three trees in ten are mixed in from any species. The
  kinds are those of Autumn Grove (138) without the giant camphor.
- **Pooled tree physics** (`physicsSystem/TrunkPool.rae`). A tree is not an
  entity. Physics has a fixed pool of 96 static trunk capsules; when the
  walker has moved 2 m, the trees within 16 m are looked up in the forest's
  cells and the nearest get a trunk each, the rest of the pool waits far
  below the ground. Physics costs the same however big the forest is.
- **The ground follows the walker.** The physics height field
  (240 m) is made again round the walker when it is 50 m from the field's
  centre (`followGround`, about 4 ms; the lib's
  `removeVenueHeightField` frees the old one).
- **The lakes are the realistic sea.** `lib/water/IslandSea` (the same as
  136's): the FFT ocean on a grid that follows the camera, its waves fading
  in shallow water. It is given `forestBiomeConfig()`, so the shore damping
  knows the forest world's coast, on the CPU and in the surface shader.

## Checks

`RAE_114_PHYSICS_TEST=1` runs the physics check and quits: the cube stacks
settle, the hero walks into one and jumps, a pooled trunk blocks a ray and
stops the hero at its bark, the pool moves onto the trees round a spot 60 m
away, and the ground made round a spot 150 m away is at the height the
renderer draws. The example gate (`compiler/tools/run_examples.sh`) runs it
and a screenshot.

`RAE_FRAME_TIMING=1 RAE_GPU_PASS_TIMING=1 RAE_DRS=off` print the frame and
GPU pass times; `RAE_WALKER_CAMERA` and `RAE_WALKER_HOUR` set the start
camera and hour, as in 136.
