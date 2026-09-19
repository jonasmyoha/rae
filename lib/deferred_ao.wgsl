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
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var gbaTex: texture_2d<f32>;
@group(0) @binding(2) var depthTex: texture_depth_2d;
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
fn ign(pix: vec2<f32>) -> f32 {
  return fract(52.9829189 * fract(dot(pix, vec2<f32>(0.06711056, 0.00583715))));
}
fn worldAt(px: vec2<i32>, dim: vec2<i32>) -> vec3<f32> {
  let d = textureLoad(depthTex, px, 0);
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(dim);
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
  let dim = vec2<i32>(textureDimensions(depthTex));
  let px = vec2<i32>(fc.xy);
  let d = textureLoad(depthTex, px, 0);
  if (d <= 0.0) { return vec4<f32>(1.0, 1.0, 1.0, 1.0); }
  let P = worldAt(px, dim);
  let gba = textureLoad(gbaTex, px, 0);
  let N = octDecode(gba.xy);
  let noise = ign(fc.xy);
  let side = P + vec3<f32>(AO_RADIUS, 0.0, 0.0);
  let sideClip = Pclip(side);
  let pClip = Pclip(P);
  var marchPx = length((sideClip - pClip) * vec2<f32>(dim)) * 0.5;
  marchPx = clamp(marchPx, 4.0, 96.0);
  let stepPxLen = marchPx / f32(AO_STEPS);
  var occl = 0.0;
  for (var s = 0; s < AO_SLICES; s = s + 1) {
    let ang = (f32(s) + noise) * 3.14159265 / f32(AO_SLICES);
    let dir = vec2<f32>(cos(ang), sin(ang));
    var maxSin = 0.0;
    let radialJitter = fract(noise + f32(s) * 0.6180339887);
    for (var t = 1; t <= AO_STEPS; t = t + 1) {
      let stepPx = dir * ((f32(t) - 1.0 + radialJitter) * stepPxLen);
      let sp = px + vec2<i32>(stepPx);
      if (sp.x < 0 || sp.y < 0 || sp.x >= dim.x || sp.y >= dim.y) { break; }
      let sd = textureLoad(depthTex, sp, 0);
      if (sd <= 0.0) { continue; }
      let S = worldAt(sp, dim);
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
