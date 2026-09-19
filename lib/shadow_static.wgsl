struct SU { lightViewProj: mat4x4<f32> };
@group(0) @binding(0) var<uniform> S: SU;
@group(0) @binding(1) var<storage, read> models: array<mat4x4<f32>>;
@vertex
fn vs(@builtin(instance_index) ii: u32, @location(0) p: vec3<f32>) -> @builtin(position) vec4<f32> {
  return S.lightViewProj * (models[ii] * vec4<f32>(p, 1.0));
}
