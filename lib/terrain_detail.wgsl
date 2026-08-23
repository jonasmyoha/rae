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

// TWO SCALES, because one was not enough.
//
// Measured against the reference plate, the ground carried about 5x too little
// variation, and what it had died on downsampling — fine grain, no patches. Clean
// open meadow in the reference sits around std 0.026-0.07 in luminance and holds
// that through a 4x downsample, i.e. the variation lives in PATCHES a hundred-odd
// pixels across, with grain riding on top.
//
// (The 0.14-0.21 figures elsewhere in that image are trees and their shadows, not
// ground. Matching those with flat colour would read as noise soup.)
//
// Cost is two fbm per fragment rather than one. The octave counts are the knob;
// drop OCT_LO to 1 first on low-end parts, it is the cheaper loss.
const RAE_TERRAIN_DETAIL_OCT_HI: u32 = 2u;       // grain
const RAE_TERRAIN_DETAIL_OCT_LO: u32 = 2u;       // patches
const RAE_TERRAIN_DETAIL_HI_SCALE: f32 = 3.10;   // ~0.3 world units per feature
const RAE_TERRAIN_DETAIL_LO_SCALE: f32 = 0.75;   // ~1.3 world units per patch
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
const RAE_TERRAIN_VAR_GRASS: f32 = 1.15;
const RAE_TERRAIN_VAR_SAND:  f32 = 0.40;
const RAE_TERRAIN_VAR_MUD:   f32 = 0.30;
const RAE_TERRAIN_VAR_ROCK:  f32 = 0.30;
const RAE_TERRAIN_VAR_WATER: f32 = 0.06;

fn raeTerrainVary(base: vec3<f32>, amount: f32, t: f32) -> vec3<f32> {
  return base * (1.0 - amount * 0.5 + amount * t);
}

// --- water -----------------------------------------------------------------
//
// The sea is part of the terrain mesh, clamped flat at the water level, so it
// gets its look here rather than from a separate water pass. Three things carry
// it, in order of how much they matter from a top-down camera:
//
//   1. DEPTH. Shallow water over the beach shelf reads lighter and greener,
//      deep water darker and bluer. This is what makes a shoreline look like a
//      shoreline rather than a blue shape with a hard edge.
//   2. FOAM. A bright band where the land meets the sea, broken up by noise so
//      it is a surf line and not a stroke.
//   3. RIPPLE. Two crossing wave sets, cheap, mostly a brightness wobble. From
//      above there is no silhouette to distort, so displacement would be wasted.

const RAE_WATER_SHALLOW: vec3<f32> = vec3<f32>(0.075, 0.330, 0.430);
const RAE_WATER_FOAM: vec3<f32>    = vec3<f32>(0.900, 0.950, 0.960);
// How far below the water line counts as "deep", in elevation units.
const RAE_WATER_DEEP_AT: f32 = 0.16;
// Width of the surf band, in water weight.
const RAE_WATER_FOAM_BAND: f32 = 0.42;
// How opaque the surf gets at its strongest.
const RAE_WATER_FOAM_STRENGTH: f32 = 0.80;
const RAE_WATER_RIPPLE_SCALE: f32 = 5.5;
const RAE_WATER_RIPPLE_SPEED: f32 = 0.55;

fn raeWaterColor(p: vec2<f32>, elev: f32, wWater: f32, t: f32) -> vec3<f32> {
  // Depth from how far the ground sank below the water line.
  let depth = clamp((RAE_BIOME_WATER_LEVEL - elev) / RAE_WATER_DEEP_AT, 0.0, 1.0);
  var col = mix(RAE_WATER_SHALLOW, RAE_TERRAIN_WATER, depth);

  // Two crossing wave sets. Cheap sines rather than fbm: this rides on top of
  // the one noise evaluation the ground already pays for.
  let w1 = sin(p.x * RAE_WATER_RIPPLE_SCALE + t * RAE_WATER_RIPPLE_SPEED * 1.7);
  let w2 = sin((p.x * 0.6 + p.y) * RAE_WATER_RIPPLE_SCALE * 0.8
               - t * RAE_WATER_RIPPLE_SPEED * 1.1);
  col = col * (1.0 + 0.06 * (w1 * 0.5 + w2 * 0.5));

  return col;
}

