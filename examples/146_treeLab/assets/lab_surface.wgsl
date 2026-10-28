// The tree lab's ground for the lib GPU grass (lib/grass_compute.wgsl, composed
// after this part): flat ground at groundZ, grass everywhere except inside the
// circles the app lists in the surface field (the count, then x, y and radius
// per circle: the placeholder tree's trunk), and no path.

// The two biome constants the grass pass reads. The lab has no water, so every
// sample sits well above the sand band.
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

@group(0) @binding(4) var<storage, read> labObstacles: array<f32>;

fn raeTerrainHeight(p: vec2<f32>, groundZ: f32) -> f32 {
  return groundZ;
}

fn labUnderObstacle(p: vec2<f32>) -> bool {
  let count = i32(labObstacles[0]);
  for (var i = 0; i < count; i = i + 1) {
    let center = vec2<f32>(labObstacles[1 + i * 3], labObstacles[2 + i * 3]);
    if (distance(p, center) < labObstacles[3 + i * 3]) { return true; }
  }
  return false;
}

fn raeGrassObstacle(p: vec2<f32>) -> f32 {
  return select(0.0, 1.0, labUnderObstacle(p));
}

fn raeBiomePath(p: vec2<f32>) -> f32 {
  return 0.0;
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
  sample.wGrass = select(1.0, 0.0, labUnderObstacle(p));
  return sample;
}
