# World biome, material & terrain system (design)

A spatial system that decides, at any world `(x, y)`, **what the ground is** —
water, sand/beach, mud, grass, rock, swamp, and greater biomes — and drives the
**terrain surface, water bodies and vegetation** from that one source of truth.
Top-down mobile is the primary target (the ground carries most of the visual, per
`docs/top-down-vegetation.md`), but it stays all-3D so third person is just a
freer camera, not a separate path.

## Core idea: one field, many consumers

Everything hangs off a **world field** evaluated the same way on CPU and GPU
(like `terrainHeightAt`/`perlin2`/`noise.wgsl` already are). At a point it yields:

- **elevation** — fbm noise; drives terrain height AND classification.
- **moisture** — a second, decorrelated fbm; wet↔dry axis.
- **slope** — magnitude of the elevation gradient (steepness).

A **classifier** turns `(elevation, moisture, slope, waterLevel)` into **material
weights** (water / sand / mud / grass / rock, room for snow/swamp) — soft,
smoothstep-banded so materials blend at their edges rather than hard-cut:

- `elevation < waterLevel` → **water** (lakes/sea are just the field below sea level).
- just above → **sand** (a beach band above the shoreline).
- steep slope or high elevation → **rock**.
- low + wet + flat → **mud / swamp**.
- otherwise → **grass**, its lushness scaled by moisture.

A coarser **biome** (a Whittaker-style bucket of moisture × elevation, later ×
temperature) selects the *palette and vegetation species* — meadow, swamp,
coastal, rocky highland, (desert/tundra later) — so "greater biomes with different
vegetation" is a lookup, not bespoke code.

**Consumers** all read the same field/classifier:
- **Terrain mesh**: height from elevation; per-fragment colour/texture from the
  material weights (splat) — the top-down base coverage.
- **Water**: a surface at `waterLevel` wherever the field is below it; the sand
  beach falls out of the classifier; gameplay marks water non-walkable.
- **Grass/vegetation**: per-blade biome/material → density, height, species,
  colour; suppressed on water/sand/rock; lush in grassland, reeds in swamp.
- **Gameplay**: walkability, spawn rules — same classifier, on the CPU.

## Procedural base + authored regions (hybrid)

Pure procedural gives an infinite, deterministic, zero-authoring world (ideal for
a top-down mobile game). But specific features want placement, so the field is a
**hybrid**:

- **Procedural base** — the noise field + classifier above.
- **Authored regions** — `BiomeRegion` ECS entities with a shape (circle
  centre+radius first; polygons later) that **override** the base inside them:
  force a biome, stamp a lake, raise a rocky hill, drop a beach. Regions are
  collected each frame into a small GPU storage buffer the classifier consults
  (contained/nearest region wins), and are equally queryable on the CPU for
  gameplay. This is the generalised form of #492's "biome regions via ECS".

Regions never hold a per-blade or per-texel entity — only region shapes + params,
exactly as #492 requires.

## Why this shape

- **One dual-ported field** avoids CPU/GPU divergence (the walker can't stand on
  water the shader drew) — the lesson `terrainHeightAt`/`noise.wgsl` already bake in.
- **Material *weights*, not a single id**, so terrain/grass blend at boundaries
  (no stair-stepped coastlines) and TAA/deferred normals stay stable.
- **Elevation drives both height and biome**, so coastlines, beaches and rocky
  peaks line up with the terrain shape for free.
- **Splat colours first, textures later** — a flat classified colour already
  reads correctly top-down (`docs/top-down-vegetation.md` §a); textures are a
  drop-in richness pass over the same weights.

## Phased implementation (QUEUE #531–#538)

1. **#532 — Shared biome field + classifier (dual CPU/GPU).** `lib/world_biome.rae`
   + `lib/world_biome.wgsl`: elevation/moisture/slope + material-weight classifier
   + biome bucket + a `BiomeConfig`. Deterministic, unit-tested. No rendering —
   the data model every other phase consumes.
2. **#533 — Terrain material splat.** Terrain fragment samples the classifier →
   blended material colour (grass/sand/mud/rock). Replaces the flat green ground;
   the grass gradient bottom tracks the LOCAL ground material.
3. **#534 — Water bodies.** A water surface at `waterLevel` where the field is
   below it; beach shoreline from the classifier; basic water material
   (colour/fresnel; transparency/reflection later); terrain basins; non-walkable.
4. **#535 — Grass biome integration.** Per-blade biome/material drives
   density/height/species/colour and suppresses grass off grassland; extends #492
   `GrassViewSettings` into per-biome vegetation params.
5. **#536 — Authored biome REGIONS (ECS).** `BiomeRegion` component + region GPU
   buffer + CPU query; hand-place lakes/beaches/rocky hills over the procedural
   base. (Closes #492's biome-region half.)
6. **#537 — Terrain textures & detail.** Tiled albedo+normal per material
   (triplanar/UV), detail normals, distance tiling break-up, optional POM — the
   richer top-down ground.
7. **#538 — Greater biomes & polish (later).** Temperature axis + more
   biomes/species/palettes; coastal foam, swamp fog, terrain deformation
   (footprints), region polygons.

## Open questions

- Elevation units: normalized field (−1..1) × amplitude, or meters directly?
  (Phase 1 picks one; height + waterLevel share it.)
- Deferred G-buffer normal encoding for a flat water surface + thin shorelines.
- Region buffer size / spatial accel if many regions (grid bucket later).
- Authoring: hand-placed regions vs a painted biome map import (tie-in with
  `.raescene`/#492 biome maps) — regions first, painted map later.
