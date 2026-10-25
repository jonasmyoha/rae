// Grove scene field for the shared terrain material. The authored mesh owns
// island height and paths; there is no island-biome noise or copied terrain shader.
const RAE_BIOME_WATER_LEVEL: f32 = -0.28;
const RAE_BIOME_BEACH_BAND: f32 = 0.24;
const RAE_TERRAIN_GRASS: vec3<f32> = vec3<f32>(0.13, 0.23, 0.055);
const RAE_TERRAIN_SAND: vec3<f32> = vec3<f32>(0.48, 0.32, 0.16);
const RAE_TERRAIN_PATH: vec3<f32> = vec3<f32>(0.48, 0.32, 0.16);
const RAE_WET_SAND_DARKEN: f32 = 1.0;
const RAE_SANDGRASS_BLUR: f32 = 0.10;
struct GroveWeights { wWater: f32, wSand: f32, wGrass: f32, }
fn raeBiomeElevation(position: vec2<f32>) -> f32 { return 1.0; }
fn raeBiomeMoisture(position: vec2<f32>) -> f32 { return 0.5; }
fn raeBiomeSlope(position: vec2<f32>) -> f32 { return 0.0; }
fn raeBiomeClassify(elevation: f32, moisture: f32, slope: f32) -> GroveWeights {
  return GroveWeights(0.0, 0.0, 1.0);
}
fn raeTerrainDetailColor(position: vec2<f32>, biome: vec3<f32>, time: f32) -> vec3<f32> {
  let grass = grassGroundColor(RAE_TERRAIN_GRASS, position, RAE_GRASS_PATCH_SCALE, RAE_GRASS_PATCH_COVERAGE);
  return mix(grass, RAE_TERRAIN_PATH, raeBiomePath(position));
}
