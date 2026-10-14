// The deferred TAA resolve (render-perf-10). Each pixel blends this frame's
// lit colour with the history reprojected along its motion vector:
// - in YCoCg on Reinhard-compressed colour, so a few very bright pixels do
//   not dominate the blend or the clip box;
// - the history is clipped toward the 3x3 neighbourhood's mean, inside a
//   box of one standard deviation (variance clipping), so a value the
//   neighbourhood no longer contains (a trail behind a moving object) is
//   pulled back instead of kept;
// - the history is fetched with a 5-tap Catmull-Rom filter, sharp under
//   motion where a nearest fetch jittered and a bilinear one blurred;
// - this frame weighs 0.05 when still, rising to 0.2 with the motion
//   length (8 pixels or more);
// - a REACTIVE pixel weighs 0.6: geometry whose motion vectors miss part
//   of its movement (wind-blown grass, the deforming metaballs) marks
//   itself in the G-buffer (gba.z = 1), and a pixel a transparent surface
//   drew over this frame (water) is found by comparing the lit image with
//   its opaque snapshot (litCopy) when one was taken.
@group(0) @binding(0) var litTex: texture_2d<f32>;
@group(0) @binding(1) var histTex: texture_2d<f32>;
@group(0) @binding(2) var gbcTex: texture_2d<f32>;
@group(0) @binding(3) var<uniform> P: vec4<f32>;  // x: history exists, y: litCopy is this frame's
@group(0) @binding(4) var gbaTex: texture_2d<f32>;
@group(0) @binding(5) var litCopyTex: texture_2d<f32>;
@group(0) @binding(6) var histSamp: sampler;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
const STILL_WEIGHT: f32 = 0.05;
const MOVING_WEIGHT: f32 = 0.2;
const MOVING_PIXELS: f32 = 8.0;
const REACTIVE_WEIGHT: f32 = 0.6;
const CLIP_SIGMA: f32 = 1.0;

fn maxChannel(c: vec3<f32>) -> f32 { return max(c.r, max(c.g, c.b)); }
// Reinhard on the largest channel (keeps the hue), and its inverse
fn compress(c: vec3<f32>) -> vec3<f32> { return c / (1.0 + maxChannel(c)); }
fn expand(c: vec3<f32>) -> vec3<f32> { return c / max(1.0 - maxChannel(c), 1e-4); }
fn toYCoCg(c: vec3<f32>) -> vec3<f32> {
  return vec3<f32>(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                   0.5 * c.r - 0.5 * c.b,
                   -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
fn fromYCoCg(c: vec3<f32>) -> vec3<f32> {
  return vec3<f32>(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}
fn litAt(px: vec2<i32>, dim: vec2<i32>) -> vec3<f32> {
  return textureLoad(litTex, clamp(px, vec2<i32>(0), dim - vec2<i32>(1)), 0).rgb;
}
// The history at uv, 5-tap Catmull-Rom (the bilinear taps cover the 4x4
// footprint's corners' small weights by leaving them out)
fn historyAt(uv: vec2<f32>, size: vec2<f32>) -> vec3<f32> {
  let samplePos = uv * size;
  let center = floor(samplePos - 0.5) + 0.5;
  let f = samplePos - center;
  let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  let w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  let w3 = f * f * (-0.5 + 0.5 * f);
  let w12 = w1 + w2;
  let offset12 = w2 / w12;
  let p0 = (center - 1.0) / size;
  let p3 = (center + 2.0) / size;
  let p12 = (center + offset12) / size;
  var sum = textureSampleLevel(histTex, histSamp, vec2<f32>(p12.x, p0.y), 0.0).rgb * (w12.x * w0.y);
  sum = sum + textureSampleLevel(histTex, histSamp, vec2<f32>(p0.x, p12.y), 0.0).rgb * (w0.x * w12.y);
  sum = sum + textureSampleLevel(histTex, histSamp, p12, 0.0).rgb * (w12.x * w12.y);
  sum = sum + textureSampleLevel(histTex, histSamp, vec2<f32>(p3.x, p12.y), 0.0).rgb * (w3.x * w12.y);
  sum = sum + textureSampleLevel(histTex, histSamp, vec2<f32>(p12.x, p3.y), 0.0).rgb * (w12.x * w3.y);
  let total = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
  return max(sum / total, vec3<f32>(0.0));
}
// Move `history` toward `center` until it is inside the box center +- extent
fn clipToBox(history: vec3<f32>, center: vec3<f32>, extent: vec3<f32>) -> vec3<f32> {
  let offset = history - center;
  let units = abs(offset / max(extent, vec3<f32>(1e-5)));
  let largest = max(units.x, max(units.y, units.z));
  if (largest > 1.0) { return center + offset / largest; }
  return history;
}
@fragment
fn fs(@builtin(position) fc: vec4<f32>) -> @location(0) vec4<f32> {
  let dim = vec2<i32>(textureDimensions(litTex));
  let px = vec2<i32>(fc.xy);
  let cur = textureLoad(litTex, px, 0).rgb;
  if (P.x < 0.5) { return vec4<f32>(cur, 1.0); }
  let motion = textureLoad(gbcTex, px, 0).xy * 2.0 - vec2<f32>(256.0 / 255.0);
  let size = vec2<f32>(dim);
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / size;
  let prevUv = uv - motion;
  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {
    return vec4<f32>(cur, 1.0);
  }
  // The neighbourhood's mean and spread, in compressed YCoCg
  var m1 = vec3<f32>(0.0);
  var m2 = vec3<f32>(0.0);
  for (var dy = -1; dy <= 1; dy = dy + 1) {
    for (var dx = -1; dx <= 1; dx = dx + 1) {
      let c = toYCoCg(compress(litAt(px + vec2<i32>(dx, dy), dim)));
      m1 = m1 + c;
      m2 = m2 + c * c;
    }
  }
  let mean = m1 / 9.0;
  let sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3<f32>(0.0)));
  let history = clipToBox(toYCoCg(compress(historyAt(prevUv, size))), mean, sigma * CLIP_SIGMA);
  var weight = mix(STILL_WEIGHT, MOVING_WEIGHT, clamp(length(motion * size) / MOVING_PIXELS, 0.0, 1.0));
  var reactive = textureLoad(gbaTex, px, 0).z > 0.75;
  if (P.y > 0.5) {
    let opaque = textureLoad(litCopyTex, px, 0).rgb;
    if (any(abs(cur - opaque) > vec3<f32>(1e-4) * (vec3<f32>(1.0) + opaque))) { reactive = true; }
  }
  if (reactive) { weight = max(weight, REACTIVE_WEIGHT); }
  let blended = mix(history, toYCoCg(compress(cur)), weight);
  return vec4<f32>(max(expand(fromYCoCg(blended)), vec3<f32>(0.0)), 1.0);
}
