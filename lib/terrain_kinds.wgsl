// An app's own terrain, painted by its ground kinds (docs/open-world-tiles.md (b),
// app3d/TerrainPatch, app3d/TerrainTiles createAppTerrainTiles): the same bindings and
// G-buffer output as terrain_splat.wgsl, but no WorldBiome. Each pixel reads its eight kinds'
// weights from the wrapping kind map (the terrain texture array's layers 0 and 1, a texel a
// lattice point, RAE_KINDS_SPACING m apart), sampled with linear filtering at a point moved a
// little by a noise, so the edge between two kinds is soft and never a straight texel edge, and
// paints each kind by its look (RAE_KIND<n>_R, _G, _B, _STYLE: app3d/TerrainPatch
// terrainKindConstants): 0 bare (its color), 1 grass, painted as the painted grass's root (the
// field, the frame's ground palette, its root lift, warmth and wind shimmer: so the blades and the
// ground are one color), 2 dry grass (the palette's straw). The grass is lit by the up normal as
// the blades' roots are. Wet ground darkens; snow lies by its depth and the slope.
//
// Compose after lib/grass_field.wgsl (the painted grass's field and palette), lib/wind.wgsl (the
// gust for the shimmer) and lib/snow.wgsl (GbufferTerrain terrainKindsShader).

override RAE_TERRAIN_TOON: f32 = 0.0;
// The kind map's texel spacing (the tiles' cellSize / segments) and how far (m) the noise
// moves a pixel's sample point, for soft, ragged edges between kinds
override RAE_KINDS_SPACING: f32 = 0.75;
override RAE_KINDS_WARP: f32 = 0.6;
override RAE_KIND0_R: f32 = 0.0;
override RAE_KIND0_G: f32 = 0.0;
override RAE_KIND0_B: f32 = 0.0;
override RAE_KIND0_STYLE: f32 = 0.0;
override RAE_KIND1_R: f32 = 0.0;
override RAE_KIND1_G: f32 = 0.0;
override RAE_KIND1_B: f32 = 0.0;
override RAE_KIND1_STYLE: f32 = 0.0;
override RAE_KIND2_R: f32 = 0.0;
override RAE_KIND2_G: f32 = 0.0;
override RAE_KIND2_B: f32 = 0.0;
override RAE_KIND2_STYLE: f32 = 0.0;
override RAE_KIND3_R: f32 = 0.0;
override RAE_KIND3_G: f32 = 0.0;
override RAE_KIND3_B: f32 = 0.0;
override RAE_KIND3_STYLE: f32 = 0.0;
override RAE_KIND4_R: f32 = 0.0;
override RAE_KIND4_G: f32 = 0.0;
override RAE_KIND4_B: f32 = 0.0;
override RAE_KIND4_STYLE: f32 = 0.0;
override RAE_KIND5_R: f32 = 0.0;
override RAE_KIND5_G: f32 = 0.0;
override RAE_KIND5_B: f32 = 0.0;
override RAE_KIND5_STYLE: f32 = 0.0;
override RAE_KIND6_R: f32 = 0.0;
override RAE_KIND6_G: f32 = 0.0;
override RAE_KIND6_B: f32 = 0.0;
override RAE_KIND6_STYLE: f32 = 0.0;
override RAE_KIND7_R: f32 = 0.0;
override RAE_KIND7_G: f32 = 0.0;
override RAE_KIND7_B: f32 = 0.0;
override RAE_KIND7_STYLE: f32 = 0.0;

const RAE_KIND_MAP_SIDE: f32 = 512.0;

struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,
  wind: vec4<f32>,
  previousWind: vec4<f32>,
  // x: the snow depth (m; lib/snow.wgsl, Gbuffer setGbufferSnow)
  scene: vec4<f32>,
  // x: how wet the ground is; y the painted grass's wind shimmer, z its warmth, w its root lift
  weather: vec4<f32>,
  // the air's drift (m) now (xy) and last frame (zw), for the gust field (lib/wind.wgsl)
  windDrift: vec4<f32>,
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,   // x = roughness, y = emissive(enc), z = mode
  fade: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
// The kind map: layer 0 kinds 0 - 3, layer 1 kinds 4 - 7 (RGBA8, wrapping)
@group(0) @binding(2) var terrainTex: texture_2d_array<f32>;
@group(0) @binding(3) var terrainSamp: sampler;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) nrm: vec3<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) worldXY: vec2<f32>,
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
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  return o;
}

struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};

fn kindsOctWrap(v: vec2<f32>) -> vec2<f32> {
  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
  return (vec2<f32>(1.0) - abs(v.yx)) * s;
}
fn kindsOctEncode(n: vec3<f32>) -> vec2<f32> {
  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
  if (n.z < 0.0) { p = kindsOctWrap(p); }
  return p * 0.5 + vec2<f32>(0.5);
}

