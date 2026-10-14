// One bloom level up (render-perf-8): this level's downsample plus the level
// below upsampled with a 3x3 tent (1 2 1 / 2 4 2 / 1 2 1, over 16). Each
// level adds its own blur radius, so the sum spreads from a tight glow to a
// wide one.
@group(0) @binding(0) var levelTex: texture_2d<f32>;
@group(0) @binding(1) var belowTex: texture_2d<f32>;
@group(0) @binding(2) var bloomSamp: sampler;
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let levelDims = vec2<f32>(textureDimensions(levelTex));
  let uv = pos.xy / levelDims;
  let texel = vec2<f32>(1.0) / vec2<f32>(textureDimensions(belowTex));
  var sum = vec3<f32>(0.0);
  for (var y = -1; y <= 1; y = y + 1) {
    for (var x = -1; x <= 1; x = x + 1) {
      let w = f32((2 - abs(x)) * (2 - abs(y)));
      sum = sum + textureSampleLevel(belowTex, bloomSamp, uv + texel * vec2<f32>(f32(x), f32(y)), 0.0).rgb * w;
    }
  }
  let level = textureLoad(levelTex, vec2<i32>(pos.xy), 0).rgb;
  return vec4<f32>(level + sum / 16.0, 1.0);
}
