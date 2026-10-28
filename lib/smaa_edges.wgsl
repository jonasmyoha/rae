// Ported from SMAA: Copyright (c) 2013 Jorge Jimenez et al., MIT License (lib/LICENSE-smaa.md)
// SMAA 1x, pass 1 (render-perf-9): luma edge detection (SMAALumaEdgeDetectionPS)
// over the lit HDR image at the scene resolution. SMAA wants gamma-encoded
// colors, so the luma is taken of the tone-mapped, gamma-encoded value
// (like the FXAA pass). Output RG: an edge on the left / top of the pixel.
// A pixel with no edge is discarded and keeps the cleared 0.
struct EdgesU {
  p0: vec4<f32>,  // x: exposure
};
@group(0) @binding(0) var<uniform> U: EdgesU;
@group(0) @binding(1) var litTex: texture_2d<f32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
// The "high" preset's threshold
const SMAA_THRESHOLD: f32 = 0.1;
const SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR: f32 = 2.0;
fn lumaAt(px: vec2<i32>, dims: vec2<i32>) -> f32 {
  let c = textureLoad(litTex, clamp(px, vec2<i32>(0), dims - vec2<i32>(1)), 0).rgb * U.p0.x;
  let mapped = c / (vec3<f32>(1.0) + c);
  return pow(dot(mapped, vec3<f32>(0.2126, 0.7152, 0.0722)), 1.0 / 2.2);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = vec2<i32>(textureDimensions(litTex));
  let px = vec2<i32>(pos.xy);
  let L = lumaAt(px, dims);
  let Lleft = lumaAt(px + vec2<i32>(-1, 0), dims);
  let Ltop = lumaAt(px + vec2<i32>(0, -1), dims);
  var delta = vec4<f32>(abs(L - vec2<f32>(Lleft, Ltop)), 0.0, 0.0);
  var edges = step(vec2<f32>(SMAA_THRESHOLD), delta.xy);
  if (dot(edges, vec2<f32>(1.0)) == 0.0) { discard; }
  let Lright = lumaAt(px + vec2<i32>(1, 0), dims);
  let Lbottom = lumaAt(px + vec2<i32>(0, 1), dims);
  delta = vec4<f32>(delta.xy, abs(L - vec2<f32>(Lright, Lbottom)));
  var maxDelta = max(delta.xy, delta.zw);
  let Lleftleft = lumaAt(px + vec2<i32>(-2, 0), dims);
  let Ltoptop = lumaAt(px + vec2<i32>(0, -2), dims);
  delta = vec4<f32>(delta.xy, abs(vec2<f32>(Lleft, Ltop) - vec2<f32>(Lleftleft, Ltoptop)));
  maxDelta = max(maxDelta, delta.zw);
  let finalDelta = max(maxDelta.x, maxDelta.y);
  // Local contrast adaptation
  edges = edges * step(vec2<f32>(finalDelta), SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
  return vec4<f32>(edges, 0.0, 1.0);
}