// The look of kind `style` with color `color` at `position`, `paintValue` the painted grass's
// root value here (lift and shimmer in), `hue` its hue
fn kindColor(style: f32, color: vec3<f32>, grass: vec3<f32>, dry: vec3<f32>, paintValue: f32, hue: f32,
             warmth: f32) -> vec3<f32> {
  if (style > 1.5) { return dry * color; }
  if (style > 0.5) { return grassPaintColor(paintValue, hue, grass * color, 1.0, warmth); }
  return color;
}

fn gbufferSurface(in: VsOut) -> FsOut {
  let d = draws[in.inst];
  let grass = d.albedoMetallic.yzw;
  let dry = d.fade.xyz;
  // the kinds here, from the map at a point the noise moves a little
  let warp = vec2<f32>(perlin2(in.worldXY * 0.55, 911u), perlin2(in.worldXY * 0.55 + vec2<f32>(37.0, 11.0), 929u))
    * RAE_KINDS_WARP;
  let uv = ((in.worldXY + warp) / RAE_KINDS_SPACING + vec2<f32>(0.5)) / RAE_KIND_MAP_SIDE;
  let low = textureSample(terrainTex, terrainSamp, uv, 0);
  let high = textureSample(terrainTex, terrainSamp, uv, 1);
  let weights = array<f32, 8>(low.r, low.g, low.b, low.a, high.r, high.g, high.b, high.a);
  let colors = array<vec3<f32>, 8>(
    vec3<f32>(RAE_KIND0_R, RAE_KIND0_G, RAE_KIND0_B),
    vec3<f32>(RAE_KIND1_R, RAE_KIND1_G, RAE_KIND1_B),
    vec3<f32>(RAE_KIND2_R, RAE_KIND2_G, RAE_KIND2_B),
    vec3<f32>(RAE_KIND3_R, RAE_KIND3_G, RAE_KIND3_B),
    vec3<f32>(RAE_KIND4_R, RAE_KIND4_G, RAE_KIND4_B),
    vec3<f32>(RAE_KIND5_R, RAE_KIND5_G, RAE_KIND5_B),
    vec3<f32>(RAE_KIND6_R, RAE_KIND6_G, RAE_KIND6_B),
    vec3<f32>(RAE_KIND7_R, RAE_KIND7_G, RAE_KIND7_B)
  );
  let styles = array<f32, 8>(RAE_KIND0_STYLE, RAE_KIND1_STYLE, RAE_KIND2_STYLE, RAE_KIND3_STYLE, RAE_KIND4_STYLE, RAE_KIND5_STYLE, RAE_KIND6_STYLE, RAE_KIND7_STYLE);
  // the painted grass's root here, as its blades take it (grass_compute.wgsl)
  let paint = grassPaintAt(in.worldXY);
  var shimmer = 0.0;
  if (F.weather.y > 0.0) {
    shimmer = grassPaintShimmer(windGustAt(in.worldXY, F.wind, F.windDrift.xy)) * F.weather.y;
  }
  let paintValue = grassPaintRootValue(paint) + F.weather.w + shimmer;
  var albedo = vec3<f32>(0.0);
  var total = 0.0;
  var grassShare = 0.0;
  for (var k = 0; k < 8; k = k + 1) {
    let w = weights[k];
    if (w > 0.002) {
      albedo = albedo + w * kindColor(styles[k], colors[k], grass, dry, paintValue, paint.y, F.weather.z);
      total = total + w;
      if (styles[k] > 0.5 && styles[k] < 1.5) { grassShare = grassShare + w; }
    }
  }
  if (total > 0.0) {
    albedo = albedo / total;
    grassShare = grassShare / total;
  }
  // the grass lit by the up normal, as its blades' roots are
  let n = normalize(mix(normalize(in.nrm), vec3<f32>(0.0, 0.0, 1.0), grassShare));
  let oct = kindsOctEncode(n);
  let rough = clamp(d.params.x, 0.045, 1.0);
  var wetRough = rough;
  if (F.weather.x > 0.0) {
    albedo = albedo * mix(1.0, 0.62, F.weather.x);
    wetRough = rough * mix(1.0, 0.55, F.weather.x);
  }
  // snow by the depth and the slope, patchy at its edge
  if (F.scene.x > 0.0) {
    let snowNoise = clamp(0.5 + 0.5 * perlin2(in.worldXY * 0.18, 4111u), 0.0, 1.0);
    albedo = mix(albedo, snowAlbedo, snowGroundCover(F.scene.x, 1.0 - normalize(in.nrm).z, snowNoise));
  }
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, max(d.params.z, RAE_TERRAIN_TOON));
  o.gbb = vec4<f32>(albedo, wetRough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, d.params.y);
  return o;
}
@fragment
fn fs(in: VsOut) -> FsOut {
  return gbufferSurface(in);
}
