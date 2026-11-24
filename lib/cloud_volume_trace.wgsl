// The volumetric cloud tier's trace (lib/CloudSlab, volumetric mode;
// docs/cloud-rendering-research.md §8.1 tier 3): as the slab's trace, one
// pixel of each 2x2 tile of the half-resolution grid a frame, but through a
// deep cloud volume shaped by 3D noise. The weather map is the field map
// (the layer's 2D field: coverage, how thick, the puff), the shape in height
// the profile strip; a low-frequency 3D noise rounds the towers into
// billows, a fine one erodes their edges where the cloud is thin. The march
// takes SlabU.pattern.w steps (48 at High, 64 at Ultra), cheap ones until it
// meets a cloud; at each sample 5 light samples in a cone toward the sun
// (the last one far, for the shadow of distant towers), two scattering
// octaves (Wrenninge: each less dense and less forward), the two-lobe phase,
// powder darkening and the sky's light growing with height. Out: the
// clouds' light (premultiplied) and how much of the sky they hide.
const PI = 3.14159265;  // sky_hosek.wgsl reads it
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var<uniform> S: SlabU;
@group(0) @binding(2) var fieldTex: texture_2d<f32>;
@group(0) @binding(3) var lutTex: texture_2d<f32>;
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  skySlabActive = true;
  let halfPx = floor(pos.xy) * 2.0 + S.pattern.xy + 0.5;
  let dir = cloudSlabDir(halfPx, S.sizes.zw);
  let jitter = fract(sin(dot(pos.xy + S.pattern.z * 7.3, vec2<f32>(12.9898, 78.233))) * 43758.5453);
  return volumeMarch(dir, jitter, clamp(i32(S.pattern.w), 16, 128));
}
