struct CompositeU {
  p0: vec4<f32>,
  p1: vec4<f32>,
  p2: vec4<f32>,
  p3: vec4<f32>,
};
@group(0) @binding(0) var<uniform> P: CompositeU;
@group(0) @binding(1) var litTex: texture_2d<f32>;
@group(0) @binding(2) var litSamp: sampler;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
fn aces(x: vec3<f32>) -> vec3<f32> {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
               vec3<f32>(0.0), vec3<f32>(1.0));
}
fn pbrNeutral(color: vec3<f32>) -> vec3<f32> {
  let startCompression = 0.8 - 0.04;
  let desaturation = 0.04;
  var c = color;
  let x = min(c.r, min(c.g, c.b));
  var offset = 0.04;
  if (x < 0.08) { offset = x - 6.25 * x * x; }
  c = c - vec3<f32>(offset);
  let peak = max(c.r, max(c.g, c.b));
  if (peak < startCompression) { return clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)); }
  let d = 1.0 - startCompression;
  let newPeak = 1.0 - d * d / (peak + d - startCompression);
  c = c * (newPeak / peak);
  let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
  return clamp(mix(c, vec3<f32>(newPeak), g), vec3<f32>(0.0), vec3<f32>(1.0));
}
fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }
fn vibrance(c: vec3<f32>, amount: f32) -> vec3<f32> {
  let mx = max(c.r, max(c.g, c.b));
  let mn = min(c.r, min(c.g, c.b));
  let w = 1.0 - (mx - mn);
  return mix(vec3<f32>(luma(c)), c, 1.0 + amount * w);
}
fn shade(uv: vec2<f32>) -> vec3<f32> {
  let hdr = textureSample(litTex, litSamp, uv).rgb;
  let exposed = hdr * P.p0.x;
  var c = aces(exposed);
  if (P.p0.z > 0.5) { c = pbrNeutral(exposed); }
  c = pow(c, vec3<f32>(1.0 / 2.2));
  c = clamp((c - vec3<f32>(0.5)) * P.p0.w + vec3<f32>(0.5), vec3<f32>(0.0), vec3<f32>(1.0));
  c = vibrance(c, P.p1.x);
  c = mix(vec3<f32>(luma(c)), c, P.p1.y);
  let l = luma(c);
  let sw = 1.0 - smoothstep(0.0, 0.5, l);
  let hw = smoothstep(0.5, 1.0, l);
  c = c + P.p2.rgb * (sw * P.p1.z) + P.p3.rgb * (hw * P.p1.w);
  return clamp(c, vec3<f32>(0.0), vec3<f32>(1.0));
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = vec2<f32>(textureDimensions(litTex));
  let uv = (pos.xy * P.p0.y) / dims;
  let center = shade(uv);
  if (P.p2.w < 0.5) { return vec4<f32>(center, 1.0); }
  let texel = P.p0.y / dims;
  let lM  = luma(center);
  let lNW = luma(shade(uv + vec2<f32>(-texel.x, -texel.y)));
  let lNE = luma(shade(uv + vec2<f32>( texel.x, -texel.y)));
  let lSW = luma(shade(uv + vec2<f32>(-texel.x,  texel.y)));
  let lSE = luma(shade(uv + vec2<f32>( texel.x,  texel.y)));
  let lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  let lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  let range = lMax - lMin;
  if (range < max(0.0312, lMax * 0.125)) { return vec4<f32>(center, 1.0); }
  var dir = vec2<f32>(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
  let dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 0.0078125);
  let rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
  dir = clamp(dir * rcpDirMin, vec2<f32>(-8.0), vec2<f32>(8.0)) * texel;
  let rgbA = 0.5 * (shade(uv + dir * (1.0 / 3.0 - 0.5)) + shade(uv + dir * (2.0 / 3.0 - 0.5)));
  let rgbB = rgbA * 0.5 + 0.25 * (shade(uv + dir * -0.5) + shade(uv + dir * 0.5));
  let lB = luma(rgbB);
  if (lB < lMin || lB > lMax) { return vec4<f32>(rgbA, 1.0); }
  return vec4<f32>(rgbB, 1.0);
}
