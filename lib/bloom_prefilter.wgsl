// The first bloom level (render-perf-8): the composite's source image (the
// lit / anti-aliased / TAA result, scene size) downsampled to half size with
// the 13 taps and the Karis average, then the soft-knee threshold: below
// threshold - knee nothing passes, above threshold + knee everything above
// the threshold passes, and a quadratic curve joins the two, so a surface
// crossing the threshold fades in instead of popping. The threshold is
// compared with the EXPOSED brightness (what the composite tone-maps); the
// output stays unexposed, like the lit image the composite adds it to.
struct PrefilterU {
  p0: vec4<f32>,  // x: threshold, y: knee, z: exposure
};
@group(0) @binding(0) var<uniform> U: PrefilterU;
@group(0) @binding(1) var sourceTex: texture_2d<f32>;
@group(0) @binding(2) var bloomSamp: sampler;
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let sourceDims = vec2<f32>(textureDimensions(sourceTex));
  // This target is half the source, rounded up: map its pixel to the source.
  let uv = pos.xy * 2.0 / sourceDims;
  let color = downsample13(sourceTex, bloomSamp, uv, vec2<f32>(1.0) / sourceDims, true);
  let brightness = max(color.r, max(color.g, color.b)) * U.p0.z;
  let knee = max(U.p0.y, 1e-4);
  var soft = clamp(brightness - U.p0.x + knee, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee);
  let weight = max(soft, brightness - U.p0.x) / max(brightness, 1e-4);
  return vec4<f32>(color * weight, 1.0);
}
