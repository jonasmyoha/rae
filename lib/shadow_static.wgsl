// A static shadow caster: depth only. Each caster is its model and its level
// cross-fade (ShadowMaps' caster floats; fade_dither.wgsl). The plain pipeline
// has no fragment stage; casters that are fading are drawn with `vsFade` and
// `fsFade`, which discard by the same dither as the main pass, so a prop
// crossing between levels casts one shadow, not two.
// A swaying caster (fade.w > 0) bends in the wind as in the main pass
// (lib/wind.wgsl), so its shadow sways with it.
struct SU { lightViewProj: mat4x4<f32>, wind: vec4<f32>, cardRight: vec4<f32>, cardUp: vec4<f32> };
struct Caster { model: mat4x4<f32>, fade: vec4<f32>, surface: vec4<f32> };
@group(0) @binding(0) var<uniform> S: SU;
@group(0) @binding(1) var<storage, read> models: array<Caster>;
fn casterWorld(ii: u32, p: vec3<f32>) -> vec4<f32> {
  let c = models[ii];
  let world = c.model * vec4<f32>(p, 1.0);
  return world;
}
@vertex
fn vs(@builtin(instance_index) ii: u32, @location(0) p: vec3<f32>) -> @builtin(position) vec4<f32> {
  return S.lightViewProj * casterWorld(ii, p);
}

struct FadeOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(flat) fade: vec4<f32>,
};
@vertex
fn vsFade(@builtin(instance_index) ii: u32, @location(0) p: vec3<f32>) -> FadeOut {
  var o: FadeOut;
  o.pos = S.lightViewProj * casterWorld(ii, p);
  o.fade = models[ii].fade;
  return o;
}
@fragment
fn fsFade(in: FadeOut) {
  if (!fadeKeeps(in.fade, in.pos.xy, vec2<f32>(0.0))) {
    discard;
  }
}

@group(0) @binding(2) var surfaceImages: texture_2d_array<f32>;
@group(0) @binding(3) var surfaceSampler: sampler;
struct FoliageOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(flat) fade: vec4<f32>,
  @location(1) uv: vec2<f32>,
  @location(2) @interpolate(flat) imageCode: f32,
};
@vertex
fn vsFoliageFade(@builtin(instance_index) ii: u32, @location(0) p: vec3<f32>,
                 @location(2) uv: vec2<f32>, @location(3) pivot: vec4<f32>, @location(4) card: vec4<f32>) -> FoliageOut {
  let caster = models[ii];
  let rest = select(p, card.xyz, card.w > 0.0);
  var leaf = windLeaf((caster.model * vec4<f32>(rest, 1.0)).xyz, vec3<f32>(0.0, 0.0, 1.0),
    (caster.model * vec4<f32>(pivot.xyz, 1.0)).xyz, caster.model[3].xyz, pivot.w * caster.fade.w * windLeafScale, S.wind);
  if (card.w > 0.0) {
    leaf.position += leafCardOffset(p, card.xyz, vec2<f32>(length(caster.model[0].xyz), length(caster.model[2].xyz)),
      S.cardRight.xyz, S.cardUp.xyz);
  }
  var result: FoliageOut;
  result.pos = S.lightViewProj * vec4<f32>(leaf.position, 1.0);
  result.fade = caster.fade;
  result.uv = uv;
  result.imageCode = caster.surface.x;
  return result;
}

@fragment
fn fsFoliage(in: FoliageOut) {
  let uvDx = dpdx(in.uv);
  let uvDy = dpdy(in.uv);
  if (in.imageCode < -0.5) {
    let layer = i32(-in.imageCode - 1.0);
    if (textureSampleGrad(surfaceImages, surfaceSampler, in.uv, layer, uvDx, uvDy).a < 0.5) {
      discard;
    }
  }
  if (!fadeKeeps(in.fade, in.pos.xy, vec2<f32>(0.0))) { discard; }
}
