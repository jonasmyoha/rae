// World biome & material field for Rae WebGPU shaders (#532).
// MUST stay aligned with lib/world_biome.rae (same noise, same band edges, same
// classification) so the terrain/grass the GPU draws matches the CPU (gameplay,
// walkability). Depends on lib/noise.wgsl (raeNoiseFbm2). Materials:
//   0 water, 1 sand, 2 mud, 3 grass, 4 rock.

// Defaults mirror world_biome.rae defaultBiomeConfig().
const RAE_BIOME_SEED: u32 = 1337u;
const RAE_BIOME_ELEV_SCALE: f32 = 90.0;
const RAE_BIOME_MOIST_SCALE: f32 = 140.0;
const RAE_BIOME_WATER_LEVEL: f32 = -0.28;
// Widened: the reference's beach is a broad band, and the old 0.15 put sand in a
// strip only a few units across.
const RAE_BIOME_BEACH_BAND: f32 = 0.24;
// How far above the sand band the slope-driven rock term stays suppressed.
const RAE_BIOME_SHORE_ROCK_FADE: f32 = 0.16;
const RAE_BIOME_ROCK_SLOPE: f32 = 0.30;
const RAE_BIOME_SWAMP_MOIST: f32 = 0.6;
const RAE_BIOME_ISLAND_RADIUS: f32 = 60.0;
const RAE_BIOME_ISLAND_FALLOFF: f32 = 22.0;

struct RaeBiome {
  elevation: f32,
  moisture: f32,
  slope: f32,
  material: i32,
  biome: i32,
  wWater: f32,
  wSand: f32,
  wMud: f32,
  wGrass: f32,
  wRock: f32,
};

// 1 well inside the island, 0 out at sea. Mirrors world_biome.rae
// worldIslandMask — an RTS level needs a readable edge, and open water is the
// most legible one.
fn raeBiomeIslandMask(p: vec2<f32>) -> f32 {
  let d = length(p);
  return 1.0 - smoothstep(RAE_BIOME_ISLAND_RADIUS - RAE_BIOME_ISLAND_FALLOFF,
                          RAE_BIOME_ISLAND_RADIUS, d);
}

// Shaped by SUBTRACTING at the rim, not by fading the noise toward ocean.
// Scaling by the mask eats the island — near the rim only an unusually high
// noise value stays above water, so the coastline lands far inside the nominal
// radius. Subtracting leaves the interior untouched and carves only the edge.
// Mirrors world_biome.rae worldElevation.
const RAE_BIOME_ISLAND_SINK: f32 = 1.6;

fn raeBiomeElevation(p: vec2<f32>) -> f32 {
  let e = raeNoiseFbm2(p / RAE_BIOME_ELEV_SCALE, 4u, 2.0, 0.5, RAE_BIOME_SEED);
  let mask = raeBiomeIslandMask(p);
  return e - (1.0 - mask) * RAE_BIOME_ISLAND_SINK;
}

// Radial dirt roads meander away from the island centre. A perpendicular
// distance to each ray avoids atan2 and guarantees every road connects inward.
// MUST match worldPath in world_biome.rae.
const RAE_BIOME_PATH_SPOKES: i32 = 3;
const RAE_BIOME_PATH_HALF: f32 = 3.4;
const RAE_BIOME_PATH_FADE: f32 = 2.6;
const RAE_BIOME_PATH_MEANDER: f32 = 11.0;
const RAE_BIOME_PATH_WAVE: f32 = 46.0;

fn raeBiomePath(p: vec2<f32>) -> f32 {
  let radius = length(p);
  if (radius < 6.0) { return 0.0; }
  let wander = sin(radius / RAE_BIOME_PATH_WAVE * 6.2831853) * RAE_BIOME_PATH_MEANDER;
  var best = 1e9;
  for (var i = 0; i < RAE_BIOME_PATH_SPOKES; i = i + 1) {
    let angle = f32(i) * 6.2831853 / f32(RAE_BIOME_PATH_SPOKES) + 0.6;
    let direction = vec2<f32>(cos(angle), sin(angle));
    if (dot(p, direction) <= 0.0) { continue; }
    let perpendicular = p.x * direction.y - p.y * direction.x;
    best = min(best, abs(perpendicular - wander));
  }
  let weight = 1.0 - smoothstep(RAE_BIOME_PATH_HALF,
                                RAE_BIOME_PATH_HALF + RAE_BIOME_PATH_FADE,
                                best);
  let onLand = smoothstep(RAE_BIOME_WATER_LEVEL + 0.02,
                          RAE_BIOME_WATER_LEVEL + 0.16,
                          raeBiomeElevation(p));
  return clamp(weight * onLand, 0.0, 1.0);
}

fn raeBiomeMoisture(p: vec2<f32>) -> f32 {
  let m = raeNoiseFbm2(p / RAE_BIOME_MOIST_SCALE, 3u, 2.0, 0.5, RAE_BIOME_SEED + 7919u);
  return clamp(m * 0.5 + 0.5, 0.0, 1.0);
}

fn raeBiomeSlope(p: vec2<f32>) -> f32 {
  let eps = 1.5;
  let gx = (raeBiomeElevation(p + vec2<f32>(eps, 0.0)) - raeBiomeElevation(p - vec2<f32>(eps, 0.0))) / (2.0 * eps);
  let gy = (raeBiomeElevation(p + vec2<f32>(0.0, eps)) - raeBiomeElevation(p - vec2<f32>(0.0, eps))) / (2.0 * eps);
  return clamp(sqrt(gx * gx + gy * gy) * 8.0, 0.0, 1.0);
}

