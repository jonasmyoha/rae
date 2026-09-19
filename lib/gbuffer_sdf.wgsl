// Metaball clusters into the G-buffer (#392): raymarch, then the G-buffer
// encode — shaded by the same lighting pass as every triangle. Composed after
// lib/gbuffer_octahedral.wgsl (lib/GbufferSdf.rae).
//
// THE DISTANCE FIELD IS TRANSCRIBED, NOT SHARED: the legacy forward renderer
// keeps its own copy in compiler/runtime/runtime_gpu3d_sdf.c. mapScene /
// sceneAlbedo / sceneNormal are a literal copy — if one changes, both must,
// and the smooth-union weight `h` that fuses distances is the same weight that
// mixes colour in both.
//
// REVERSE-Z: the deferred depth buffer clears to 0 and tests Greater (#367);
// the emitted frag_depth follows the frame it writes into.
struct Frame {
  viewProj: mat4x4<f32>,
  invViewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  camPos: vec4<f32>,
};
struct Params {
  info: vec4<u32>,
  baseColorMetallic: vec4<f32>,
  emissiveRoughness: vec4<f32>,
  blend: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> balls: array<vec4<f32>>;
@group(0) @binding(2) var<uniform> P: Params;
@group(0) @binding(3) var<storage, read> ballColors: array<vec4<f32>>;
fn smoothMin(a: f32, b: f32, k: f32) -> f32 {
  let h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}
fn mapScene(p: vec3<f32>) -> f32 {
  var d = 10000.0;
  var i = 0u;
  loop {
    if (i >= P.info.x) { break; }
    let b = balls[i];
    d = smoothMin(d, length(p - b.xyz) - b.w, P.blend.x);
    i = i + 1u;
  }
  return d;
}
fn sceneAlbedo(p: vec3<f32>) -> vec3<f32> {
  var d = 10000.0;
  var col = vec3<f32>(0.0);
  var i = 0u;
  loop {
    if (i >= P.info.x) { break; }
    let b = balls[i];
    let di = length(p - b.xyz) - b.w;
    let h = clamp(0.5 + 0.5 * (di - d) / P.blend.x, 0.0, 1.0);
    col = mix(ballColors[i].rgb, col, h);
    d = mix(di, d, h) - P.blend.x * h * (1.0 - h);
    i = i + 1u;
  }
  return col;
}
fn sceneNormal(p: vec3<f32>) -> vec3<f32> {
  let e = 0.003;
  return normalize(vec3<f32>(
    mapScene(p + vec3<f32>(e, 0.0, 0.0)) - mapScene(p - vec3<f32>(e, 0.0, 0.0)),
    mapScene(p + vec3<f32>(0.0, e, 0.0)) - mapScene(p - vec3<f32>(0.0, e, 0.0)),
    mapScene(p + vec3<f32>(0.0, 0.0, e)) - mapScene(p - vec3<f32>(0.0, 0.0, e))));
}
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) ndc: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VsOut {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  var o: VsOut;
  o.pos = vec4<f32>(points[vi], 0.0, 1.0);
  o.ndc = points[vi];
  return o;
}
struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
  @builtin(frag_depth) depth: f32,
};
@fragment
fn fs(in: VsOut) -> FsOut {
  let nearH = F.invViewProj * vec4<f32>(in.ndc, 1.0, 1.0);
  let farH  = F.invViewProj * vec4<f32>(in.ndc, 0.0, 1.0);
  let rayOrigin = F.camPos.xyz;
  let rayDir = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);
  var travel = 0.05;
  var hit = false;
  var step = 0u;
  loop {
    if (step >= 112u || travel > 80.0) { break; }
    let d = mapScene(rayOrigin + rayDir * travel);
    if (d < max(0.0015, travel * 0.00035)) { hit = true; break; }
    travel = travel + max(d * 0.72, 0.003);
    step = step + 1u;
  }
  if (!hit) { discard; }
  let worldPos = rayOrigin + rayDir * travel;
  let N = sceneNormal(worldPos);
  let oct = octEncode(N);
  let albedo = sceneAlbedo(worldPos);
  let rough = clamp(P.emissiveRoughness.a, 0.045, 1.0);
  let clip = F.viewProj * vec4<f32>(worldPos, 1.0);
  let clipPrev = F.prevViewProj * vec4<f32>(worldPos, 1.0);
  let motion = (clip.xy / clip.w - clipPrev.xy / clipPrev.w) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var emissive = 1.0;
  var mode = 0.0;
  let emitPeak = max(P.emissiveRoughness.r, max(P.emissiveRoughness.g, P.emissiveRoughness.b));
  if (emitPeak > 0.0) {
    emissive = clamp(log(1.0 + emitPeak) / 6.91, 0.0, 1.0);
    mode = 0.33333333;
  }
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, mode);
  o.gbb = vec4<f32>(albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, clamp(P.baseColorMetallic.a, 0.0, 1.0), emissive);
  o.depth = clamp(clip.z / clip.w, 0.0, 1.0);
  return o;
}
