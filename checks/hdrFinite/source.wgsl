// checks/hdrFinite's picture: a smooth mid gray, and one pixel (U.p0.xy) of the bits U.p0.z
struct SourceU { p0: vec4<u32>, };
@group(0) @binding(0) var<uniform> U: SourceU;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  if (all(vec2<u32>(pos.xy) == U.p0.xy)) { return vec4<f32>(vec3<f32>(bitcast<f32>(U.p0.z)), 1.0); }
  return vec4<f32>(0.5 + 0.001 * pos.x, 0.6, 0.7, 1.0);
}
