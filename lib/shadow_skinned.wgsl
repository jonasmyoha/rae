struct SU { lightViewProj: mat4x4<f32> };
@group(0) @binding(0) var<uniform> S: SU;
@group(0) @binding(1) var<storage, read> models: array<mat4x4<f32>>;
@group(0) @binding(2) var<storage, read> palette: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read> paletteBases: array<u32>;
fn jointMat(j: u32, base: u32) -> mat4x4<f32> {
  let r0 = palette[base + j * 3u + 0u];
  let r1 = palette[base + j * 3u + 1u];
  let r2 = palette[base + j * 3u + 2u];
  return mat4x4<f32>(
    vec4<f32>(r0.x, r1.x, r2.x, 0.0),
    vec4<f32>(r0.y, r1.y, r2.y, 0.0),
    vec4<f32>(r0.z, r1.z, r2.z, 0.0),
    vec4<f32>(r0.w, r1.w, r2.w, 1.0));
}
@vertex
fn vs(@builtin(instance_index) ii: u32,
      @location(0) p: vec3<f32>,
      @location(3) jf: vec4<f32>, @location(4) w: vec4<f32>) -> @builtin(position) vec4<f32> {
  let j = vec4<u32>(u32(jf.x), u32(jf.y), u32(jf.z), u32(jf.w));
  let pbase = paletteBases[ii];
  var skin = jointMat(j.x, pbase) * w.x;
  skin = skin + jointMat(j.y, pbase) * w.y;
  skin = skin + jointMat(j.z, pbase) * w.z;
  skin = skin + jointMat(j.w, pbase) * w.w;
  let sp = skin * vec4<f32>(p, 1.0);
  return S.lightViewProj * (models[ii] * vec4<f32>(sp.xyz, 1.0));
}
