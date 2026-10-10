// The same kernel WITHOUT the guard: the last workgroup's extra invocations
// write past the element count (the fixture shows they do)
@group(0) @binding(1) var<storage, read_write> results: array<u32>;

@compute @workgroup_size(computeWorkgroupSize)
fn main(@builtin(global_invocation_id) globalId: vec3<u32>) {
  _ = computeBounds.count;  // read, so the layout keeps binding 0
  let index = computeElementIndex(globalId);
  results[index] = index * 3u + 1u;
}
