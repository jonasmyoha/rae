struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
@group(0) @binding(2) var<storage, read> palette: array<vec4<f32>>;
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
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) nrm: vec3<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) vcol: vec3<f32>,
};
@vertex
fn vs(@builtin(instance_index) ii: u32,
      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>,
      @location(3) jf: vec4<f32>, @location(4) w: vec4<f32>,
      @location(5) vc: vec4<f32>) -> VsOut {
  let d = draws[ii];
  let j = vec4<u32>(u32(jf.x), u32(jf.y), u32(jf.z), u32(jf.w));
  let pbase = u32(max(d.params.w, 0.0));
  var skin = jointMat(j.x, pbase) * w.x;
  skin = skin + jointMat(j.y, pbase) * w.y;
  skin = skin + jointMat(j.z, pbase) * w.z;
  skin = skin + jointMat(j.w, pbase) * w.w;
  let sp = skin * vec4<f32>(p, 1.0);
  var o: VsOut;
  o.pos = F.viewProj * (d.model * vec4<f32>(sp.xyz, 1.0));
  let sn = skin * vec4<f32>(n, 0.0);
  o.nrm = normalize((d.model * vec4<f32>(sn.xyz, 0.0)).xyz);
  o.inst = ii;
  o.vcol = vc.rgb;
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * (d.prevModel * vec4<f32>(sp.xyz, 1.0));
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  return o;
}
struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};
@fragment
fn fs(in: VsOut) -> FsOut {
  let d = draws[in.inst];
  let n = normalize(in.nrm);
  let oct = octEncode(n);
  let rough = clamp(d.params.x, 0.045, 1.0);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);
  o.gbb = vec4<f32>(d.albedoMetallic.rgb * in.vcol, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y,
                     clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);
  return o;
}
