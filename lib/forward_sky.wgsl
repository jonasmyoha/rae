// The forward main pass's background (forward-rae-1): a fullscreen triangle at
// the far plane (reverse-Z depth 0) drawn with depth Equal after the opaque
// draws, so only the samples nothing covered evaluate the sky — the same
// background the deferred lighting pass writes where the depth is empty.
const PI = 3.14159265;  // sky_hosek.wgsl reads it (deferred_shade.wgsl defines it elsewhere)
@group(0) @binding(0) var<uniform> L: LightU;
struct SkyOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) ndc: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> SkyOut {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  var o: SkyOut;
  o.pos = vec4<f32>(points[vi], 0.0, 1.0);
  o.ndc = points[vi];
  return o;
}
@fragment
fn fs(in: SkyOut) -> @location(0) vec4<f32> {
  if (L.clearColor.w > 0.5) { return vec4<f32>(L.clearColor.rgb, 1.0); }
  let farW = L.invViewProj * vec4<f32>(in.ndc, 0.0, 1.0);
  return vec4<f32>(skyColor(normalize(farW.xyz / farW.w - L.camPos.xyz)), 1.0);
}
