// The walker examples' palette (lib/app3d/WalkerApp: 136, 137, 143): warm
// paths and pale mineral sand. The grass is not a constant here: it is the
// frame's ground palette (app3d/GroundPalette, terrain_grass_painted.wgsl).
// Linear albedos; the local detail shader adds restrained spatial variation.
// The submerged sand stays bright enough to show through turquoise water.
const RAE_TERRAIN_SAND:  vec3<f32> = vec3<f32>(0.70, 0.42, 0.12);
const RAE_TERRAIN_MUD:   vec3<f32> = vec3<f32>(0.25, 0.19, 0.10);
const RAE_TERRAIN_PATH:  vec3<f32> = vec3<f32>(0.34, 0.18, 0.065);
const RAE_TERRAIN_ROCK:  vec3<f32> = vec3<f32>(0.38, 0.35, 0.27);
