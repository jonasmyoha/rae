// The gust field on the GPU at 64 points in each of four winds (a calm, a
// breeze, a gale, and a gale whose drift is near the wrap); every input is
// exactly representable, so the Rae side builds the same ones
// (checks/windGust/Main.rae).
@group(0) @binding(0) var<storage, read_write> results: array<f32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let i = id.x;
  let point = i % 64u;
  let kind = i / 64u;
  let xy = vec2<f32>(f32(point % 8u) * 29.75 - 120.0, f32(point / 8u) * 31.5 - 100.0);
  var wind = vec4<f32>(0.0, 0.0, 0.6, 400.0);
  var drift = vec2<f32>(1234.5, -567.25);
  if (kind == 1u) { wind = vec4<f32>(0.75, 0.5, 0.6, 400.0); }
  if (kind == 2u) { wind = vec4<f32>(2.5, 1.75, 0.8, 1812.5); }
  if (kind == 3u) {
    wind = vec4<f32>(-2.25, 1.5, 0.8, 3599.5);
    drift = vec2<f32>(24575.5, 3.25);
  }
  results[i] = windGustAt(xy, wind, drift);
}
