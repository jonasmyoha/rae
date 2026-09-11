// gpu2d_text.wgsl — the 2D MSDF text pass (#111), a Rae asset since #909 (was
// the C constant G2D_TEXT_WGSL). Instanced glyph quads sampling an MSDF atlas,
// antialiased with the median-of-3 + screen-px-range trick; premultiplied
// output, optional dilated outline composited under the body, and a softness
// (px) that widens the coverage falloff for soft shadows. Instance = 5 x vec4:
// rect, uv, color, params(pxRange, outlineWidth, softness), outline colour.
struct Glyph {
  rect: vec4<f32>,
  uv: vec4<f32>,
  color: vec4<f32>,
  params: vec4<f32>,  // x=pxRange, y=outlineWidth(px), z=softness(px)
  outline: vec4<f32>,  // straight outline colour
};
@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;
@group(0) @binding(1) var<storage, read> glyphs: array<Glyph>;
@group(0) @binding(2) var atlasTex: texture_2d<f32>;
@group(0) @binding(3) var atlasSamp: sampler;
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) @interpolate(flat) inst: u32,
};
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
  var corners = array<vec2<f32>, 6>(
    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),
    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));
  let c = corners[vi];
  let g = glyphs[ii];
  let phys = uXform[0].xy;
  let posPx = (g.rect.xy + c * g.rect.zw) * uXform[0].zw + uXform[1].xy;
  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0,
                      1.0 - posPx.y / phys.y * 2.0);
  var o: VsOut;
  o.pos = vec4<f32>(ndc, 0.0, 1.0);
  o.uv = g.uv.xy + c * g.uv.zw;
  o.inst = ii;
  return o;
}
fn median3(r: f32, g: f32, b: f32) -> f32 {
  return max(min(r, g), min(max(r, g), b));
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
  let bodyCov = clamp(sd / sw + 0.5, 0.0, 1.0);
  let ow = g.params.y;
  if (ow <= 0.0) {
    let a = g.color.a * bodyCov;
    return vec4<f32>(g.color.rgb * a, a);  // premultiplied
  }
// Outline = the glyph dilated by `ow` px; composite body OVER outline,
// both premultiplied.
  let outerCov = clamp((sd + ow) / sw + 0.5, 0.0, 1.0);
  let ba = g.color.a * bodyCov;
  let oa = g.outline.a * outerCov;
  let outA = ba + oa * (1.0 - ba);
  let outRGB = g.color.rgb * ba + g.outline.rgb * oa * (1.0 - ba);
  return vec4<f32>(outRGB, outA);  // premultiplied
}
