// transparentForward.wgsl — the transparent forward pass (#843).
//
// Draws instanced geometry into the lit HDR target with premultiplied alpha
// blending (one / one-minus-src-alpha, set on the pipeline: every style returns
// its color times its alpha, and an additive one returns alpha 0), testing the OPAQUE
// scene depth read-only: transparents are occluded by geometry but never write
// depth, so they cannot occlude each other or the opaque frame.
//
// It reads the SAME Frame uniform and DrawU records as the G-buffer geometry pass
// (group 0, bindings 0/1), so a record packed for one pass can go to either.
// On this pass albedoMetallic.w is the instance ALPHA (there is no metallic on
// an unlit blend). params.x selects a style: 0 plain (the original unlit color
// and alpha), 1 an opt-in rounded water-drop impostor, 2 a soft puff of smoke
// or dust (it thins toward its silhouette and is lit from above), 3 a soft glow
// (it thins toward its silhouette, with a hot core): lib/Explosions, 4 a
// shimmering surface (bands that run in time, brighter seen side-on, alpha or
// additive; its settings in params.yzw and fade.xy, TransparentForward
// addTransparentShimmerInstance).
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
  @location(2) normal: vec3<f32>,
  @location(3) world: vec3<f32>,
  @location(4) eye: vec3<f32>,
};

// The camera's position from the view-projection: the point whose clip x, y
// and w are all 0 (rows 0, 1 and 3 of the matrix, solved by Cramer's rule)
fn eyeOf(m: mat4x4<f32>) -> vec3<f32> {
  let r0 = vec4<f32>(m[0][0], m[1][0], m[2][0], m[3][0]);
  let r1 = vec4<f32>(m[0][1], m[1][1], m[2][1], m[3][1]);
  let r3 = vec4<f32>(m[0][3], m[1][3], m[2][3], m[3][3]);
  let a = r0.xyz;
  let b = r1.xyz;
  let c = r3.xyz;
  let bc = cross(b, c);
  let det = dot(a, bc);
  return -(bc * r0.w + cross(c, a) * r1.w + cross(a, b) * r3.w) / det;
}

// A shimmering surface (style 4): bands up the surface that run in time, a
// finer ripple across them (whole turns of uv.x, so a cylinder has no seam),
// and more light and alpha where it is seen side-on (the fresnel term). Both
// faces: the normal's side does not matter.
fn shimmer(d: DrawU, i: VsOut) -> vec4<f32> {
  let time = d.params.y;
  let bands = d.params.z;
  let speed = d.params.w;
  let tau = 6.2831853;
  let height = i.world.z * bands;
  let main = sin(tau * (height - time * speed));
  let across = sin(tau * (i.uv.x * 13.0 + height * 0.37 + time * speed * 0.23));
  let fine = sin(tau * (i.uv.x * 29.0 - height * 0.61 - time * speed * 0.41));
  let wave = smoothstep(0.3, 1.0, 0.5 + 0.5 * (0.55 * main + 0.3 * across + 0.15 * fine));
  let toEye = normalize(i.eye - i.world);
  let facing = abs(dot(normalize(i.normal), toEye));
  let edge = pow(1.0 - facing, 3.0) * d.fade.y;
  let alpha = clamp(d.albedoMetallic.w * (0.35 + 0.65 * wave) + edge * 0.5, 0.0, 1.0);
  let color = d.albedoMetallic.rgb * (0.6 + 0.8 * wave + 1.5 * edge);
  if (d.fade.x > 0.5) {
    return vec4<f32>(color * alpha, 0.0);
  }
  return vec4<f32>(color * alpha, alpha);
}

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
  o.normal = (d.model * vec4<f32>(n, 0.0)).xyz;
  o.world = world.xyz;
  o.eye = eyeOf(F.viewProj);
  return o;
}

@fragment
fn fs(i: VsOut) -> @location(0) vec4<f32> {
  let d = draws[i.inst];
  let style = d.params.x;
  if (style > 3.5) {
    return shimmer(d, i);
  }
  if (style > 1.5) {
    // How squarely this point of the puff faces the camera: the view's forward
    // axis is the clip w row, so the normal's clip w is minus its facing
    let normal = normalize(i.normal);
    let facing = clamp(-(F.viewProj * vec4<f32>(normal, 0.0)).w, 0.0, 1.0);
    let soft = smoothstep(0.0, 0.8, facing);
    if (style > 2.5) {
      let glowAlpha = d.albedoMetallic.w * soft;
      return vec4<f32>(d.albedoMetallic.rgb * (0.55 + 0.9 * facing * facing) * glowAlpha, glowAlpha);
    }
    let fromAbove = 0.5 + 0.5 * normal.z;
    let puffAlpha = d.albedoMetallic.w * soft;
    return vec4<f32>(d.albedoMetallic.rgb * (0.55 + 0.6 * fromAbove) * puffAlpha, puffAlpha);
  }
  if (style > 0.5) {
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
    return vec4<f32>(color * alpha, alpha);
  }
  return vec4<f32>(d.albedoMetallic.rgb * d.albedoMetallic.w, d.albedoMetallic.w);
}
