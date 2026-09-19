// Terrain ground palette for the walker example — this app's copy of
// lib/terrain_palette.wgsl (composed instead of it by rendererSetTerrainPalette).
//
// The lib default carries the game proto's calibration: albedos measured for
// its PBR-Neutral grade, where sand is pushed to the albedo ceiling to survive
// its exposure. Under this example's neutral ACES that beach renders as a
// snowfield. These values are calibrated for THIS scene instead, against the
// painted-meadow reference used for the game's terrain plates: a saturated
// mid green (sRGB ~ 73,133,54) with soft, low-contrast mottling — grass first,
// sand a warm dirt that reads as ground, not snow.
//
// Recalibrate the same way as the lib file: render, sample the ground in the
// screenshot, step each channel toward the target by a damped ratio.
const RAE_TERRAIN_GRASS: vec3<f32> = vec3<f32>(0.060, 0.290, 0.022);
const RAE_TERRAIN_SAND:  vec3<f32> = vec3<f32>(0.380, 0.230, 0.075);
const RAE_TERRAIN_MUD:   vec3<f32> = vec3<f32>(0.220, 0.130, 0.050);
const RAE_TERRAIN_PATH:  vec3<f32> = vec3<f32>(0.340, 0.150, 0.040);
const RAE_TERRAIN_ROCK:  vec3<f32> = vec3<f32>(0.360, 0.300, 0.220);
const RAE_TERRAIN_WATER: vec3<f32> = vec3<f32>(0.001, 0.327, 0.599);

// How strongly the noise breaks each material up. The reference meadow is
// painted with SUBTLE patches (p10..p90 of its green spans ~30 of 255), so the
// grass variation is well under the lib default's 1.15.
const RAE_TERRAIN_VAR_STRETCH: f32 = 2.30;

const RAE_TERRAIN_VAR_GRASS: f32 = 0.55;
const RAE_TERRAIN_VAR_SAND:  f32 = 0.30;
const RAE_TERRAIN_VAR_MUD:   f32 = 0.30;
const RAE_TERRAIN_VAR_PATH:  f32 = 0.35;
const RAE_TERRAIN_VAR_ROCK:  f32 = 0.30;
const RAE_TERRAIN_VAR_WATER: f32 = 0.06;
