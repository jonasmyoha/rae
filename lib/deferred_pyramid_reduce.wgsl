@group(0) @binding(0) var srcTex: texture_2d<f32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) f32 {
  let px = vec2<i32>(pos.xy) * 2;
  let dim = vec2<i32>(textureDimensions(srcTex, 0)) - vec2<i32>(1);
  let a = textureLoad(srcTex, min(px,                  dim), 0).r;
  let b = textureLoad(srcTex, min(px + vec2<i32>(1,0), dim), 0).r;
  let c = textureLoad(srcTex, min(px + vec2<i32>(0,1), dim), 0).r;
  let d = textureLoad(srcTex, min(px + vec2<i32>(1,1), dim), 0).r;
  return max(max(a, b), max(c, d));
}
