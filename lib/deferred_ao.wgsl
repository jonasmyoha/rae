struct LightU {
  invViewProj: mat4x4<f32>,
  camPos: vec4<f32>,
  sunDir: vec4<f32>,
  sunColor: vec4<f32>,
  ambSky: vec4<f32>,
  ambGround: vec4<f32>,
  clearColor: vec4<f32>,
  viewProj: mat4x4<f32>,
  skyParams: vec4<f32>,
  skyZenith: vec4<f32>,
  skyHorizon: vec4<f32>,
  hosek: array<vec4<f32>, 9>,
};
// SSAO at HALF the scene resolution (render-perf-6): it marches over the
// half-resolution depth (deferred_ao_depth.wgsl: one full-resolution sample
// kept per 2x2 block, and which one), and the separable blur and lighting's
// depth-aware upsample bring it back to full size. The slice directions
// follow a 4x4 interleaved pattern, so every 4x4 block of half pixels covers
// 16 different rotations and the blur averages them into a smooth result.
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var gbaTex: texture_2d<f32>;
@group(0) @binding(2) var aoDepthTex: texture_2d<f32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
const AO_RADIUS: f32 = 1.2;
const AO_SLICES: i32 = 3;
const AO_STEPS: i32 = 6;
const AO_BIAS: f32 = 0.06;
// A 4x4 Bayer order: neighboring half pixels get rotations far apart.
fn interleaved(halfPx: vec2<i32>) -> f32 {
  var order = array<f32, 16>(0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                             3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
  let cell = (halfPx.x & 3) + (halfPx.y & 3) * 4;
  return (order[cell] + 0.5) / 16.0;
}
// The full-resolution texel a half pixel kept (index 0-3 in its 2x2 block).
fn keptTexel(halfPx: vec2<i32>, index: f32, fullDim: vec2<i32>) -> vec2<i32> {
  let i = i32(index + 0.5);
  return min(halfPx * 2 + vec2<i32>(i & 1, i >> 1u), fullDim - vec2<i32>(1));
}
fn worldAt(fullPx: vec2<i32>, d: f32, fullDim: vec2<i32>) -> vec3<f32> {
  let uv = (vec2<f32>(fullPx) + vec2<f32>(0.5)) / vec2<f32>(fullDim);
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
  let w = L.invViewProj * ndc;
  return w.xyz / w.w;
}
fn Pclip(w: vec3<f32>) -> vec2<f32> {
  let c = L.viewProj * vec4<f32>(w, 1.0);
  let n = c.xy / max(c.w, 1e-4);
  return vec2<f32>(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
}
@fragment
fn fs(@builtin(position) fc: vec4<f32>) -> @location(0) vec4<f32> {
  let dim = vec2<i32>(textureDimensions(aoDepthTex));
  let fullDim = vec2<i32>(textureDimensions(gbaTex));
  let px = vec2<i32>(fc.xy);
  let kept = textureLoad(aoDepthTex, px, 0).xy;
  if (kept.x <= 0.0) { return vec4<f32>(1.0, 1.0, 1.0, 1.0); }
  let fullPx = keptTexel(px, kept.y, fullDim);
  let P = worldAt(fullPx, kept.x, fullDim);
  let gba = textureLoad(gbaTex, fullPx, 0);
  let N = octDecode(gba.xy);
  let noise = interleaved(px);
  let side = P + vec3<f32>(AO_RADIUS, 0.0, 0.0);
  let sideClip = Pclip(side);
  let pClip = Pclip(P);
  // In HALF pixels: the same world radius covers half as many of them.
  var marchPx = length((sideClip - pClip) * vec2<f32>(dim)) * 0.5;
  marchPx = clamp(marchPx, 2.0, 48.0);
  let stepPxLen = marchPx / f32(AO_STEPS);
  var occl = 0.0;
  for (var s = 0; s < AO_SLICES; s = s + 1) {
    let ang = (f32(s) + noise) * 3.14159265 / f32(AO_SLICES);
    let dir = vec2<f32>(cos(ang), sin(ang));
    var maxSin = 0.0;
    let radialJitter = fract(noise * 7.0 + f32(s) * 0.6180339887);
    for (var t = 1; t <= AO_STEPS; t = t + 1) {
      let stepPx = dir * ((f32(t) - 1.0 + radialJitter) * stepPxLen);
      let sp = px + vec2<i32>(stepPx);
      if (sp.x < 0 || sp.y < 0 || sp.x >= dim.x || sp.y >= dim.y) { break; }
      let sampleKept = textureLoad(aoDepthTex, sp, 0).xy;
      if (sampleKept.x <= 0.0) { continue; }
      let S = worldAt(keptTexel(sp, sampleKept.y, fullDim), sampleKept.x, fullDim);
      let v = S - P;
      let dist = length(v);
      if (dist < 0.001 || dist > AO_RADIUS) { continue; }
      let sinE = dot(N, v) / dist - AO_BIAS;
      let falloff = 1.0 - (dist / AO_RADIUS) * (dist / AO_RADIUS);
      maxSin = max(maxSin, sinE * falloff);
    }
    occl = occl + clamp(maxSin, 0.0, 1.0);
  }
  let ao = clamp(1.0 - occl / f32(AO_SLICES), 0.0, 1.0);
  return vec4<f32>(ao, ao, ao, 1.0);
}
