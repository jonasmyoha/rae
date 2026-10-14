// The forward main pass's static material (forward-rae-1): the static mesh's
// vertex stage and surface (gbuffer_static_surface.wgsl) shaded in place with
// shadeSurface (deferred_shade.wgsl), the same function the deferred lighting
// pass runs, so the two paths shade the same. Drawn into a 4x MSAA target with
// no depth pre-pass. There is no SSAO in the forward pass (its depth never
// leaves the tile), so `ssao` is 1.
@group(0) @binding(2) var<uniform> L: LightU;
@group(0) @binding(3) var<uniform> SH: ShadowU;
@group(0) @binding(4) var shadowTex: texture_depth_2d_array;
@group(0) @binding(5) var shadowSamp: sampler_comparison;
@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
  let d = draws[in.inst];
  let material = staticSurface(in, d);
  var surface: ShadedSurface;
  surface.wpos = in.wpos;
  surface.N = material.nrm;
  surface.albedo = material.albedo;
  surface.metallic = d.albedoMetallic.a;
  surface.rough = d.params.x;
  surface.mode = d.params.z;
  surface.ao = d.params.y;
  surface.emissive = 0.0;
  if (d.params.z > 0.16 && d.params.z < 0.5) {
    surface.emissive = exp(d.params.y * 6.91) - 1.0;
    surface.ao = 1.0;
  }
  surface.ssao = 1.0;
  return vec4<f32>(shadeSurface(surface), 1.0);
}
