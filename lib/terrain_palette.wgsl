// Terrain ground PALETTE — the grade-calibrated colours (#13).
//
// SPLIT OUT of terrain_detail.wgsl so it can be OWNED PER APP. These RAE_TERRAIN_*
// constants are measured albedos that encode the game's specific lighting AND its
// colour grade (Khronos PBR Neutral + the display grade in src/grade.rae) — run
// through a different tone map they read wrong. They used to live in the shared
// lib, so example 114 (neutral ACES) inherited the game's calibration and could
// not diverge.
//
// terrainSplatWgsl() (lib/gbuffer_terrain.rae) now composes an app override if it
// exists — an app ships its own calibrated copy at assets/terrain_palette.wgsl
// (bundled on iOS too) — and falls back to THIS file otherwise. So the game owns
// its calibration app-side, and another app can recalibrate for its own grade by
// editing its copy (or this default) without touching the other. This file is the
// shared DEFAULT; it currently carries the game's values as a reasonable starting
// point, but nothing else is bound to them.
//
// Recalibrate against the plate the usual way (render → tools/match_reference.py →
// step each channel toward the target by a damped ratio); see AGENTS.md.

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
const RAE_TERRAIN_GRASS: vec3<f32> = vec3<f32>(0.539, 0.632, 0.256);
// ALBEDO SATURATES AT 1.0. MEASURED, not assumed -- and an earlier comment here
// claiming otherwise was wrong and cost a calibration round.
//
// Driving sand's red from 1.24 to 3.0 moved the rendered red not at all: it sat
// at 0.55 while green and blue kept climbing, which is why the beach went
// PALER and LESS warm the harder it was pushed. Whatever the albedo is written
// into, it does not carry values above one.
//
// So this holds the reference's sand RATIO (0.906 : 0.749 : 0.647 normalised to
// red) and brightness comes from exposure, which is applied after the G-buffer
// and is not capped. Grass is re-calibrated down to compensate for the exposure
// lift -- it has the headroom, sand does not.
const RAE_TERRAIN_SAND:  vec3<f32> = vec3<f32>(1.000, 0.977, 0.697);
const RAE_TERRAIN_MUD:   vec3<f32> = vec3<f32>(0.760, 0.560, 0.230);
// Dirt road: darker and browner than beach sand so both remain legible where
// the route crosses the shore.
const RAE_TERRAIN_PATH:  vec3<f32> = vec3<f32>(0.720, 0.520, 0.330);
const RAE_TERRAIN_ROCK:  vec3<f32> = vec3<f32>(0.520, 0.470, 0.350);
const RAE_TERRAIN_WATER: vec3<f32> = vec3<f32>(0.001, 0.327, 0.599);

// How strongly the noise breaks each material up. Grass wants visible patchiness;
// water wants almost none, or the sea looks like cling film.
// How far the patch/grain field is expanded about its midpoint before it drives
// the per-material variation. See raeTerrainDetailColor.
const RAE_TERRAIN_VAR_STRETCH: f32 = 2.30;

const RAE_TERRAIN_VAR_GRASS: f32 = 1.15;
// Sand's variation has to stay UNDER the albedo ceiling. raeTerrainVary scales by
// up to (1 - a/2 + a), so 0.80 pushed sand to 1.4x an albedo already at 1.0 --
// everything above the cap clipped and the beach rendered as flat white.
const RAE_TERRAIN_VAR_SAND:  f32 = 0.38;
const RAE_TERRAIN_VAR_MUD:   f32 = 0.30;
const RAE_TERRAIN_VAR_PATH:  f32 = 0.42;
const RAE_TERRAIN_VAR_ROCK:  f32 = 0.30;
const RAE_TERRAIN_VAR_WATER: f32 = 0.06;
