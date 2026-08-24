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
@group(0) @binding(2) var terrainTex: texture_2d<f32>;
@group(0) @binding(3) var terrainSamp: sampler;

// World units per tile repeat. A ground cell is 18 units; ~6 units per tile puts
// three repeats across a cell, matching the plate's patch scale without the
// repeat reading as wallpaper under the top-down camera.
const RAE_TERRAIN_TEX_SCALE: f32 = 0.16666667;
// Measured mean of assets/terrain_grass.png. The tile is DIVIDED by this so it
// carries only spatial variation (mean ~1), then multiplied by the calibrated
// RAE_TERRAIN_GRASS albedo -- the plate pixels are already lit, so using them
// raw as albedo would light them twice and blow out the frame. This keeps the
// #13 calibration intact while stamping the plate's own texture onto the ground.
const RAE_TERRAIN_GRASS_TEX_MEAN: vec3<f32> = vec3<f32>(0.644, 0.665, 0.395);

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
  // TEXTURED GRASS (#9). Stamp the plate tile over the procedural grass, weighted
  // by the grass biome weight so only grass takes it (sand/rock/water/road stay
  // procedural). `texBlend` = d.albedoMetallic.x, written per-frame by
  // drawTerrainMesh from a module global that defaults to 0 -- so an app that
  // never uploads a tile (example 114) blends nothing and this is exact identity.
  // Sampled unconditionally so the binding is never dead-stripped from the
  // pipeline's auto-derived layout.
  let texBlend = d.albedoMetallic.x;
  let b = raeBiomeClassify(in.bio.x, in.bio.y, in.bio.z);
  let texel = textureSample(terrainTex, terrainSamp, in.worldXY * RAE_TERRAIN_TEX_SCALE).rgb;
  let grassTex = RAE_TERRAIN_GRASS * (texel / RAE_TERRAIN_GRASS_TEX_MEAN);
  albedo = mix(albedo, grassTex, clamp(texBlend * b.wGrass, 0.0, 1.0));
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);
  o.gbb = vec4<f32>(albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, d.params.y);
  return o;
}
