// The frame's mean log brightness for the automatic exposure (AutoExposure,
// docs/physical-light-units.md step 2). One workgroup of 16 x 16 threads
// reads a 64 x 64 grid over the lit HDR image (each thread a 4 x 4 block of
// cells), takes log2 of each sample's luminance and sums the 4,096 values in
// workgroup memory; result[0] is their mean. The lit image is already
// exposed (LightUnits), so the caller divides by the exposure it was drawn
// with to get the scene's luminance.
@group(0) @binding(0) var lit: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> result: array<f32, 4>;

var<workgroup> partial: array<f32, 256>;

@compute @workgroup_size(16, 16)
fn measure(
  @builtin(local_invocation_index) index: u32,
  @builtin(local_invocation_id) local: vec3<u32>
) {
  let size = vec2<f32>(textureDimensions(lit));
  var sum = 0.0;
  for (var j = 0u; j < 4u; j = j + 1u) {
    for (var i = 0u; i < 4u; i = i + 1u) {
      let cell = (vec2<f32>(f32(local.x * 4u + i), f32(local.y * 4u + j)) + 0.5) / 64.0;
      let texel = vec2<i32>(cell * size);
      let colour = textureLoad(lit, texel, 0).rgb;
      // a floor so black pixels (and NaN-free empty targets) stay finite
      let luminance = max(dot(colour, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0001);
      sum = sum + log2(luminance);
    }
  }
  partial[index] = sum;
  workgroupBarrier();
  var stride = 128u;
  loop {
    if (stride == 0u) { break; }
    if (index < stride) {
      partial[index] = partial[index] + partial[index + stride];
    }
    workgroupBarrier();
    stride = stride / 2u;
  }
  if (index == 0u) {
    result[0] = partial[0] / 4096.0;
    result[1] = 1.0;
  }
}
