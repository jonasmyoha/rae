// FXAA at the SCENE resolution, before the composite upscales to native
// (render-perf-4). It used to run inside the composite, after the bilinear
// upscale — where the edges are already smeared — and shaded every native
// pixel five to nine times. Here it reads the lit (HDR) image once per scene
// pixel and writes an anti-aliased HDR image of the same size; the composite
// then only tone-maps, grades and upscales.
//
// Edges are found on a tone-mapped, gamma-encoded luma (the FXAA thresholds
// assume display values, and raw HDR luma is unbounded; without the gamma, dark
// edges fall under the threshold); the colors blended are the HDR ones, so the
// composite's tone map and grade see exactly what they did.
struct AntialiasU {
  p0: vec4<f32>,  // x: exposure
};
@group(0) @binding(0) var<uniform> P: AntialiasU;
@group(0) @binding(1) var litTex: texture_2d<f32>;
@group(0) @binding(2) var litSamp: sampler;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
// One mip, so level 0 is exactly the texel and needs no derivatives (the
// early returns below make the control flow non-uniform).
fn hdr(uv: vec2<f32>) -> vec3<f32> {
  return textureSampleLevel(litTex, litSamp, uv, 0.0).rgb;
}
fn displayLuma(c: vec3<f32>) -> f32 {
  let exposed = c * P.p0.x;
  let mapped = exposed / (vec3<f32>(1.0) + exposed);
  return pow(dot(mapped, vec3<f32>(0.2126, 0.7152, 0.0722)), 1.0 / 2.2);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = vec2<f32>(textureDimensions(litTex));
  let texel = vec2<f32>(1.0) / dims;
  let uv = pos.xy * texel;
  let center = hdr(uv);
  let lM  = displayLuma(center);
  let lNW = displayLuma(hdr(uv + vec2<f32>(-texel.x, -texel.y)));
  let lNE = displayLuma(hdr(uv + vec2<f32>( texel.x, -texel.y)));
  let lSW = displayLuma(hdr(uv + vec2<f32>(-texel.x,  texel.y)));
  let lSE = displayLuma(hdr(uv + vec2<f32>( texel.x,  texel.y)));
  let lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  let lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  let range = lMax - lMin;
  if (range < max(0.0312, lMax * 0.125)) { return vec4<f32>(center, 1.0); }
  var dir = vec2<f32>(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
  let dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 0.0078125);
  let rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
  dir = clamp(dir * rcpDirMin, vec2<f32>(-8.0), vec2<f32>(8.0)) * texel;
  let rgbA = 0.5 * (hdr(uv + dir * (1.0 / 3.0 - 0.5)) + hdr(uv + dir * (2.0 / 3.0 - 0.5)));
  let rgbB = rgbA * 0.5 + 0.25 * (hdr(uv + dir * -0.5) + hdr(uv + dir * 0.5));
  let lB = displayLuma(rgbB);
  if (lB < lMin || lB > lMax) { return vec4<f32>(rgbA, 1.0); }
  return vec4<f32>(rgbB, 1.0);
}
