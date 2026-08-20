// Terrain material splat (#533). Same G-buffer output as the static GB_WGSL, but
// the ground albedo comes from the biome classifier: each vertex samples
// raeBiomeGroundColor(worldXY) (per-vertex — cheap; the terrain is smooth so the
// interpolation reads fine), so the ground shows grass/sand/mud/rock/water blended
// by the field. Composed with lib/noise.wgsl + lib/world_biome.wgsl prepended (they
// provide raeNoiseFbm2 + raeBiomeGroundColor). Vertex layout = pos(3)/nrm(3)/uv(2),
// same as the static pipeline.

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
  @location(4) splat: vec3<f32>,   // biome ground colour, sampled per-vertex
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
  o.splat = raeBiomeGroundColor(world.xy);
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
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);
  o.gbb = vec4<f32>(in.splat, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, 0.0, d.params.y);
  return o;
}