// Classify soft material weights (sum to 1) + dominant id + coarse biome. Mirrors
// world_biome.rae classifyWeights exactly.
fn raeBiomeClassify(elev: f32, moist: f32, slope: f32) -> RaeBiome {
  let wl = RAE_BIOME_WATER_LEVEL;
  let sandHi = wl + RAE_BIOME_BEACH_BAND;
  let wWater = 1.0 - smoothstep(wl, wl + 0.02, elev);
  let land = 1.0 - wWater;
  let wSand = land * smoothstep(wl, wl + 0.02, elev) * (1.0 - smoothstep(sandHi - 0.02, sandHi, elev));
  // SLOPE ALONE MUST NOT MAKE A BEACH INTO ROCK.
  //
  // The shore is the steepest ground on the island by construction -- it is
  // where the land falls to the water -- so a purely slope-driven rock term
  // claimed the entire coastal strip. Rendering the materials as marker colours
  // showed the "beach" was mostly ROCK with a thin band of sand at the waterline,
  // which is why it read grey instead of warm and why sand measured 4.4% of the
  // ground against the reference's 40%.
  //
  // Rock now means cliff or highland: the slope term is suppressed just above the
  // sand band, while genuinely high ground stays rocky regardless of slope.
  let notShore = smoothstep(sandHi, sandHi + RAE_BIOME_SHORE_ROCK_FADE, elev);
  let wRock = land * clamp(smoothstep(RAE_BIOME_ROCK_SLOPE, RAE_BIOME_ROCK_SLOPE + 0.12, slope) * notShore
    + smoothstep(0.35, 0.55, elev), 0.0, 1.0);
  let aboveSand = smoothstep(sandHi, sandHi + 0.04, elev);
  let flat = 1.0 - smoothstep(RAE_BIOME_ROCK_SLOPE * 0.5, RAE_BIOME_ROCK_SLOPE, slope);
  let lowland = 1.0 - smoothstep(0.15, 0.35, elev);
  let wetness = smoothstep(RAE_BIOME_SWAMP_MOIST, RAE_BIOME_SWAMP_MOIST + 0.1, moist);
  let wMud = land * aboveSand * flat * lowland * wetness * (1.0 - wRock);
  let wGrass = land * aboveSand * (1.0 - wRock) * (1.0 - wMud);
  var sum = wWater + wSand + wMud + wGrass + wRock;
  if (sum < 0.0001) { sum = 1.0; }
  var o: RaeBiome;
  o.elevation = elev;
  o.moisture = moist;
  o.slope = slope;
  o.wWater = wWater / sum;
  o.wSand = wSand / sum;
  o.wMud = wMud / sum;
  o.wGrass = wGrass / sum;
  o.wRock = wRock / sum;
  // Dominant material.
  var mat = 3;              // grass
  var best = o.wGrass;
  if (o.wWater > best) { best = o.wWater; mat = 0; }
  if (o.wSand > best) { best = o.wSand; mat = 1; }
  if (o.wMud > best) { best = o.wMud; mat = 2; }
  if (o.wRock > best) { best = o.wRock; mat = 4; }
  o.material = mat;
  var biome = 3;           // grassland
  if (mat == 0) { biome = 0; }        // ocean
  else if (mat == 1) { biome = 1; }   // beach
  else if (mat == 2) { biome = 2; }   // swamp
  else if (mat == 4) { biome = 4; }   // highland
  o.biome = biome;
  return o;
}

fn raeBiomeSample(p: vec2<f32>) -> RaeBiome {
  return raeBiomeClassify(raeBiomeElevation(p), raeBiomeMoisture(p), raeBiomeSlope(p));
}

// Splat palette — MUST match world_biome.rae grassMatColor()/etc. Blends the
// material weights into one ground colour (terrain splat + grass gradient bottom).
fn raeBiomeSplatColor(b: RaeBiome) -> vec3<f32> {
  let grass = vec3<f32>(0.105, 0.245, 0.055);
  let sand  = vec3<f32>(0.74, 0.66, 0.44);
  let mud   = vec3<f32>(0.24, 0.19, 0.11);
  let rock  = vec3<f32>(0.34, 0.33, 0.32);
  let water = vec3<f32>(0.06, 0.17, 0.26);
  return grass * b.wGrass + sand * b.wSand + mud * b.wMud + rock * b.wRock + water * b.wWater;
}

fn raeBiomeGroundColor(p: vec2<f32>) -> vec3<f32> {
  return raeBiomeSplatColor(raeBiomeSample(p));
}

// How far one unit of elevation lifts the ground. MUST match terrainAmplitude in
// the game's terrain.rae — the CPU builds the mesh from it and anything placed on
// the GPU has to land on that same surface.
const RAE_BIOME_TERRAIN_AMPLITUDE: f32 = 3.2;

// THE ground height, for anything the GPU scatters onto the terrain.
//
// Every system that puts something on the ground needs this, and each copy is a
// chance for them to disagree. The grass compute pass used to carry its own
// two-octave Perlin, so once the terrain moved to the biome field the blades were
// standing on a surface that no longer existed — hovering over water among other
// things. One definition, shared.
//
// Water is one height: elevation below the water level clamps to exactly the
// water level, matching terrainHeightAt on the CPU.
fn raeTerrainHeight(p: vec2<f32>, groundZ: f32) -> f32 {
  let e = max(raeBiomeElevation(p), RAE_BIOME_WATER_LEVEL);
  return groundZ + e * RAE_BIOME_TERRAIN_AMPLITUDE;
}

// Grass-material weight at a spot — what vegetation placement should test.
fn raeBiomeGrassWeight(p: vec2<f32>) -> f32 {
  return raeBiomeSample(p).wGrass;
}
