// The bounds of a one-dimensional compute dispatch (lib/gpu/GpuCompute.rae).
//
// ceil(count / workgroupSize) workgroups are dispatched, so the last one is
// usually partial: its invocations past `count` must do nothing. Compose this
// file FIRST, with shader(files: ["lib/gpu/computeBounds.wgsl", "<kernel>"]),
// and start every kernel with the guard:
//
//   @compute @workgroup_size(computeWorkgroupSize)
//   fn main(@builtin(global_invocation_id) globalId: vec3<u32>) {
//     if (!computeElementInRange(globalId)) { return; }
//     let index = computeElementIndex(globalId);
//     ...
//   }
//
// Binding 0 of group 0 is the bounds uniform, written by recordBoundedCompute;
// the kernel's own resources start at binding 1. The workgroup size is an
// override the pipeline sets (createBoundedComputePipeline), so the size the
// dispatch is computed with and the size the kernel runs with are one number.

struct ComputeBounds {
  count: u32,
  workgroupCount: u32,
  reserved0: u32,
  reserved1: u32,
};

@group(0) @binding(0) var<uniform> computeBounds: ComputeBounds;

override computeWorkgroupSize: u32 = 64u;

// The element this invocation computes
fn computeElementIndex(globalId: vec3<u32>) -> u32 {
  return globalId.x;
}

// Whether that element exists: false for the invocations of the last,
// partial workgroup that lie past `count`
fn computeElementInRange(globalId: vec3<u32>) -> bool {
  return globalId.x < computeBounds.count;
}
