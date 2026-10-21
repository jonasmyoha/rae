// A static shadow caster: depth only. Each caster is its model and its level
// cross-fade (ShadowMaps' caster floats; fade_dither.wgsl). The plain pipeline
// has no fragment stage; casters that are fading are drawn with `vsFade` and
// `fsFade`, which discard by the same dither as the main pass, so a prop
// crossing between levels casts one shadow, not two.
struct SU { lightViewProj: mat4x4<f32> };
struct Caster { model: mat4x4<f32>, fade: vec4<f32> };
@group(0) @binding(0) var<uniform> S: SU;
@group(0) @binding(1) var<storage, read> models: array<Caster>;
@vertex
fn vs(@builtin(instance_index) ii: u32, @location(0) p: vec3<f32>) -> @builtin(position) vec4<f32> {
  return S.lightViewProj * (models[ii].model * vec4<f32>(p, 1.0));
}

struct FadeOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(flat) fade: vec4<f32>,
};
@vertex
fn vsFade(@builtin(instance_index) ii: u32, @location(0) p: vec3<f32>) -> FadeOut {
  var o: FadeOut;
  o.pos = S.lightViewProj * (models[ii].model * vec4<f32>(p, 1.0));
  o.fade = models[ii].fade;
  return o;
}
@fragment
fn fsFade(in: FadeOut) {
  if (!fadeKeeps(in.fade, in.pos.xy, vec2<f32>(0.0))) {
    discard;
  }
}
