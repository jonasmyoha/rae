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
// an unlit blend). params.x selects an opt-in rounded water-drop impostor.
// Ordinary transparent geometry keeps its original unlit color and alpha.
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
  fade: vec4<f32>,     // the level cross-fade (GbufferRecord [40..43]); 0 when not fading
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) @interpolate(flat) inst: u32,
  @location(1) uv: vec2<f32>,
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
  o.uv = uv;
  return o;
}

@fragment
fn fs(i: VsOut) -> @location(0) vec4<f32> {
  let d = draws[i.inst];
  if (d.params.x > 0.5) {
    // Analytic silhouette: no square edges, texture atlas, or per-drop geometry.
    let q = i.uv * 2.0 - 1.0;
    let radius = length(q);
    let edge = max(fwidth(radius), 0.025);
    let coverage = 1.0 - smoothstep(1.0 - edge, 1.0, radius);
    if (coverage <= 0.0) { discard; }
    // A clear center, blue-gray lower rim and a small sky glint. This is an
    // inexpensive water impression, not scene refraction or a physical BRDF.
    let rim = smoothstep(0.45, 0.96, radius);
    let glint = exp(-32.0 * dot(q - vec2<f32>(-0.30, 0.38), q - vec2<f32>(-0.30, 0.38)));
    let reflection = smoothstep(-0.35, 0.65, q.y);
    // The instance color already includes current environmental illumination.
    // Light the glint too: a constant white addition made droplets glow at night.
    let color = d.albedoMetallic.rgb * (mix(0.45, 1.0, reflection) + glint * 0.85);
    let alpha = coverage * d.albedoMetallic.w * (0.12 + 0.20 * rim + 0.68 * glint);
    return vec4<f32>(color, alpha);
  }
  return vec4<f32>(d.albedoMetallic.rgb, d.albedoMetallic.w);
}
