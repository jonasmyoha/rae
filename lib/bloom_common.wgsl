// Shared by the bloom passes (render-perf-8): the fullscreen triangle and
// the 13-tap downsample (Jimenez, "Next generation post processing in Call
// of Duty", 2014): five overlapping 2x2 boxes, the centre one weighted 0.5
// and the four corner ones 0.125 each, which keeps the result stable when
// the camera moves by less than a texel.
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
fn bloomLuma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }
// The five boxes of the 13 taps; `karis` weights each box by 1/(1 + luma)
// (the first downsample only), so one very bright pixel cannot flash a
// whole block on and off from frame to frame (fireflies).
fn downsample13(tex: texture_2d<f32>, samp: sampler, uv: vec2<f32>, texel: vec2<f32>, karis: bool) -> vec3<f32> {
  let a = textureSampleLevel(tex, samp, uv + texel * vec2<f32>(-2.0, -2.0), 0.0).rgb;
  let b = textureSampleLevel(tex, samp, uv + texel * vec2<f32>( 0.0, -2.0), 0.0).rgb;
  let c = textureSampleLevel(tex, samp, uv + texel * vec2<f32>( 2.0, -2.0), 0.0).rgb;
  let d = textureSampleLevel(tex, samp, uv + texel * vec2<f32>(-1.0, -1.0), 0.0).rgb;
  let e = textureSampleLevel(tex, samp, uv + texel * vec2<f32>( 1.0, -1.0), 0.0).rgb;
  let f = textureSampleLevel(tex, samp, uv + texel * vec2<f32>(-2.0,  0.0), 0.0).rgb;
  let g = textureSampleLevel(tex, samp, uv, 0.0).rgb;
  let h = textureSampleLevel(tex, samp, uv + texel * vec2<f32>( 2.0,  0.0), 0.0).rgb;
  let i = textureSampleLevel(tex, samp, uv + texel * vec2<f32>(-1.0,  1.0), 0.0).rgb;
  let j = textureSampleLevel(tex, samp, uv + texel * vec2<f32>( 1.0,  1.0), 0.0).rgb;
  let k = textureSampleLevel(tex, samp, uv + texel * vec2<f32>(-2.0,  2.0), 0.0).rgb;
  let l = textureSampleLevel(tex, samp, uv + texel * vec2<f32>( 0.0,  2.0), 0.0).rgb;
  let m = textureSampleLevel(tex, samp, uv + texel * vec2<f32>( 2.0,  2.0), 0.0).rgb;
  var boxes = array<vec3<f32>, 5>(
    (d + e + i + j) * 0.25,
    (a + b + f + g) * 0.25,
    (b + c + g + h) * 0.25,
    (f + g + k + l) * 0.25,
    (g + h + l + m) * 0.25);
  var weights = array<f32, 5>(0.5, 0.125, 0.125, 0.125, 0.125);
  var sum = vec3<f32>(0.0);
  var total = 0.0;
  for (var n = 0; n < 5; n = n + 1) {
    var w = weights[n];
    if (karis) { w = w / (1.0 + bloomLuma(boxes[n])); }
    sum = sum + boxes[n] * w;
    total = total + w;
  }
  return sum / total;
}
