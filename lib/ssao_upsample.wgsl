// The SSAO at a full-size pixel (render-perf-6): the AO pass is half
// resolution, so it is upsampled from the four half texels around the pixel,
// bilinear weights times depth similarity, so the AO of a surface does not
// bleed onto the one in front of it. 1 where no half texel has depth. Shared by
// the deferred lighting (deferred_light.wgsl) and the forward pass with SSAO
// (forward_entry / forward_static / forward_sdf); the includer declares
// `aoTex` (the AO) and `aoDepthTex` (the AO pass's half-size depth).
fn ssaoAt(px: vec2<i32>, depth: f32) -> f32 {
  var aoSum = 0.0;
  var aoW = 0.0;
  let dims = vec2<i32>(textureDimensions(aoTex));
  let halfPos = (vec2<f32>(px) + vec2<f32>(0.5)) * 0.5 - vec2<f32>(0.5);
  let halfBase = vec2<i32>(floor(halfPos));
  let halfFrac = halfPos - floor(halfPos);
  for (var i = 0; i < 4; i = i + 1) {
    let o = vec2<i32>(i & 1, i >> 1u);
    let q = clamp(halfBase + o, vec2<i32>(0), dims - vec2<i32>(1));
    let qd = textureLoad(aoDepthTex, q, 0).x;
    if (qd <= 0.0) { continue; }
    let bx = select(1.0 - halfFrac.x, halfFrac.x, o.x == 1);
    let by = select(1.0 - halfFrac.y, halfFrac.y, o.y == 1);
    let w = (bx * by + 1e-3) / (1.0 + abs(qd - depth) / depth * 40.0);
    aoSum = aoSum + textureLoad(aoTex, q, 0).r * w;
    aoW = aoW + w;
  }
  if (aoW > 0.0) { return aoSum / aoW; }
  return 1.0;
}
