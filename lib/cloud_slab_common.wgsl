// The 2.5D slab cloud tier (lib/CloudSlab, docs/cloud-rendering-research.md
// §8.1 tier 2): shared by its passes. A thin slab of cloud between the
// clouds' base (cloudShape.z) and a top the kind sets, whose footprint is the
// same 2D field the layer draws (sky_hosek.wgsl skyCloudSample) baked into a
// field map round the eye, and whose shape in height is a profile strip (the
// LUT: stratus, stratocumulus, cumulus). Composed after sky_hosek.wgsl and
// deferred_uniforms.wgsl; each pass declares its own bindings.
struct SlabU {
  // the view-projection the history was drawn with (the frame before)
  prevViewProj: mat4x4<f32>,
  // x, y: which pixel of each 2x2 tile is traced this frame; z: the frame
  pattern: vec4<f32>,
  // the trace's size (a quarter of the resolve's pixels) and the resolve's
  // (half the lit target's width and height)
  sizes: vec4<f32>,
};
// The field map: cloudFieldTexels square texels over cloudFieldSpan meters
// of the clouds' plane round the eye, snapped to a texel
const cloudFieldTexels: f32 = 512.0;
const cloudFieldSpan: f32 = 32000.0;
fn cloudFieldOrigin() -> vec2<f32> {
  let texel = cloudFieldSpan / cloudFieldTexels;
  return floor(L.camPos.xy / texel) * texel - vec2<f32>(0.5 * cloudFieldSpan);
}
// The slab's depth (m): a stratus sheet thin, cumulus a kilometer and more,
// a storm's towers taller
fn cloudSlabThickness() -> f32 {
  let kind = clamp(L.cloudShape.x, 0.0, 1.0);
  let dark = clamp(L.clouds.y, 0.0, 1.0);
  return mix(300.0, 1000.0, kind) * (1.0 + 1.2 * max(dark - 0.5, 0.0)) * max(L.cloudShape.w, 0.1);
}
// The view ray through a pixel of the resolve (half-resolution) grid
fn cloudSlabDir(halfPx: vec2<f32>, resolveSize: vec2<f32>) -> vec3<f32> {
  let uv = halfPx / resolveSize;
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  let far = L.invViewProj * ndc;
  return normalize(far.xyz / far.w - L.camPos.xyz);
}
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
