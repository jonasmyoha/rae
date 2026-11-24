// checks/hdrFinite's judge, over a texture of any size: red where a value is infinite or no number
// (by its bits), green where one is below zero, blue where one is over 30000
@group(0) @binding(0) var tex: texture_2d<f32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = textureDimensions(tex);
  let at = vec2<u32>(pos.xy) * dims / 256u;
  let c = textureLoad(tex, vec2<i32>(at), 0).rgb;
  let bits = bitcast<vec3<u32>>(c);
  let bad = any((bits & vec3<u32>(0x7f800000u)) == vec3<u32>(0x7f800000u));
  let below = !bad && any(c < vec3<f32>(0.0));
  let over = !bad && any(c > vec3<f32>(30000.0));
  return vec4<f32>(select(0.0, 1.0, bad), select(0.0, 1.0, below), select(0.0, 1.0, over), 1.0);
}
