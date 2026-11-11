# Open world in tiles (design)

How a walker world bigger than one mesh is drawn, simulated and filled: one tile grid for the
ground and for what stands on it, a terrain whose heights and ground kinds come from the app,
content streamed in tiles, and a host that keeps the world loaded round every player. The walker
examples (143, 136, 137) and a downstream game (game proto2) share it. This is the plan; the
tasks that build it are queued in game proto2 and named at the end.

## What exists today

- **The ground** (app3d/TerrainTiles): a gridSide x gridSide window of heightfield tiles, `cellSize`
  m each, round the camera's cell. A tile's mesh is rewritten in place when it wraps to a new cell,
  so crossing a cell rewrites one row or column. Heights come from WorldBiome
  (`worldTerrainHeight`); the terrain material (terrain_splat.wgsl) classifies the ground per pixel
  on the GPU from the same biome functions (elevation, moisture, slope, paths) and reads the frame's
  ground palette (app3d/GroundPalette) for the grass.
- **The far ground** (WalkerTerrain's far tiles: 400 m, a point every 10 m, 1 m under the near
  ground, with a hole where the near ground covers it).
- **The physics ground** (app3d/WalkerPhysics): one Box3D height field on a 1.5 m lattice, round
  one walker, made again when that walker has gone `followDistance` from its center.
- **Content streaming** (app3d/TileStreaming): square tiles, `tileSize` a side, loaded within
  `loadDistance` of ONE point and let go past `unloadDistance`, at most `slotCount`, one a frame,
  with TileLoaded / TileGone events the app answers. The GPU side (app3d/SlotMeshes: one quad mesh
  per paint, a range per slot), trees and props as a PropField group per slot (setPropGroup,
  clearPropGroup), far forms (app3d/FarTiles: boxes past the load distance).
- **The forest** (app3d/PropField: cells, levels of detail, impostors; FarSprites to the horizon).
- **SurfaceMap**: what the ground is made of over a level, from its triangles (a road over the
  terrain under it), for footsteps, dust and where grass and trees may grow.

What is missing for an app's own world: the terrain only knows WorldBiome; the ground kinds exist
only as GPU functions; the physics ground and the streamer each follow one point.

## (a) One grid: ground cells and content tiles

- The world is one grid on one origin. **Ground cells** are `cellSize` (18 m) with `segments` (24)
  quads a side: a point every 0.75 m. **Content tiles** are `cellsPerTile` x `cellsPerTile` ground
  cells (8: 144 m). A content tile's corners are ground-cell corners, so a tile's ground, props,
  colliders and roads all start and end at the same lines.
- Windows (prototype numbers; each app may set its own):

  | | ground cells round the camera | content load / unload | content slots | far |
  |---|---|---|---|---|
  | desktop | 17 x 17 (306 m) | 400 m / 480 m | 40 | far ground and far trees to 1.8 km |
  | phone | 13 x 13 (234 m) | 300 m / 360 m | 24 | far ground and far trees to 1 km |

  Slots: the unload circle holds about pi * 480^2 / 144^2 = 35 tile centers (desktop), pi *
  360^2 / 144^2 = 20 (phone), plus a margin.

## (b) The terrain source: the app's heights and ground kinds

Rae has no function values, so the terrain cannot call an app's height function. It asks with
data, as TileStreaming does:

- `updateTerrainTiles` no longer computes heights itself: it lists the cells whose tiles must be
  shaped this frame (**TerrainShapeWanted**: cellX, cellY), and the app answers each with a
  **TerrainPatch**: the cell's heights on its (segments + 1)^2 lattice, and the ground-kind weights
  on the same lattice (up to 8 kinds, 0..1 each, summing to 1), then `commitTerrainPatch`. A crossing
  asks for one row (17 cells, 17 x 625 points): cheap for any height function.
- **WorldBiome is one source**: a Rae system that answers the same events from
  `worldTerrainHeight` and its classifier. 143, 136 and 137 keep their look: the splat shader keeps
  its procedural path for a WorldBiome terrain (mode "biome") beside the new one (mode "kinds"); the
  examples move to "kinds" only when a picture shows no difference.
- **The ground kinds on the GPU**: a wrapping kind map round the camera, 512 x 512 texels of 0.75 m
  (384 m, more than the 306 m window), two RGBA8 layers (8 weights), 2 MB. A committed patch writes
  its cell's 25 x 25 texels; the shader samples it at the world position modulo the map, with linear
  filtering and a small noise warp of the sample point (about half a meter), so a boundary between
  kinds is soft and never a straight texel edge. No ground picture, no texels on screen.
