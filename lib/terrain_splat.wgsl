// Terrain material splat (#533). Same G-buffer output as the static GB_WGSL, but
// the ground albedo comes from the biome classifier.
//
// PER FRAGMENT, not per vertex. This used to evaluate raeBiomeGroundColor at each
// vertex and interpolate. A ground tile is 18 world units across 12 segments —
// one vertex every 1.5 units, on a character 2 units tall — so from a top-down
// camera the ground was a smooth gradient with no surface detail and a smeared
// shoreline. The world XY is now carried to the fragment stage and the material
// evaluated there, which puts the biome boundary on the pixel and lets
// raeTerrainDetailColor add grain (lib/terrain_detail.wgsl).
//
// Composed with lib/noise.wgsl + lib/world_biome.wgsl + lib/terrain_detail.wgsl
// prepended. Vertex layout = pos(3)/nrm(3)/uv(2), same as the static pipeline.

struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,   // x = roughness, y = emissive(enc), z = mode
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
// Textured terrain (#9). The FIRST texture sampled anywhere in this renderer.
// One tile harvested from the reference plate, bound with a repeat sampler by
// lib/gbuffer_terrain.rae. Gated to identity when no tile is uploaded (see fs),
// so example 114 and any other terrain user is unaffected.
// A texture_2d_ARRAY now (#14): one layer per material, indexed to match
// gbuffer_terrain.rae's terrainLayers — 0 grass, 1 sand, 2 road, 3 water. Gated
// to identity when no tiles are uploaded (see fs), so example 114 is unaffected.
@group(0) @binding(2) var terrainTex: texture_2d_array<f32>;
@group(0) @binding(3) var terrainSamp: sampler;
const RAE_TL_GRASS: i32 = 0;
const RAE_TL_SAND:  i32 = 1;
const RAE_TL_ROAD:  i32 = 2;
const RAE_TL_WATER: i32 = 3;

// World units per tile repeat. A ground cell is 18 units; ~6 units per tile puts
// three repeats across a cell, matching the plate's patch scale without the
// repeat reading as wallpaper under the top-down camera.
const RAE_TERRAIN_TEX_SCALE: f32 = 0.16666667;
// Measured means of the plate-harvested tiles (assets/terrain_{grass,sand,road,
// water}.png). Each tile is DIVIDED by its mean so it carries only spatial
// variation (mean ~1), then multiplied by the calibrated RAE_TERRAIN_* albedo --
// the plate pixels are already lit, so using them raw as albedo would light them
// twice and blow out the frame. This keeps the #13 calibration intact while
// stamping the plate's own texture onto the ground.
const RAE_TERRAIN_GRASS_TEX_MEAN: vec3<f32> = vec3<f32>(0.644, 0.665, 0.395);
const RAE_TERRAIN_SAND_TEX_MEAN:  vec3<f32> = vec3<f32>(0.922, 0.798, 0.715);
const RAE_TERRAIN_ROAD_TEX_MEAN:  vec3<f32> = vec3<f32>(0.761, 0.640, 0.511);
const RAE_TERRAIN_WATER_TEX_MEAN: vec3<f32> = vec3<f32>(0.037, 0.378, 0.619);
// Water scrolls (the task's "(scrolling) water"): world units per second the
// water tile drifts, so the sea surface has moving detail over the procedural
// ripples. Small — a fast scroll reads as a conveyor belt from the fixed camera.
const RAE_TERRAIN_WATER_SCROLL: f32 = 0.06;

fn octWrap(v: vec2<f32>) -> vec2<f32> {
  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
  return (vec2<f32>(1.0) - abs(v.yx)) * s;
}
fn octEncode(n: vec3<f32>) -> vec2<f32> {
  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
  if (n.z < 0.0) { p = octWrap(p); }
  return p * 0.5 + vec2<f32>(0.5);
}

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) nrm: vec3<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) worldXY: vec2<f32>,  // world XY, for the per-pixel grain
  @location(5) bio: vec3<f32>,      // elevation, moisture, slope — low-freq, safe to interpolate
};

@vertex
fn vs(@builtin(instance_index) ii: u32,
      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {
  let d = draws[ii];
  var o: VsOut;
  let world = d.model * vec4<f32>(p, 1.0);
  o.pos = F.viewProj * world;
  o.nrm = normalize((d.model * vec4<f32>(n, 0.0)).xyz);
  o.inst = ii;
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * (d.prevModel * vec4<f32>(p, 1.0));
  o.worldXY = world.xy;
  // The expensive part of the biome stays here: elevation/moisture/slope are
  // ~23 octaves of noise between them, and they are smooth enough to interpolate.
  // The fragment only classifies them, which is where the sharp shoreline is.
  let elev = raeBiomeElevation(world.xy);
  o.bio = vec3<f32>(elev, raeBiomeMoisture(world.xy), raeBiomeSlope(world.xy));
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  return o;
}

struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};

