// Nearby exclusion circles supplied by an app: count, then x/y/radius/pad.
// The positions are world-space, so rebuilding the bounded nearby list as a
// camera crosses terrain cells never makes an existing edge follow the camera.
@group(0) @binding(4) var<storage, read> grassObstacles: array<f32>;

fn raeGrassObstacle(p: vec2<f32>) -> f32 {
  let count = min(u32(max(grassObstacles[0], 0.0)), 512u);
  for (var index = 0u; index < count; index = index + 1u) {
    let offset = 1u + index * 4u;
    let centre = vec2<f32>(grassObstacles[offset], grassObstacles[offset + 1u]);
    let radius = grassObstacles[offset + 2u];
    let delta = p - centre;
    if (dot(delta, delta) < radius * radius) { return 1.0; }
  }
  return 0.0;
}
