// One half of the separable depth-aware SSAO blur at half resolution
// (render-perf-6). The direction comes from the file composed before this
// one (deferred_ao_blur_x.wgsl / deferred_ao_blur_y.wgsl): run along x, then
// along y. Five taps cover the 4x4 interleaved pattern of the SSAO. A tap on
// another surface (its depth differs by more than a few percent) counts
// little, so the AO does not bleed across silhouettes; sky taps are skipped.
@group(0) @binding(0) var aoTex: texture_2d<f32>;
@group(0) @binding(1) var aoDepthTex: texture_2d<f32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
// How fast a tap's weight falls with its relative depth difference.
const DEPTH_SHARPNESS: f32 = 40.0;
@fragment
fn fs(@builtin(position) fc: vec4<f32>) -> @location(0) vec4<f32> {
  let dim = vec2<i32>(textureDimensions(aoTex));
  let px = vec2<i32>(fc.xy);
  let center = textureLoad(aoDepthTex, px, 0).x;
  if (center <= 0.0) { return vec4<f32>(1.0, 1.0, 1.0, 1.0); }
  var weights = array<f32, 5>(0.12, 0.22, 0.32, 0.22, 0.12);
  var sum = 0.0;
  var total = 0.0;
  for (var k = -2; k <= 2; k = k + 1) {
    let q = clamp(px + blurDirection * k, vec2<i32>(0), dim - vec2<i32>(1));
    let d = textureLoad(aoDepthTex, q, 0).x;
    if (d <= 0.0) { continue; }
    let w = weights[k + 2] / (1.0 + abs(d - center) / center * DEPTH_SHARPNESS);
    sum = sum + textureLoad(aoTex, q, 0).r * w;
    total = total + w;
  }
  let ao = sum / max(total, 1e-6);
  return vec4<f32>(ao, ao, ao, 1.0);
}