- **The kinds' looks** are the app's table (up to 8): a color, roughness, and whether the kind is
  grass (painted with the painted grass's own field and the frame's ground palette, as the blades:
  the ground between the blades is their color at every distance and in every season), dry grass
  (the palette's straw) or bare (sand, gravel, rock). Snow (lib/snow.wgsl) and wetness lie on every
  kind as today.
- **The physics ground** is per content tile: a height field from the committed patches' heights
  on every second point (1.5 m), made when a content tile loads and removed when it goes; the
  heights are the drawn ones, so what you walk on is what you see. WalkerPhysics' one following
  field stays for apps without content tiles.

**Built (step 3, Rae 0.3.93):** app3d/TerrainPatch (the patch: heights with a one-point border so
normals agree across tiles, 8 kind weights a point, the kinds' looks, the kind map's texels),
app3d/TerrainTiles' app mode (createAppTerrainTiles; `wanted`, commitTerrainPatch,
uploadTerrainKinds, setTerrainKindsShader; a tile waiting for its patch is not drawn),
lib/terrain_kinds.wgsl (the "kinds" splat: the kind map in the terrain texture array's layers 0 and
1, a noise-warped sample for soft edges, bare / painted grass / dry grass, wetness, snow, the grass
lit by the up normal) and app3d/TerrainPatchBodies (a patch's physics height field). Case 1397;
example 145 with RAE_TILES_TERRAIN=1. One change from the plan: the walker examples keep their
WorldBiome path as it was (heights computed in TerrainTiles, the biome splat), not a WorldBiome
system answering the events, so their pictures stay identical; they move onto the events with
step 8, when their content does.

## (c) Content per tile

Each TileLoaded is answered by the app with what stands in the tile, from its own rules and seed:

- **trees, bushes, rocks**: one PropField group per slot (setPropGroup at load, clearPropGroup at
  unload), so the field's cells, levels of detail and impostors work as for a fixed forest, and
  the far trees (FarSprites) keep their own list for the whole world;
- **buildings and other meshes**: SlotMeshes ranges per paint, or instances of shared models;
- **colliders**: the tile's height field, rocks, trunks (a trunk pool near the walkers, as today),
  buildings, roads; all destroyed with the tile;
- **far forms**: FarTiles boxes for tall things past the load distance.

At most one content tile is built a frame (budget: 2 ms on the desktop); a tile's build may be
split over frames if it is bigger.

## (d) Several centers: the host keeps the world round every player

Every side derives the same tiles from the seed and the rules, so nothing about the ground is
sent. But in a session one side (the host) simulates what is shared: bots, free cars, thrown
bodies. They live near players, and the players can be far apart.

- **TileStreaming takes several centers**: a tile is wanted when it is within the load distance of
  any center and kept until it is past the unload distance of all; the nearest to any center
  loads first.
- **Two streamers on the host**: the drawing streamer round its own camera (meshes, props, far
  forms), and a **simulation streamer** round every player (heights and colliders only, a smaller
  radius: 150 m), so a bot or a car near a far player stands on real ground on the host. A tile in
  both shares its colliders. The other sides need only their drawing streamer: their own walker and
  the car they drive are simulated where they are drawn.
- A dynamic body whose tile is not loaded on the host sleeps (made kinematic where it is) until
  its tile comes back.

## (e) Roads as meshes

A road is a line (points, a width, shoulders), not a ground kind in texels:

- the carriageway and its paint as meshes along the line (sections every 2 m, level across),
  lifted a few centimeters over the ground, cut at content-tile edges into each tile's slot ranges;
- the ground under it graded: the app's heights already follow the road (the source answers the
  graded heights), and the shoulder's ground kind (gravel) comes from the source too;
- colliding (a static mesh per tile piece, so cars drive on the road, not on the ground under it);
- written into the SurfaceMap so trees and grass keep off and footsteps and tyres know it is road.

The rules (a line through points, its sections, the strips, the dashes) come from a road-building
module of a downstream game, made generic; the road network is the app's.

**Built (step 5):** app3d/RoadMesh: a road line through an app's points (open or closed; a
Catmull-Rom curve, then a point every `sectionLength` m), the ground asked as data
(roadGroundPoints: each point's middle and edges; roadSectionHeights: the highest plus the lift),
the carriageway and the paint (edge lines, a dashed center line; RoadStyle), its length and
tightest bend; case 1398. An app draws the meshes as static meshes and collides the carriageway
(VenueMesh addVenueMesh). Cutting a road at content-tile edges waits for step 6 (content in tiles).

## (f) Budgets and how a big world is checked

- Desktop (Mac): 60 fps; a frame with a stream (a ground row and a content tile) at most 2 ms over
  a quiet one. Phone (iPhone 15 Pro): 30 fps today, 60 the goal; the same 2 ms.
- Memory, fixed however big the world: ground tiles 289 x 625 vertices x 32 B = 5.8 MB (phone
  169 tiles, 3.4 MB); the kind map 2 MB; physics 40 fields of 97^2 heights, 1.5 MB; content by its
  slots.
- The check: a world 4 km a side, walked and driven across: memory flat after the first minute;
  the frame time of stream frames; two players 2 km apart in one session, each on loaded ground with
  its bots on the host.

## What is Rae's and what is the app's

| Rae (generic) | the app |
|---|---|
| TerrainTiles asking for patches (TerrainShapeWanted, TerrainPatch, commitTerrainPatch) | its height and ground-kind rules answering them |
| the WorldBiome source (the walker examples) | its own source (its level rules) |
| the kind map, the splat shader's "kinds" mode, the kinds' table type | its kinds' colors and which are grass |
| per-tile physics height fields | |
| TileStreaming with several centers; the simulation streamer | which tiles' content, from its seed |
| road meshes from a line, cut per tile, colliding, into the SurfaceMap | its road network |
| SlotMeshes, PropField groups, FarTiles, FarSprites (exist) | its buildings, props, far forms |

## The tasks (queued in game proto2)

1. this plan;
2. the walkers' terrain grass painted as the painted grass's own color (blades and ground one
   color), mode "biome";
3. the terrain source (patches, the kind map, the "kinds" mode, per-tile physics), WorldBiome as
   one source, the walker examples unchanged;
4. game proto2's ground on it (its look kept, checked side by side);
5. roads as meshes;
6. content in tiles, several centers, the simulation streamer;
7. a 4 km world;
8. the walker examples' content through the same tiles.
