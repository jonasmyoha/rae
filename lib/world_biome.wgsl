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
const RAE_BIOME_BEACH_BAND: f32 = 0.06;
const RAE_BIOME_ROCK_SLOPE: f32 = 0.30;
const RAE_BIOME_SWAMP_MOIST: f32 = 0.6;

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

fn raeBiomeElevation(p: vec2<f32>) -> f32 {
  return raeNoiseFbm2(p / RAE_BIOME_ELEV_SCALE, 4u, 2.0, 0.5, RAE_BIOME_SEED);
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
  let wRock = land * clamp(smoothstep(RAE_BIOME_ROCK_SLOPE, RAE_BIOME_ROCK_SLOPE + 0.12, slope)
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
