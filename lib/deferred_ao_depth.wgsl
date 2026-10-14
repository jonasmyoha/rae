// The half-resolution depth the SSAO marches over (render-perf-6). Each half
// pixel keeps ONE of its 2x2 full-resolution depths, in a checkerboard: even
// cells the nearest (the largest reverse-Z value), odd cells the farthest. So
// at a silhouette both the object and what is behind it survive, and the AO
// does not lose thin edges. It also stores which of the four it kept (0-3),
// so the SSAO reads that texel's normal and the true position of that sample.
@group(0) @binding(0) var depthTex: texture_depth_2d;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) fc: vec4<f32>) -> @location(0) vec4<f32> {
  let fullDim = vec2<i32>(textureDimensions(depthTex));
  let halfPx = vec2<i32>(fc.xy);
  let base = halfPx * 2;
  let keepNearest = ((halfPx.x + halfPx.y) & 1) == 0;
  var best = 2.0;
  if (keepNearest) { best = -1.0; }
  var bestIndex = 0;
  for (var i = 0; i < 4; i = i + 1) {
    let texel = min(base + vec2<i32>(i & 1, i >> 1u), fullDim - vec2<i32>(1));
    let d = textureLoad(depthTex, texel, 0);
    if ((keepNearest && d > best) || (!keepNearest && d < best)) {
      best = d;
      bestIndex = i;
    }
  }
  return vec4<f32>(best, f32(bestIndex), 0.0, 1.0);
}