@fragment
fn fs(in: VsOut) -> FsOut {
  let d = draws[in.inst];
  let n = normalize(in.nrm);
  let oct = octEncode(n);
  let rough = clamp(d.params.x, 0.045, 1.0);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var albedo = raeTerrainDetailColor(in.worldXY, in.bio, d.params.w);
  // TEXTURED TERRAIN (#9 grass, #14 the rest). Stamp each plate-harvested tile
  // over the procedural colour, weighted by that material's biome weight so only
  // the dominant material takes its own texture. Each tile is DIVIDED by its own
  // mean and multiplied by the calibrated RAE_TERRAIN_* albedo, so it adds the
  // plate's spatial texture WITHOUT re-lighting already-lit pixels or moving the
  // #13-calibrated mean. `texBlend` = d.albedoMetallic.x, written per-frame by
  // drawTerrainMesh from a module global that defaults to 0 -- so an app that
  // never uploads tiles (example 114) blends nothing and this is exact identity.
  // Sampled unconditionally so the array binding is never dead-stripped from the
  // pipeline's auto-derived layout. Rock keeps its procedural colour: the plate
  // has no rocky-ground/stone tile to harvest (its low-saturation regions are
  // shadowed greens), so there is no honest rock texture to stamp.
  let texBlend = d.albedoMetallic.x;
  let b = raeBiomeClassify(in.bio.x, in.bio.y, in.bio.z);
  // BLURRED sand/grass texture-stamp weights (#83). The procedural colour already
  // crossfades sand<->grass over a wide band (raeTerrainDetailColor), but the tile
  // TEXTURES below are stamped by the classify's near-line weights, which re-sharpens
  // the edge. Re-split the combined sand+grass weight across the same wide elevation
  // band so the textures dissolve into each other too — a soft, road-like border. The
  // sum is preserved, so the coast's edge against water/mud/rock is untouched.
  let sgSum83 = b.wSand + b.wGrass;
  let sgMix83 = smoothstep(RAE_BIOME_WATER_LEVEL + RAE_BIOME_BEACH_BAND - RAE_SANDGRASS_BLUR,
                           RAE_BIOME_WATER_LEVEL + RAE_BIOME_BEACH_BAND + RAE_SANDGRASS_BLUR, in.bio.x);
  let wGrass83 = sgSum83 * sgMix83;
  let wSand83 = sgSum83 * (1.0 - sgMix83);
  let uv = in.worldXY * RAE_TERRAIN_TEX_SCALE;
  let grassT = RAE_TERRAIN_GRASS * (textureSample(terrainTex, terrainSamp, uv, RAE_TL_GRASS).rgb / RAE_TERRAIN_GRASS_TEX_MEAN);
  let sandT  = RAE_TERRAIN_SAND  * (textureSample(terrainTex, terrainSamp, uv, RAE_TL_SAND ).rgb / RAE_TERRAIN_SAND_TEX_MEAN);
  // Water tile drifts with the clock (d.params.w carries time), for moving
  // surface detail over the procedural ripples/foam.
  let wuv = uv + vec2<f32>(d.params.w * RAE_TERRAIN_WATER_SCROLL, d.params.w * RAE_TERRAIN_WATER_SCROLL * 0.6);
  let waterT = RAE_TERRAIN_WATER * (textureSample(terrainTex, terrainSamp, wuv, RAE_TL_WATER).rgb / RAE_TERRAIN_WATER_TEX_MEAN);
  // Sequential blend by weight -- at any pixel one weight dominates, so this
  // reads as "the dominant material's texture" while shore blends average.
  albedo = mix(albedo, grassT, clamp(texBlend * wGrass83, 0.0, 1.0));
  albedo = mix(albedo, sandT,  clamp(texBlend * wSand83,  0.0, 1.0));
  // Water gets HALF weight: the sea already carries procedural foam/ripples, so
  // the tile is subtle surface detail, not a replacement -- full strength reads
  // as cracked ice under the top-down camera.
  albedo = mix(albedo, waterT, clamp(texBlend * b.wWater * 0.5, 0.0, 1.0));
  // Road is an overlay (raeBiomePath), not a biome weight; stamp it over land
  // under the same gate, below water so a shore crossing still reads wet.
  let roadT = RAE_TERRAIN_PATH * (textureSample(terrainTex, terrainSamp, uv, RAE_TL_ROAD).rgb / RAE_TERRAIN_ROAD_TEX_MEAN);
  albedo = mix(albedo, roadT, clamp(texBlend * raeBiomePath(in.worldXY) * (1.0 - b.wWater), 0.0, 1.0));
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);
  o.gbb = vec4<f32>(albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, d.params.y);
  return o;
}
