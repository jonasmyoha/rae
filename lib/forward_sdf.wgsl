// Metaball clusters in the forward main pass (forward-rae-2): the raymarch
// (sdf_surface.wgsl) shaded in place with shadeSurface (deferred_shade.wgsl),
// as the deferred lighting shades what gbuffer_sdf.wgsl writes. The fragment
// runs once per pixel, so in the 4x MSAA pass the metaballs' silhouettes are
// not antialiased (the triangles' edges are).
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
struct ForwardOut {
  @location(0) colour: vec4<f32>,
  @builtin(frag_depth) depth: f32,
};
@fragment
fn fs(in: VsOut) -> ForwardOut {
  let march = marchMetaballs(in.ndc);
  if (!march.hit) { discard; }
  var surface: ShadedSurface;
  surface.wpos = march.worldPos;
  surface.N = sceneNormal(march.worldPos);
  surface.albedo = sceneAlbedo(march.worldPos);
  surface.metallic = P.baseColorMetallic.a;
  surface.rough = P.emissiveRoughness.a;
  surface.ao = 1.0;
  surface.emissive = 0.0;
  surface.mode = 0.0;
  let emitPeak = sdfEmitPeak();
  if (emitPeak > 0.0) {
    surface.emissive = emitPeak;
    surface.mode = 0.33333333;
  }
  let clip = F.viewProj * vec4<f32>(march.worldPos, 1.0);
  surface.ssao = 1.0;
  if (forwardSsao > 0.5) {
    surface.ssao = ssaoAt(vec2<i32>(in.pos.xy), clamp(clip.z / clip.w, 0.0, 1.0));
  }
  var o: ForwardOut;
  o.colour = vec4<f32>(shadeSurface(surface), 1.0);
  o.depth = clamp(clip.z / clip.w, 0.0, 1.0);
  return o;
}
