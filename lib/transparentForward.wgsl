// transparentForward.wgsl — the transparent forward pass (#843).
//
// Draws instanced geometry into the lit HDR target with straight alpha blending
// (src-alpha / one-minus-src-alpha, set on the pipeline), testing the OPAQUE
// scene depth read-only: transparents are occluded by geometry but never write
// depth, so they cannot occlude each other or the opaque frame.
//
// It reads the SAME Frame uniform and DrawU records as the G-buffer geometry pass
// (group 0, bindings 0/1), so a record packed for one pass can go to either.
// On this pass albedoMetallic.w is the instance ALPHA (there is no metallic on
// an unlit blend). Unlit on purpose: colour * alpha. Water / particle shading
// belongs to the clients that follow (#830, #844).
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

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(flat) inst: u32,
};

@vertex
fn vs(@builtin(instance_index) ii: u32,
      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {
  let d = draws[ii];
  var o: VsOut;
  let world = d.model * vec4<f32>(p, 1.0);
  o.pos = F.viewProj * world;
  // Jitter LAST, exactly as the G-buffer pass does (#397): the opaque frame
  // was rasterised with this sub-pixel offset, so transparents must be too or
  // their edges swim against the depth they test and TAA then smears them.
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  o.inst = ii;
  return o;
}

@fragment
fn fs(i: VsOut) -> @location(0) vec4<f32> {
  let d = draws[i.inst];
  return vec4<f32>(d.albedoMetallic.rgb, d.albedoMetallic.w);
}
