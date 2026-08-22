// Per-fragment terrain material detail (#539 ported to the fragment stage).
//
// procgen/texture.rae bakes the five biome materials into images by feeding a
// low-frequency and a high-frequency fbm value into a per-material colour ramp.
// The ramps are pure functions of those two numbers, so they can be evaluated
// live per pixel instead — which is what this file does. Same look, no texture
// memory, no atlas bleeding at tile seams, and no sampler bindings (nothing else
// in this renderer binds a texture).
//
// MOBILE. ONE fbm per fragment, and that is the entire per-pixel noise budget.
//
// The biome FIELDS (elevation, moisture, slope) are NOT evaluated here. Sampling
// them per pixel costs about 23 octaves — elevation is 4-octave fbm, moisture 3,
// and slope calls elevation four more times — which measured as a 50% frame-rate
// drop. They are low-frequency and smooth, so the vertex stage computes them and
// interpolates; only raeBiomeClassify runs per pixel, and that is smoothsteps
// with no noise at all. The sharp shoreline comes from the classify step, so it
// survives; the cost does not.
//
// The low-frequency variation `n` reuses the interpolated moisture rather than
// paying for a second fbm — free, and broad damp/dry patches are what it means.
// RAE_TERRAIN_DETAIL_OCT_HI is the quality knob; 1 on low-end parts.
//
// Requires lib/noise.wgsl and lib/world_biome.wgsl prepended.

const RAE_TERRAIN_DETAIL_OCT_HI: u32 = 2u;       // fine grain — the only noise here
const RAE_TERRAIN_DETAIL_HI_SCALE: f32 = 3.10;   // world units -> high freq
const RAE_TERRAIN_DETAIL_SEED: u32 = 1337u;

// The per-material ramps from procgen/texture.rae texMaterialColor(), kept in the
// same order and with the same constants so a baked texture and this agree.
fn raeTerrainGrass(n: f32, d: f32) -> vec3<f32> {
  let base = 0.30 + n * 0.28;
  return vec3<f32>(0.10 + base * 0.22 + d * 0.06, 0.24 + base * 0.55, 0.08 + base * 0.16);
}
fn raeTerrainSand(n: f32, d: f32) -> vec3<f32> {
  let t = 0.60 + n * 0.24 + d * 0.06;
  return vec3<f32>(t + 0.14, t + 0.03, t - 0.20);
}
fn raeTerrainRock(n: f32, d: f32) -> vec3<f32> {
  let g = 0.32 + n * 0.36 - d * 0.14;
  return vec3<f32>(g, g * 0.98, g * 1.03);
}
fn raeTerrainMud(n: f32, d: f32) -> vec3<f32> {
  let t = 0.16 + n * 0.22;
  return vec3<f32>(t + 0.18, t + 0.08, t + 0.01);
}
fn raeTerrainWater(n: f32, d: f32) -> vec3<f32> {
  let hi = n * 0.28 + d * 0.28;
  return vec3<f32>(0.05 + hi, 0.22 + hi * 1.1, 0.42 + hi * 0.9);
}

// Ground albedo, textured per pixel and blended by biome weight. `bio` is the
// interpolated (elevation, moisture, slope) from the vertex stage; `p` is world
// XY for the grain. The weights already sum to 1 (raeBiomeClassify normalises
// them), so this is a straight weighted sum with no renormalise.
fn raeTerrainDetailColor(p: vec2<f32>, bio: vec3<f32>) -> vec3<f32> {
  let b = raeBiomeClassify(bio.x, bio.y, bio.z);
  let n = clamp(bio.y, 0.0, 1.0);
  let d = clamp(raeNoiseFbm2(p * RAE_TERRAIN_DETAIL_HI_SCALE,
                             RAE_TERRAIN_DETAIL_OCT_HI, 2.0, 0.5,
                             RAE_TERRAIN_DETAIL_SEED) * 0.5 + 0.5, 0.0, 1.0);
  return raeTerrainGrass(n, d) * b.wGrass
       + raeTerrainSand(n, d)  * b.wSand
       + raeTerrainMud(n, d)   * b.wMud
       + raeTerrainRock(n, d)  * b.wRock
       + raeTerrainWater(n, d) * b.wWater;
}
