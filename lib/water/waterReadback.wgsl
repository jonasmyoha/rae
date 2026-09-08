// Fixed-size gameplay snapshot. Preserve signed J in .w, not clamped foam.
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var repeatingSampler: sampler;
@group(0) @binding(2) var<storage, read_write> snapshot: array<vec4<f32>>;
@compute @workgroup_size(8, 8)
fn capture(@builtin(global_invocation_id) id: vec3<u32>) {
  if (id.x >= 32u || id.y >= 32u) { return; }
  let uv = (vec2<f32>(id.xy) + vec2<f32>(0.5)) / 32.0;
  snapshot[id.y * 32u + id.x] = textureSampleLevel(source, repeatingSampler, uv, 0.0);
}
