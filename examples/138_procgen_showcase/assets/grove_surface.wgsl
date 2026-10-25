// The autumn grove's ground for the lib GPU grass (lib/grass_compute.wgsl,
// composed after this part): the island's height (GroveScene.groundHeight),
// grass inside the island's rim, none on the path, and none under the rocks
// and trunks the app lists in the surface field: the count, then x, y and
// radius per obstacle (GroveGrass.groveObstacleField).

// The two biome constants the grass pass reads. The grove has no beach, so
// every sample sits well above the sand band.
const RAE_BIOME_WATER_LEVEL: f32 = -0.28;
const RAE_BIOME_BEACH_BAND: f32 = 0.24;

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

@group(0) @binding(4) var<storage, read> groveObstacles: array<f32>;

fn raeTerrainHeight(p: vec2<f32>, groundZ: f32) -> f32 {
  let radius = sqrt((p.x + 15.0) * (p.x + 15.0) / 1849.0 + (p.y - 6.0) * (p.y - 6.0) / 784.0);
  let base = 0.12 * sin(p.x * 0.36) * sin(p.y * 0.3);
  if (radius > 0.9167) {
    let rim = (radius - 0.9167) * 12.0;
    return groundZ + base * (1.0 - rim) - 0.35 * rim;
  }
  return groundZ + base;
}

fn groveUnderObstacle(p: vec2<f32>) -> bool {
  let count = i32(groveObstacles[0]);
  for (var i = 0; i < count; i = i + 1) {
    let centre = vec2<f32>(groveObstacles[1 + i * 3], groveObstacles[2 + i * 3]);
    if (distance(p, centre) < groveObstacles[3 + i * 3]) { return true; }
  }
  return false;
}

fn raeGrassObstacle(p: vec2<f32>) -> f32 {
  return select(0.0, 1.0, groveUnderObstacle(p));
}

fn raeBiomeSample(p: vec2<f32>) -> RaeBiome {
  var sample: RaeBiome;
  sample.elevation = 1.0;
  sample.moisture = 0.5;
  sample.slope = 0.0;
  sample.material = 3;
  sample.biome = 0;
  sample.wWater = 0.0;
  sample.wSand = 0.0;
  sample.wMud = 0.0;
  sample.wRock = 0.0;
  let insideIsland = (p.x + 15.0) * (p.x + 15.0) / 1764.0 + (p.y - 6.0) * (p.y - 6.0) / 729.0 <= 0.93;
  sample.wGrass = select(0.0, 1.0, insideIsland && !groveUnderObstacle(p));
  return sample;
}
