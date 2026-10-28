struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,    // xy the TAA jitter (z, w: the trees' wind, unused here)
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,
  fade: vec4<f32>,     // the level cross-fade (GbufferRecord [40..43]); 0 when not fading
};
struct GrassU {
  a: vec4<f32>,
  b: vec4<f32>,
  c: vec4<f32>,
  d: vec4<f32>,
  e: vec4<f32>,
  f: vec4<f32>,
  g: vec4<f32>,
  h: vec4<f32>,
  i: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
@group(0) @binding(2) var<uniform> GW: GrassU;
@group(0) @binding(3) var tuftImage: texture_2d_array<f32>;
@group(0) @binding(4) var tuftSampler: sampler;
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
  @location(5) uv: vec2<f32>,
};
const bladeSegments: u32 = 4u;
fn bladeLocal(vi: u32, curve: f32) -> vec3<f32> {
  let segment = vi / 6u;
  let segmentVertex = vi % 6u;
  var corner = 0u;
  if (segmentVertex == 0u) { corner = 0u; }
  else if (segmentVertex == 1u) { corner = 1u; }
  else if (segmentVertex == 2u) { corner = 3u; }
  else if (segmentVertex == 3u) { corner = 0u; }
  else if (segmentVertex == 4u) { corner = 3u; }
  else { corner = 2u; }
  let top = (corner == 2u) || (corner == 3u);
  let right = (corner == 1u) || (corner == 3u);
  let z = (f32(segment) + select(0.0, 1.0, top)) / f32(bladeSegments);
  let halfWidth = 0.025 + 0.475 * pow(max(1.0 - z, 0.0), 0.72);
  let centerX = curve * z * z;
  let x = centerX + select(-halfWidth, halfWidth, right);
  // A shallow bow makes highlights flow across a tuft instead of lighting the
  // entire old flat sheet as one polygon.
  let bow = (0.055 + abs(curve) * 0.045) * sin(z * 3.14159265);
  return vec3<f32>(x, bow, z);
}
fn bladeNormal(z: f32, curve: f32) -> vec3<f32> {
  let bowSlope = (0.055 + abs(curve) * 0.045) * 3.14159265 * cos(z * 3.14159265);
  let surfaceNormal = normalize(vec3<f32>(0.0, 1.0, -bowSlope));
  // Foliage normals lean toward the sky so both sides retain broad, soft
  // daylight while the curved surface still carries a readable highlight.
  return normalize(mix(surfaceNormal, vec3<f32>(0.0, 0.0, 1.0), 0.58));
}
// The alternatives share one bounded instance record and identical placement.
fn quadUv(vertex: u32) -> vec2<f32> {
  let corners = array<vec2<f32>, 6>(vec2<f32>(0,0), vec2<f32>(1,0), vec2<f32>(1,1),
    vec2<f32>(0,0), vec2<f32>(1,1), vec2<f32>(0,1));
  return corners[vertex % 6u];
}
fn tuftLocal(vertex: u32, curve: f32) -> vec3<f32> {
  let uv = quadUv(vertex);
  let angle = f32(vertex / 6u) * 2.0943951;
  let across = (uv.x - 0.5) * 2.8;
  let lean = curve * uv.y * uv.y * 0.45;
  return vec3<f32>(cos(angle) * across + lean, sin(angle) * across, uv.y);
}
fn ribbonTuft(vertex: u32, curve: f32) -> vec3<f32> {
  let blade = vertex / 24u;
  let angle = f32(blade) * 2.399963;
  let lean = curve * 0.35 + sin(f32(blade) * 1.73) * 0.8;
  let local = bladeLocal(vertex % 24u, lean);
  let height = 0.60 + f32((blade * 7u) % 12u) * (0.40 / 11.0);
  let reach = local.z * local.z * (0.85 + f32(blade % 3u) * 0.12);
  let across = local.x * 0.22 + reach;
  return vec3<f32>(cos(angle) * across, sin(angle) * across, local.z * height);
}
@vertex
fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {
  let d = draws[ii];
  var lp = bladeLocal(vi, d.params.w);
  var uv = vec2<f32>(0.5, 0.5);
  if (GW.i.w < 0.5) {
    let corner = quadUv(vi);
    lp = vec3<f32>((corner.x - 0.5) * mix(1.0, 0.08, corner.y), 0.0, corner.y);
  } else if (GW.i.w > 2.5) {
    lp = tuftLocal(vi, d.params.w);
    let corner = quadUv(vi);
    uv = vec2<f32>(corner.x, 1.0 - corner.y);
  } else if (GW.i.w > 1.5) {
    lp = ribbonTuft(vi, d.params.w);
  }
  var o: VsOut;
  o.height = lp.z;
  o.uv = uv;
  let base = vec2<f32>(d.model[3].x, d.model[3].y);
  // Grass reads the same base WindVector as foliage. Broad fronts travel in
  // that direction, while a small per-blade phase moves only the tip.
  let wind = GW.f.xyz;
  let timeNow = GW.f.w;
  let timePrev = GW.f.w - GW.b.w;
  let wNow = d.model * vec4<f32>(lp, 1.0);
  let offsetNow = grassWindOffset(base, lp.z, timeNow, wind);
  let pNow = vec3<f32>(wNow.xy + offsetNow, wNow.z);
  o.pos = F.viewProj * vec4<f32>(pNow, 1.0);
  var localNormal = bladeNormal(lp.z, d.params.w);
  if (GW.i.w < 0.5 || GW.i.w > 1.5) { localNormal = vec3<f32>(0.0, 0.0, 1.0); }
  let restNormal = normalize((d.model * vec4<f32>(localNormal, 0.0)).xyz);
  o.nrm = normalize(restNormal + vec3<f32>(-offsetNow * lp.z * 0.9, 0.0));
  o.inst = ii;
  o.clipNow = o.pos;
  let wPrev = d.prevModel * vec4<f32>(lp, 1.0);
  let offsetPrev = grassWindOffset(base, lp.z, timePrev, wind);
  let pPrev = vec3<f32>(wPrev.xy + offsetPrev, wPrev.z);
  o.clipPrev = F.prevViewProj * vec4<f32>(pPrev, 1.0);
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  return o;
}
struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};
// The surface at this fragment, in the G-buffer encoding: the G-buffer
// entry below writes it, the forward main pass's fsForward
// (forward_entry.wgsl) decodes and shades it.
fn gbufferSurface(in: VsOut) -> FsOut {
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
  let height = clamp(in.height, 0.0, 1.0);
  let grounded = mix(0.58, 1.0, smoothstep(0.0, 0.32, height));
  var albedo = mix(botCol, topCol, height) * grounded;
  // Sample outside the uniform branch so derivatives remain valid in both paths.
  let tuft = textureSample(tuftImage, tuftSampler, in.uv, 0);
  if (GW.i.w > 2.5) {
    if (tuft.a < 0.5) { discard; }
    let tone = clamp(dot(tuft.rgb, vec3<f32>(0.2126, 0.7152, 0.0722)) / 0.16, 0.62, 1.18);
    albedo = mix(botCol, topCol, height) * mix(0.94, tone, smoothstep(0.0, 0.32, height));
  }
  if (GW.i.w < 0.5) { albedo = mix(botCol, topCol, height); }
  // z = 1: TAA-reactive (render-perf-10): thin, wind-blown blades ghost on history
  o.gba = vec4<f32>(oct.x, oct.y, 1.0, d.params.z);
  o.gbb = vec4<f32>(albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);
  return o;
}
@fragment
fn fs(in: VsOut) -> FsOut {
  return gbufferSurface(in);
}
