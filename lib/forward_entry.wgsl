// The forward main pass's entry for a G-buffer material (forward-rae-4):
// the material's gbufferSurface(in) computes what it would write into the
// G-buffer, and this decodes it the way the deferred lighting pass does
// (deferred_light.wgsl) and shades it with shadeSurface, in the forward
// pass. Composed AFTER the material (its VsOut, FsOut and gbufferSurface)
// and after the shading prelude (precision, sky_hosek.wgsl,
// deferred_uniforms.wgsl, deferred_shade.wgsl, ssao_upsample.wgsl). The lighting
// bindings sit at 12..17, above every material's own. The world position is rebuilt from
// the fragment's position with the scene size the light uniform carries in
// sunDir.w / ambGround.w (uploadLightUniform). The SSAO is read only with forwardSsao (else 1).
@group(0) @binding(12) var<uniform> L: LightU;
@group(0) @binding(13) var<uniform> SH: ShadowU;
@group(0) @binding(14) var shadowTex: texture_depth_2d_array;
@group(0) @binding(15) var shadowSamp: sampler_comparison;
// SSAO (forward-rae-6): the AO of the 1x depth + normal pre-pass, when the
// renderer has it (forwardSsao 1, set at pipeline creation; 0 compiles the
// read out). Composed after ssao_upsample.wgsl.
@group(0) @binding(16) var aoTex: texture_2d<f32>;
@group(0) @binding(17) var aoDepthTex: texture_2d<f32>;
override forwardSsao: f32 = 0.0;
// gbuffer_octahedral.wgsl's octDecode under its own name: not every material
// composes that file, and some define their own octEncode
fn forwardOctDecode(e: vec2<f32>) -> vec3<f32> {
  let f = e * 2.0 - vec2<f32>(1.0);
  var n = vec3<f32>(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  let t = max(-n.z, 0.0);
  n = vec3<f32>(n.x + select(t, -t, n.x >= 0.0),
                n.y + select(t, -t, n.y >= 0.0), n.z);
  return normalize(n);
}
@fragment
fn fsForward(in: VsOut) -> @location(0) vec4<f32> {
  let g = gbufferSurface(in);
  let mode = g.gba.w;
  if (mode > 0.5 && mode < 0.83) {
    return vec4<f32>(g.gbb.rgb, 1.0);
  }
  let size = vec2<f32>(L.sunDir.w, L.ambGround.w);
  let uv = in.pos.xy / size;
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, in.pos.z, 1.0);
  let world = L.invViewProj * ndc;
  var surface: ShadedSurface;
  surface.wpos = world.xyz / world.w;
  surface.N = forwardOctDecode(g.gba.xy);
  surface.albedo = g.gbb.rgb;
  surface.metallic = g.gbc.z;
  surface.rough = g.gbb.a;
  surface.mode = mode;
  surface.ao = g.gbc.w;
  surface.emissive = 0.0;
  if (mode > 0.16 && mode < 0.5) {
    surface.emissive = exp(g.gbc.w * 6.91) - 1.0;
    surface.ao = 1.0;
  }
  surface.ssao = 1.0;
  if (forwardSsao > 0.5) {
    surface.ssao = ssaoAt(vec2<i32>(in.pos.xy), in.pos.z);
  }
  return vec4<f32>(shadeSurface(surface), 1.0);
}
