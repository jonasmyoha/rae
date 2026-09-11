struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,
};
struct GrassU {
  a: vec4<f32>,
  b: vec4<f32>,
  c: vec4<f32>,
  d: vec4<f32>,
  e: vec4<f32>,
  f: vec4<f32>,
  g: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
@group(0) @binding(2) var<uniform> GW: GrassU;
fn octWrap(v: vec2<f32>) -> vec2<f32> {
  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
  return (vec2<f32>(1.0) - abs(v.yx)) * s;
}
fn octEncode(n: vec3<f32>) -> vec2<f32> {
  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
  if (n.z < 0.0) { p = octWrap(p); }
  return p * 0.5 + vec2<f32>(0.5);
}
fn octDecode(e: vec2<f32>) -> vec3<f32> {
  let f = e * 2.0 - vec2<f32>(1.0);
  var n = vec3<f32>(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  let t = max(-n.z, 0.0);
  n = vec3<f32>(n.x + select(t, -t, n.x >= 0.0),
                n.y + select(t, -t, n.y >= 0.0), n.z);
  return normalize(n);
}
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) nrm: vec3<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) height: f32,
};
fn windBend(base: vec2<f32>, time: f32, strength: f32, gust: f32) -> f32 {
  let phase = base.x * 0.5 + base.y * 0.35;
  return strength * (0.22 * sin(time * 1.5 + phase) + 0.06 * gust * sin(time * 3.3 + phase * 1.9) + 0.10);
}
fn bladeLocal(vi: u32, tipW: f32) -> vec3<f32> {
  var corner = 0u;
  if (vi == 0u) { corner = 0u; }
  else if (vi == 1u) { corner = 1u; }
  else if (vi == 2u) { corner = 3u; }
  else if (vi == 3u) { corner = 0u; }
  else if (vi == 4u) { corner = 3u; }
  else { corner = 2u; }
  let top = (corner == 2u) || (corner == 3u);
  let right = (corner == 1u) || (corner == 3u);
  let z = select(0.0, 1.0, top);
  let w = select(0.5, tipW, top);
  let x = select(-w, w, right);
  return vec3<f32>(x, 0.0, z);
}
@vertex
fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {
  let d = draws[ii];
  let lp = bladeLocal(vi, d.params.w);
  var o: VsOut;
  o.height = lp.z;
  let base = vec2<f32>(d.model[3].x, d.model[3].y);
  let windDir = normalize(GW.f.xy);
  let zc = lp.z * lp.z;
  let wNow = d.model * vec4<f32>(lp, 1.0);
  let bendN = windBend(base, GW.a.w, GW.f.z, GW.f.w);
  let pNow = vec3<f32>(wNow.x + windDir.x * bendN * zc, wNow.y + windDir.y * bendN * zc, wNow.z);
  o.pos = F.viewProj * vec4<f32>(pNow, 1.0);
  o.nrm = normalize((d.model * vec4<f32>(0.0, 0.0, 1.0, 0.0)).xyz);
  o.inst = ii;
  o.clipNow = o.pos;
  let wPrev = d.prevModel * vec4<f32>(lp, 1.0);
  let bendP = windBend(base, GW.a.w - GW.b.w, GW.f.z, GW.f.w);
  let pPrev = vec3<f32>(wPrev.x + windDir.x * bendP * zc, wPrev.y + windDir.y * bendP * zc, wPrev.z);
  o.clipPrev = F.prevViewProj * vec4<f32>(pPrev, 1.0);
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
  let botCol = d.albedoMetallic.rgb;
  let topCol = botCol * GW.g.w;
  let albedo = mix(botCol, topCol, clamp(in.height, 0.0, 1.0));
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);
  o.gbb = vec4<f32>(albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);
  return o;
}
