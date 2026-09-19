@group(0) @binding(0) var litTex: texture_2d<f32>;
@group(0) @binding(1) var histTex: texture_2d<f32>;
@group(0) @binding(2) var gbcTex: texture_2d<f32>;
@group(0) @binding(3) var<uniform> P: vec4<f32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) fc: vec4<f32>) -> @location(0) vec4<f32> {
  let dim = vec2<i32>(textureDimensions(litTex));
  let px = vec2<i32>(fc.xy);
  let cur = textureLoad(litTex, px, 0).rgb;
  if (P.x < 0.5) { return vec4<f32>(cur, 1.0); }
  let mRaw = textureLoad(gbcTex, px, 0).xy;
  let motion = mRaw * 2.0 - vec2<f32>(256.0 / 255.0);
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(dim);
  let prevUv = uv - motion;
  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {
    return vec4<f32>(cur, 1.0);
  }
  let hp = vec2<i32>(prevUv * vec2<f32>(dim));
  var hist = textureLoad(histTex, hp, 0).rgb;
  var lo = cur;
  var hi = cur;
  for (var dy = -1; dy <= 1; dy = dy + 1) {
    for (var dx = -1; dx <= 1; dx = dx + 1) {
      let q = clamp(px + vec2<i32>(dx, dy), vec2<i32>(0), dim - vec2<i32>(1));
      let c = textureLoad(litTex, q, 0).rgb;
      lo = min(lo, c);
      hi = max(hi, c);
    }
  }
  hist = clamp(hist, lo, hi);
  return vec4<f32>(mix(cur, hist, 0.9), 1.0);
}
