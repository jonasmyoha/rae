// gpu2d_image.wgsl — the 2D image draw (one textured, rounded, tinted quad per
// draw), a Rae asset since #908 (was the C constant G2D_IMG_WGSL). Output is
// premultiplied. uImg = rect, tint (straight, premultiplied here),
// params (radius, rotation about the quad center in radians, tile flag, premultiplied source),
// uv (origin + size), clip rect (design units), clip (radius, enabled) —
// the last two are the rounded clip the box pass has always had (#1003),
// written per draw from the clip active when the image was queued; the clip's
// vec4 ends with the color change (hue turn in radians, brightness less 1;
// 0, 0 none: Gpu2dCanvasImage canvasSetImageColor).
@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;
@group(0) @binding(1) var<uniform> uImg: array<vec4<f32>, 6>;
@group(0) @binding(2) var tex: texture_2d<f32>;
@group(0) @binding(3) var samp: sampler;
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) local: vec2<f32>,
  @location(2) posD: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VsOut {
  var corners = array<vec2<f32>, 6>(
    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),
    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));
  let c = corners[vi];
  let rect = uImg[0];
  let phys = uXform[0].xy;
  // Rotate the quad about its center (#1003); uv and the rounding SDF stay in
  // the unrotated local frame.
  let local = c * rect.zw;
  let center = rect.zw * 0.5;
  let a = uImg[2].y;
  let ca = cos(a); let sa = sin(a);
  let rel = local - center;
  let rot = vec2<f32>(rel.x * ca - rel.y * sa, rel.x * sa + rel.y * ca);
  let posDesign = rect.xy + center + rot;
  let posPx = posDesign * uXform[0].zw + uXform[1].xy;
  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0, 1.0 - posPx.y / phys.y * 2.0);
  var o: VsOut;
  o.pos = vec4<f32>(ndc, 0.0, 1.0);
  let uv = uImg[3];
  o.uv = uv.xy + c * uv.zw;
  o.local = local;
  o.posD = posDesign;
  return o;
}
fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {
  let q = abs(p) - b + vec2<f32>(r, r);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;
}
// The color turned about the gray axis by `angle` (Pixi's ColorMatrixFilter.hue) and scaled by
// `brightness`, held to 0..1; the same as Gpu2dCanvasImage imageColorOf. Linear in rgb, so it
// applies to a premultiplied color as well.
fn imageColor(rgb: vec3<f32>, angle: f32, brightness: f32) -> vec3<f32> {
  let c = cos(angle);
  let s = sin(angle);
  let third = 1.0 / 3.0;
  let rootThird = sqrt(third);
  let same = c + (1.0 - c) * third;
  let ahead = third * (1.0 - c) - rootThird * s;
  let behind = third * (1.0 - c) + rootThird * s;
  let turned = vec3<f32>(
    rgb.r * same + rgb.g * ahead + rgb.b * behind,
    rgb.r * behind + rgb.g * same + rgb.b * ahead,
    rgb.r * ahead + rgb.g * behind + rgb.b * same);
  return clamp(turned * brightness, vec3<f32>(0.0), vec3<f32>(1.0));
}
@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
  // A tiled draw (params.z) repeats the source: uv beyond 1 wraps, which the
  // clamp sampler alone would not do (no mips, so the derivative seam is moot).
  var uv = in.uv;
  if (uImg[2].z > 0.5) { uv = fract(uv); }
  var texel = textureSample(tex, samp, uv);
  if (uImg[5].z != 0.0 || uImg[5].w != 0.0) {
    texel = vec4<f32>(imageColor(texel.rgb, uImg[5].z, 1.0 + uImg[5].w), texel.a);
  }
  let tint = uImg[1];
  let half = uImg[0].zw * 0.5;
  let rad = uImg[2].x;
  let d = sdRoundBox(in.local - half, half, rad);
  let aa = max(fwidth(d), 0.0001);
  var cov = 1.0 - smoothstep(-aa, aa, d);
  if (uImg[5].y > 0.5) {  // rounded clip coverage (#1003)
    let cc = uImg[4].xy + uImg[4].zw * 0.5;
    let ch = uImg[4].zw * 0.5;
    let cr = uImg[5].x;
    let cd = sdRoundBox(in.posD - cc, ch, cr);
    let caa = max(fwidth(cd), 0.0001);
    cov = cov * (1.0 - smoothstep(-caa, caa, cd));
  }
  // A source that is premultiplied already (uImg[2].w: a cached drawing, Gpu2dCanvasCache)
  if (uImg[2].w > 0.5) {
    let k = tint.a * cov;
    return vec4<f32>(texel.rgb * tint.rgb * k, texel.a * k);
  }
  let a = texel.a * tint.a * cov;
  return vec4<f32>(texel.rgb * tint.rgb * a, a);  // premultiplied
}
