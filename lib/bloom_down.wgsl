// One bloom level down (render-perf-8): the 13 taps over the level above,
// to half its size.
@group(0) @binding(0) var aboveTex: texture_2d<f32>;
@group(0) @binding(1) var bloomSamp: sampler;
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let aboveDims = vec2<f32>(textureDimensions(aboveTex));
  let uv = pos.xy * 2.0 / aboveDims;
  return vec4<f32>(downsample13(aboveTex, bloomSamp, uv, vec2<f32>(1.0) / aboveDims, false), 1.0);
}
