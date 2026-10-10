// The fixture's kernel: results[index] = index * 3 + 1 for every element in
// range, nothing for the last workgroup's extra invocations
@group(0) @binding(1) var<storage, read_write> results: array<u32>;

@compute @workgroup_size(computeWorkgroupSize)
fn main(@builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!computeElementInRange(globalId)) { return; }
  let index = computeElementIndex(globalId);
  results[index] = index * 3u + 1u;
}
