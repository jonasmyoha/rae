// gpu2d_text.wgsl — the 2D MSDF text pass (#111), a Rae asset since #909 (was
// the C constant G2D_TEXT_WGSL). Instanced glyph quads sampling an MSDF atlas,
// antialiased with the median-of-3 + screen-px-range trick; premultiplied
// output, optional dilated outline composited under the body, and a softness
// (px) that widens the coverage falloff for soft shadows. Instance = 5 x vec4:
// rect, uv, color, params(pxRange, outlineWidth, softness, rotation), outline
// colour. A glyph rotates about its own centre by params.w (#1003; the canvas
// already moved the centre about the run's pivot). uClip is the rounded clip
// the box pass has always had, bound per same-clip run.
struct Glyph {
  rect: vec4<f32>,
  uv: vec4<f32>,
  color: vec4<f32>,
  params: vec4<f32>,  // x=pxRange, y=outlineWidth(px), z=softness(px), w=rotation
  outline: vec4<f32>,  // straight outline colour
};
@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;
@group(0) @binding(1) var<storage, read> glyphs: array<Glyph>;
@group(0) @binding(2) var atlasTex: texture_2d<f32>;
@group(0) @binding(3) var atlasSamp: sampler;
@group(0) @binding(4) var<uniform> uClip: array<vec4<f32>, 2>;
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) posD: vec2<f32>,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
  var corners = array<vec2<f32>, 6>(
    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),
    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));
  let c = corners[vi];
  let g = glyphs[ii];
  let phys = uXform[0].xy;
  let local = c * g.rect.zw;
  let center = g.rect.zw * 0.5;
  let a = g.params.w;
  let ca = cos(a); let sa = sin(a);
  let rel = local - center;
  let rot = vec2<f32>(rel.x * ca - rel.y * sa, rel.x * sa + rel.y * ca);
  let posDesign = g.rect.xy + center + rot;
  let posPx = posDesign * uXform[0].zw + uXform[1].xy;
  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0,
                      1.0 - posPx.y / phys.y * 2.0);
  var o: VsOut;
  o.pos = vec4<f32>(ndc, 0.0, 1.0);
  o.uv = g.uv.xy + c * g.uv.zw;
  o.inst = ii;
  o.posD = posDesign;
  return o;
}
fn median3(r: f32, g: f32, b: f32) -> f32 {
  return max(min(r, g), min(max(r, g), b));
}
fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {
  let q = abs(p) - b + vec2<f32>(r, r);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;
}
fn clipCoverage(posD: vec2<f32>) -> f32 {
  if (uClip[1].y < 0.5) { return 1.0; }
  let cc = uClip[0].xy + uClip[0].zw * 0.5;
  let ch = uClip[0].zw * 0.5;
  let cd = sdRoundBox(posD - cc, ch, uClip[1].x);
  let caa = max(fwidth(cd), 0.0001);
  return 1.0 - smoothstep(-caa, caa, cd);
}
@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
  let g = glyphs[in.inst];
  let s = textureSample(atlasTex, atlasSamp, in.uv);
// signed distance from the glyph edge, in physical px (design pxRange * avg
// scale). Positive inside the glyph.
  let sc = (uXform[0].z + uXform[0].w) * 0.5;
  let sd = g.params.x * sc * (median3(s.r, s.g, s.b) - 0.5);
// softness widens the coverage falloff (px) — 1 = crisp AA, larger = a soft
// blurred edge (used for soft drop-shadows).
  let sw = max(g.params.z, 1.0);
  let bodyCov = clamp(sd / sw + 0.5, 0.0, 1.0) * clipCoverage(in.posD);
  let ow = g.params.y;
  if (ow <= 0.0) {
    let a = g.color.a * bodyCov;
    return vec4<f32>(g.color.rgb * a, a);  // premultiplied
  }
// Outline = the glyph dilated by `ow` px; composite body OVER outline,
// both premultiplied.
  let outerCov = clamp((sd + ow) / sw + 0.5, 0.0, 1.0) * clipCoverage(in.posD);
  let ba = g.color.a * bodyCov;
  let oa = g.outline.a * outerCov;
  let outA = ba + oa * (1.0 - ba);
  let outRGB = g.color.rgb * ba + g.outline.rgb * oa * (1.0 - ba);
  return vec4<f32>(outRGB, outA);  // premultiplied
}