// How much surf covers this spot, 0..1.
//
// SEPARATE FROM THE WATER COLOUR, and applied after the material blend. Foam
// belongs exactly where water and sand are equally weighted, so folding it into
// the water term meant multiplying it by wWater ~ 0.5 and then blending half of
// that away against the sand — a surf line that was mathematically present and
// visually absent. The reference's is a strong white band; this has to survive
// the blend to look like one.
fn raeWaterFoam(p: vec2<f32>, elev: f32, wWater: f32, t: f32) -> f32 {
  if (wWater < 0.04) { return 0.0; }
  let depth = clamp((RAE_BIOME_WATER_LEVEL - elev) / RAE_WATER_DEEP_AT, 0.0, 1.0);
  let edge = 1.0 - smoothstep(0.0, RAE_WATER_FOAM_BAND, abs(wWater - 0.5));
  // Ragged, and crawling up the beach, so the line reads as surf and not a stroke.
  let w = sin((p.x * 0.6 + p.y) * RAE_WATER_RIPPLE_SCALE * 0.8
              - t * RAE_WATER_RIPPLE_SPEED * 1.1);
  let ragged = clamp(0.45 + 0.55 * w, 0.0, 1.0);
  return clamp(edge * ragged * (1.0 - depth * 0.8), 0.0, 1.0);
}

// Ground albedo, textured per pixel and blended by biome weight. `bio` is the
// interpolated (elevation, moisture, slope) from the vertex stage; `p` is world
// XY for the grain. The weights already sum to 1 (raeBiomeClassify normalises
// them), so this is a straight weighted sum with no renormalise.
fn raeTerrainDetailColor(p: vec2<f32>, bio: vec3<f32>, t: f32) -> vec3<f32> {
  let b = raeBiomeClassify(bio.x, bio.y, bio.z);
  let d = clamp(raeNoiseFbm2(p * RAE_TERRAIN_DETAIL_HI_SCALE,
                             RAE_TERRAIN_DETAIL_OCT_HI, 2.0, 0.5,
                             RAE_TERRAIN_DETAIL_SEED) * 0.5 + 0.5, 0.0, 1.0);
  let blotch = clamp(raeNoiseFbm2(p * RAE_TERRAIN_DETAIL_LO_SCALE,
                                 RAE_TERRAIN_DETAIL_OCT_LO, 2.0, 0.5,
                                 RAE_TERRAIN_DETAIL_SEED + 991u) * 0.5 + 0.5, 0.0, 1.0);
  // Patches carry most of it, grain rides on top, and the interpolated moisture
  // adds a very broad drift so two distant meadows are not the same green.
  let v = clamp(0.20 * clamp(bio.y, 0.0, 1.0) + 0.55 * blotch + 0.25 * d, 0.0, 1.0);
  let ground = raeTerrainVary(RAE_TERRAIN_GRASS, RAE_TERRAIN_VAR_GRASS, v) * b.wGrass
       + raeTerrainVary(RAE_TERRAIN_SAND,  RAE_TERRAIN_VAR_SAND,  v) * b.wSand
       + raeTerrainVary(RAE_TERRAIN_MUD,   RAE_TERRAIN_VAR_MUD,   v) * b.wMud
       + raeTerrainVary(RAE_TERRAIN_ROCK,  RAE_TERRAIN_VAR_ROCK,  v) * b.wRock
       + raeWaterColor(p, bio.x, b.wWater, t) * b.wWater;
  let foam = raeWaterFoam(p, bio.x, b.wWater, t);
  return mix(ground, RAE_WATER_FOAM, foam * RAE_WATER_FOAM_STRENGTH);
}
