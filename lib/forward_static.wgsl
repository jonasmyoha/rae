// The forward main pass's static material (forward-rae-1): the static mesh's
// vertex stage and surface (gbuffer_static_surface.wgsl) shaded in place with
// shadeSurface (deferred_shade.wgsl), the same function the deferred lighting
// pass runs, so the two paths shade the same. Drawn into a 4x MSAA target with
// no depth pre-pass of its own; with SSAO (forward-rae-6) a 1x pre-pass feeds
// the AO, read here, else `ssao` is 1.
@group(0) @binding(4) var<uniform> L: LightU;
@group(0) @binding(5) var<uniform> SH: ShadowU;
@group(0) @binding(6) var shadowTex: texture_depth_2d_array;
@group(0) @binding(7) var shadowSamp: sampler_comparison;
// SSAO (forward-rae-6): the AO of the 1x depth + normal pre-pass, when the
// renderer has it (forwardSsao 1, set at pipeline creation; 0 compiles the
// read out). Composed after ssao_upsample.wgsl.
@group(0) @binding(8) var aoTex: texture_2d<f32>;
@group(0) @binding(9) var aoDepthTex: texture_2d<f32>;
override forwardSsao: f32 = 0.0;
@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
  let d = draws[in.inst];
  let material = staticSurface(in, d);
  // A level of detail fading in or out (fade_dither.wgsl)
  if (!fadeKeeps(d.fade, in.pos.xy, fadeFrameOffset(F.jitter.xy))) {
    discard;
  }
  var surface: ShadedSurface;
  surface.wpos = in.wpos;
  surface.N = material.nrm;
  surface.albedo = material.albedo;
  surface.metallic = d.albedoMetallic.a;
  surface.rough = abs(d.params.x);
  surface.mode = d.params.z;
  surface.ao = d.params.y;
  surface.emissive = 0.0;
  if (d.params.z > 0.16 && d.params.z < 0.5) {
    surface.emissive = exp(d.params.y * 6.91) - 1.0;
    surface.ao = 1.0;
  }
  surface.ssao = 1.0;
  if (forwardSsao > 0.5) {
    surface.ssao = ssaoAt(vec2<i32>(in.pos.xy), in.pos.z);
  }
  return vec4<f32>(shadeSurface(surface), 1.0);
}
