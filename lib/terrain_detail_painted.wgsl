// PAINTED GROUND detail: a variant of lib/terrain_detail.wgsl, chosen by
// listing this file in place of it (with lib/terrain_grass_painted.wgsl), as
// the walkers do. Warm sand with broad painted patches and fine grain, a wet
// band at the waterline, and NO surf painted on the ground: the realistic
// water (lib/water) owns all the moving foam. Needs only the five ground
// colors of the app's terrain_palette.wgsl.
// Walker ground: broad painted patches and fine, low-contrast sand grain.
// The water pass owns all moving foam; the ground never paints white surf.
// These constants are also consumed by the shared terrain texture blend.
const RAE_SANDGRASS_BLUR: f32 = 0.10;
const RAE_WET_SAND_DARKEN: f32 = 0.78;

fn raeTerrainDetailColor(position: vec2<f32>, biome: vec3<f32>, time: f32) -> vec3<f32> {
  let weights = raeBiomeClassify(biome.x, biome.y, biome.z);
  let broad = clamp(0.5 + raeNoiseFbm2(position * 0.14, 2u, 2.0, 0.5, 2328u), 0.0, 1.0);
  let grain = raeNoiseFbm2(position * 2.2, 1u, 2.0, 0.5, 1337u);
  let beachTop = RAE_BIOME_WATER_LEVEL + RAE_BIOME_BEACH_BAND;
  let grassMix = smoothstep(beachTop - RAE_SANDGRASS_BLUR, beachTop + RAE_SANDGRASS_BLUR, biome.x);
  let shoreWeight = weights.wSand + weights.wGrass;
  let wet = 1.0 - smoothstep(0.0, 0.10, biome.x - RAE_BIOME_WATER_LEVEL);
  let sand = RAE_TERRAIN_SAND * (0.94 + broad * 0.12 + grain * 0.035);
  let beach = sand * mix(1.0, RAE_WET_SAND_DARKEN, wet);
  let grass = terrainGrassColor(position, broad);
  let ground = mix(beach, grass, grassMix) * shoreWeight
    + beach * weights.wWater
    + RAE_TERRAIN_MUD * (0.92 + 0.16 * broad) * weights.wMud
    + RAE_TERRAIN_ROCK * (0.92 + 0.16 * broad) * weights.wRock;
  let road = RAE_TERRAIN_PATH * (0.94 + broad * 0.12 + grain * 0.025);
  return mix(ground, road, raeBiomePath(position) * (1.0 - weights.wWater));
}
