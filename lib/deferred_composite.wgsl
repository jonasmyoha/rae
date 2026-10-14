struct CompositeU {
  p0: vec4<f32>,
  p1: vec4<f32>,
  p2: vec4<f32>,
  p3: vec4<f32>,
};
@group(0) @binding(0) var<uniform> P: CompositeU;
@group(0) @binding(1) var litTex: texture_2d<f32>;
@group(0) @binding(2) var litSamp: sampler;
// The half-size bloom (GbufferBloom, render-perf-8), added before exposure
// with strength P.p2.w; 0 means bloom is off and the texture is not read.
@group(0) @binding(3) var bloomTex: texture_2d<f32>;
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
// The lit target has one mip, so the explicit level 0 samples exactly what
// textureSample would, without derivatives.
fn shade(uv: vec2<f32>) -> vec3<f32> {
  var hdr = textureSampleLevel(litTex, litSamp, uv, 0.0).rgb;
  if (P.p2.w > 0.0) {
    hdr = hdr + textureSampleLevel(bloomTex, litSamp, uv, 0.0).rgb * P.p2.w;
  }
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
// Anti-aliasing ran before this, at the scene resolution (the antialias pass,
// deferred_antialias.wgsl); the composite only tone-maps, grades and upscales,
// one shade() per native pixel.
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = vec2<f32>(textureDimensions(litTex));
  let uv = (pos.xy * P.p0.y) / dims;
  return vec4<f32>(shade(uv), 1.0);
}
