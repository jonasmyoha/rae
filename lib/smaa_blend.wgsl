// Ported from SMAA: Copyright (c) 2013 Jorge Jimenez et al., MIT License (lib/LICENSE-smaa.md)
// SMAA 1x, pass 3 (render-perf-9): neighbourhood blending
// (SMAANeighborhoodBlendingPS). Each pixel mixes with the neighbour its
// blending weights point at, through one bilinear fetch, in HDR; the result
// is the anti-aliased scene the composite upscales.
@group(0) @binding(0) var litTex: texture_2d<f32>;
@group(0) @binding(1) var blendTex: texture_2d<f32>;
@group(0) @binding(2) var linearSamp: sampler;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = vec2<f32>(textureDimensions(litTex));
  let rt = vec4<f32>(vec2<f32>(1.0) / dims, dims);
  let texcoord = pos.xy * rt.xy;
  let offset = rt.xyxy * vec4<f32>(1.0, 0.0, 0.0, 1.0) + texcoord.xyxy;
  var a: vec4<f32>;
  a.x = textureSampleLevel(blendTex, linearSamp, offset.xy, 0.0).a;  // right
  a.y = textureSampleLevel(blendTex, linearSamp, offset.zw, 0.0).g;  // top
  let own = textureSampleLevel(blendTex, linearSamp, texcoord, 0.0);
  a.w = own.x;  // bottom
  a.z = own.z;  // left
  if (dot(a, vec4<f32>(1.0)) < 1e-5) {
    return vec4<f32>(textureSampleLevel(litTex, linearSamp, texcoord, 0.0).rgb, 1.0);
  }
  let horizontal = max(a.x, a.z) > max(a.y, a.w);
  var blendingOffset = vec4<f32>(0.0, a.y, 0.0, a.w);
  var blendingWeight = a.yw;
  if (horizontal) {
    blendingOffset = vec4<f32>(a.x, 0.0, a.z, 0.0);
    blendingWeight = a.xz;
  }
  blendingWeight = blendingWeight / dot(blendingWeight, vec2<f32>(1.0));
  let blendingCoord = blendingOffset * vec4<f32>(rt.xy, -rt.xy) + texcoord.xyxy;
  var color = blendingWeight.x * textureSampleLevel(litTex, linearSamp, blendingCoord.xy, 0.0).rgb;
  color = color + blendingWeight.y * textureSampleLevel(litTex, linearSamp, blendingCoord.zw, 0.0).rgb;
  return vec4<f32>(color, 1.0);
}
