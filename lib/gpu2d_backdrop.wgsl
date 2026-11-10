// gpu2d_backdrop.wgsl — the 2D canvas's backdrop blur (lib/Gpu2dCanvasBackdrop.rae): what the frame
// has drawn so far, shrunk to a quarter of its size and blurred, for BackdropBlur nodes to sample.
// One full-screen triangle per pass. u = (source texel width, source texel height, step x, step y),
// the step in source texels.
//   down: four bilinear taps, one per 2x2 corner, average the 4x4 source texels under an output
//         texel (the source is the frame, four times the size).
//   blur: a 13-tap Gaussian along the step (sigma 2 steps, so it reaches 3 sigma); run across, then
//         down, as many times as the blur's size needs.
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
fn blur(in: VsOut) -> @location(0) vec4<f32> {
  let step = u.zw * u.xy;
  var sum = vec4<f32>(0.0);
  var total = 0.0;
  for (var k = -6; k <= 6; k = k + 1) {
    let x = f32(k);
    let w = exp(-x * x / 8.0);
    sum = sum + textureSample(src, samp, in.uv + step * x) * w;
    total = total + w;
  }
  return sum / total;
}
