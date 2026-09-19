struct U {
  lightViewProj: mat4x4<f32>,
  invLightViewProj: mat4x4<f32>,
  lightDir: vec4<f32>,
  params: vec4<f32>,
  bounds: vec4<f32>,
};
@group(0) @binding(0) var<uniform> U0: U;
@group(0) @binding(1) var<storage, read> balls: array<vec4<f32>>;
fn smoothMin(a: f32, b: f32, k: f32) -> f32 {
  let h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}
fn mapScene(p: vec3<f32>) -> f32 {
  var d = 10000.0;
  var i = 0u;
  let n = u32(U0.params.x);
  loop {
    if (i >= n) { break; }
    let b = balls[i];
    d = smoothMin(d, length(p - b.xyz) - b.w, U0.params.y);
    i = i + 1u;
  }
  return d;
}
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) ndc: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VsOut {
  var corner = array<vec2<f32>, 6>(
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
    vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));
  let c = corner[vi];
  let p = mix(U0.bounds.xy, U0.bounds.zw, c);
  var o: VsOut;
  o.pos = vec4<f32>(p, 0.0, 1.0);
  o.ndc = p;
  return o;
}
@fragment
fn fs(in: VsOut) -> @builtin(frag_depth) f32 {
  let nearH = U0.invLightViewProj * vec4<f32>(in.ndc, 0.0, 1.0);
  let origin = nearH.xyz / nearH.w;
  let dir = normalize(U0.lightDir.xyz);
  var travel = 0.0;
  var hit = false;
  var step = 0u;
  let span = U0.params.z;
  loop {
    if (step >= 96u || travel > span) { break; }
    let d = mapScene(origin + dir * travel);
    if (d < 0.004) { hit = true; break; }
    travel = travel + max(d * 0.8, 0.005);
    step = step + 1u;
  }
  if (!hit) { discard; }
  let world = origin + dir * travel;
  let clip = U0.lightViewProj * vec4<f32>(world, 1.0);
  return clamp(clip.z / clip.w, 0.0, 1.0);
}
