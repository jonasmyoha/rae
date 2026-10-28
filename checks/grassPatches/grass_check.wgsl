@group(0) @binding(0) var<storage, read_write> results: array<u32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let cell = vec2<i32>(i32(id.x) - 32, i32(id.x % 9u) - 4);
  let spacing = 40.0 / 256.0;
  let point = grassCandidatePosition(cell, spacing, 1.0);
  // The same cell addressed from two different streaming grid centers.
  let center = vec2<i32>(37, -19);
  let streamed = grassCandidatePosition(center + (cell - center), spacing, 1.0);
  let time = f32(id.x) * 0.39;
  let wind = vec3<f32>(0.8, 0.6, 0.7);
  let tip = grassWindOffset(point, 1.0, time, wind);
  let calm = grassWindOffset(point, 1.0, time, vec3<f32>(0,0,1));
  let root = grassWindOffset(point, 0.0, time, wind);
  let adjacent = grassWindOffset(point + vec2<f32>(0.02,0), 1.0, time, wind);
  let next = grassWindOffset(point, 1.0, time + 1.0/60.0, wind);
  let color = grassGroundColor(vec3<f32>(0.08,0.245,0.015), point, 7.5, 0.58);
  let uniform = vec2<f32>(cell) * spacing
    + (vec2<f32>(hash2u(cell, 7u), hash2u(cell, 13u)) - vec2<f32>(0.5)) * spacing * 0.9;
  let safe = all(grassCandidatePosition(cell, spacing, 0.0) == uniform) && all(point == streamed) && all(root == vec2<f32>(0)) && all(calm == vec2<f32>(0))
    && dot(tip, wind.xy) > 0.0 && length(tip) < 0.30
    && distance(tip, adjacent) < 0.01 && distance(tip, next) < 0.01
    && all(color > vec3<f32>(0.0)) && all(color < vec3<f32>(1.0));
  results[id.x] = select(0u,1u,safe);
  results[64u + id.x] = select(0u,1u,length(tip) > 0.035);
}
