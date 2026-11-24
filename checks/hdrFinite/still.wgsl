// checks/hdrFinite's G-buffer: no motion (as the G-buffer stores it, 128 / 255), nothing reactive
@group(0) @binding(0) var<uniform> U: vec4<u32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs() -> @location(0) vec4<f32> {
  return vec4<f32>(128.0 / 255.0, 128.0 / 255.0, f32(min(U.x, 0u)), 0.0);
}
