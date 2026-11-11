// gpu2d_backdrop.wgsl — the 2D canvas's backdrop blur (lib/Gpu2dCanvasBackdrop.rae): what the frame
// has drawn so far, shrunk to a quarter or an eighth of its size and blurred, for BackdropBlur nodes.
// One full-screen triangle per pass. u = (source texel width, source texel height, step x, step y),
// the step in source texels.
//   down:  four bilinear taps, one per 2x2 corner, average the 4x4 source texels under an output
//          texel (the copy a quarter of the frame's size).
//   down8: sixteen bilinear taps average the 8x8 under it (the copy an eighth of the size).
//   blur:  a 25-tap Gaussian along the step, sigma 4 steps, so it reaches 3 sigma; the step is a
//          quarter of the blur's sigma, so the taps are never more than about a texel apart and one
//          pass across and one down make the whole blur.
@group(0) @binding(0) var<uniform> u: vec4<f32>;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VsOut {
  let p = vec2<f32>(f32((vi << 1u) & 2u), f32(vi & 2u));
  var o: VsOut;
  o.pos = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
  o.uv = vec2<f32>(p.x, 1.0 - p.y);
  return o;
}
@fragment
fn down(in: VsOut) -> @location(0) vec4<f32> {
  let t = u.xy;
  let a = textureSample(src, samp, in.uv + vec2<f32>(-t.x, -t.y));
  let b = textureSample(src, samp, in.uv + vec2<f32>(t.x, -t.y));
  let c = textureSample(src, samp, in.uv + vec2<f32>(-t.x, t.y));
  let d = textureSample(src, samp, in.uv + vec2<f32>(t.x, t.y));
  return (a + b + c + d) * 0.25;
}
@fragment
fn down8(in: VsOut) -> @location(0) vec4<f32> {
  let t = u.xy;
  var sum = vec4<f32>(0.0);
  for (var i = 0; i < 4; i = i + 1) {
    for (var j = 0; j < 4; j = j + 1) {
      let offset = vec2<f32>(f32(i) * 2.0 - 3.0, f32(j) * 2.0 - 3.0);
      sum = sum + textureSample(src, samp, in.uv + offset * t);
    }
  }
  return sum / 16.0;
}
@fragment
fn blur(in: VsOut) -> @location(0) vec4<f32> {
  let step = u.zw * u.xy;
  var sum = vec4<f32>(0.0);
  var total = 0.0;
  for (var k = -12; k <= 12; k = k + 1) {
    let x = f32(k);
    let w = exp(-x * x / 32.0);
    sum = sum + textureSample(src, samp, in.uv + step * x) * w;
    total = total + w;
  }
  return sum / total;
}
