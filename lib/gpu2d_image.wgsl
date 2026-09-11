// gpu2d_image.wgsl — the 2D image draw (one textured, rounded, tinted quad per
// draw), a Rae asset since #908 (was the C constant G2D_IMG_WGSL). Output is
// premultiplied; uImg = rect, tint (straight, premultiplied here), params
// (radius), uv (origin + size).
@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;
@group(0) @binding(1) var<uniform> uImg: array<vec4<f32>, 4>;  // rect, tint, params(radius), uv
@group(0) @binding(2) var tex: texture_2d<f32>;
@group(0) @binding(3) var samp: sampler;
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) local: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VsOut {
  var corners = array<vec2<f32>, 6>(
    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),
    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));
  let c = corners[vi];
  let rect = uImg[0];
  let phys = uXform[0].xy;
  let posPx = (rect.xy + c * rect.zw) * uXform[0].zw + uXform[1].xy;
  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0, 1.0 - posPx.y / phys.y * 2.0);
  var o: VsOut;
  o.pos = vec4<f32>(ndc, 0.0, 1.0);
  let uv = uImg[3];
  o.uv = uv.xy + c * uv.zw;
  o.local = c * rect.zw;
  return o;
}
fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {
  let q = abs(p) - b + vec2<f32>(r, r);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;
}
@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
  let texel = textureSample(tex, samp, in.uv);
  let tint = uImg[1];
  let half = uImg[0].zw * 0.5;
  let rad = uImg[2].x;
  let d = sdRoundBox(in.local - half, half, rad);
  let aa = max(fwidth(d), 0.0001);
  let cov = 1.0 - smoothstep(-aa, aa, d);
  let a = texel.a * tint.a * cov;
  return vec4<f32>(texel.rgb * tint.rgb * a, a);  // premultiplied
}
