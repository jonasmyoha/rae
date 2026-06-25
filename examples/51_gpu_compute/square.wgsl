// Minimal generic compute kernel (not a raytracer): out[i] = in[i]*in[i] + 1.
// Demonstrates lib/gpu.rae dispatching an arbitrary WGSL kernel on the GPU.
@group(0) @binding(0) var<storage, read>       inp:  array<f32>;
@group(0) @binding(1) var<storage, read_write>  outp: array<f32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let i = gid.x;
  if (i >= arrayLength(&outp)) { return; }
  outp[i] = inp[i] * inp[i] + 1.0;
}
