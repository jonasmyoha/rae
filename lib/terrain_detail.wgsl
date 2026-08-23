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

// GROUND PALETTE, measured off the reference plate.
//
// These replace the absolute colour ramps ported from procgen/texture.rae. Those
// were authored to look right as standalone texture swatches on a neutral
// background; run through this scene's sunlight they blew out — sand read as
// snow and water as pale ice.
//
// Instead the palette is art-directed to a target and the noise only MODULATES
// it. Medians sampled from ai_mockups/empty_plate/plate_a.png, divided down for
// sunlight (see raeTerrainLightFudge):
//
//   grass (0.533, 0.600, 0.314)   sand  (0.925, 0.784, 0.698)
//   water (0.024, 0.384, 0.635)   path  (0.698, 0.671, 0.400)
//
// Keeping colour identity in a constant and variation in a multiplier means the
// ground can be re-graded toward the reference by editing five vectors, without
// touching the noise.
// CALIBRATED AGAINST THE RENDER, not chosen to look right in isolation.
//
// The scene's sky irradiance is markedly blue, and ground normals point up, so
// the hemisphere term lands on the ground almost undiluted. Albedo picked to
// match the reference directly came out with blue running about 2x the target on
// every material. Sky exposure and turbidity scale all three channels together
// and cannot correct a per-channel bias, so the correction lives here.
//
// Method: render, sample the same patches in both images, multiply each albedo
// by target/measured, repeat. Recalibrate if the sun, the sky model or the
// ambient ever change -- these numbers encode this lighting.
const RAE_TERRAIN_GRASS: vec3<f32> = vec3<f32>(0.435, 0.451, 0.112);
const RAE_TERRAIN_SAND:  vec3<f32> = vec3<f32>(1.000, 0.723, 0.450);
const RAE_TERRAIN_MUD:   vec3<f32> = vec3<f32>(0.760, 0.560, 0.230);
const RAE_TERRAIN_ROCK:  vec3<f32> = vec3<f32>(0.520, 0.470, 0.350);
const RAE_TERRAIN_WATER: vec3<f32> = vec3<f32>(0.001, 0.164, 0.364);

// How strongly the noise breaks each material up. Grass wants visible patchiness;
// water wants almost none, or the sea looks like cling film.
const RAE_TERRAIN_VAR_GRASS: f32 = 0.34;
const RAE_TERRAIN_VAR_SAND:  f32 = 0.14;
const RAE_TERRAIN_VAR_MUD:   f32 = 0.22;
const RAE_TERRAIN_VAR_ROCK:  f32 = 0.26;
const RAE_TERRAIN_VAR_WATER: f32 = 0.06;

fn raeTerrainVary(base: vec3<f32>, amount: f32, t: f32) -> vec3<f32> {
  return base * (1.0 - amount * 0.5 + amount * t);
}

// Ground albedo, textured per pixel and blended by biome weight. `bio` is the
// interpolated (elevation, moisture, slope) from the vertex stage; `p` is world
// XY for the grain. The weights already sum to 1 (raeBiomeClassify normalises
// them), so this is a straight weighted sum with no renormalise.
fn raeTerrainDetailColor(p: vec2<f32>, bio: vec3<f32>) -> vec3<f32> {
  let b = raeBiomeClassify(bio.x, bio.y, bio.z);
  let d = clamp(raeNoiseFbm2(p * RAE_TERRAIN_DETAIL_HI_SCALE,
                             RAE_TERRAIN_DETAIL_OCT_HI, 2.0, 0.5,
                             RAE_TERRAIN_DETAIL_SEED) * 0.5 + 0.5, 0.0, 1.0);
  // Broad patchiness comes free from the interpolated moisture; fine grain from
  // the one fbm. Mixing them stops the ground reading as a single noise scale.
  let t = clamp(0.45 * clamp(bio.y, 0.0, 1.0) + 0.55 * d, 0.0, 1.0);
  return raeTerrainVary(RAE_TERRAIN_GRASS, RAE_TERRAIN_VAR_GRASS, t) * b.wGrass
       + raeTerrainVary(RAE_TERRAIN_SAND,  RAE_TERRAIN_VAR_SAND,  t) * b.wSand
       + raeTerrainVary(RAE_TERRAIN_MUD,   RAE_TERRAIN_VAR_MUD,   t) * b.wMud
       + raeTerrainVary(RAE_TERRAIN_ROCK,  RAE_TERRAIN_VAR_ROCK,  t) * b.wRock
       + raeTerrainVary(RAE_TERRAIN_WATER, RAE_TERRAIN_VAR_WATER, t) * b.wWater;
}
