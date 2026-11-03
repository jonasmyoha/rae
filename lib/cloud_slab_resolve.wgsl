// The slab tier's resolve (lib/CloudSlab): the half-resolution clouds. A
// pixel traced this frame takes the trace; the three others of its 2x2 tile
// take the history (the frame before's resolve), reprojected along their
// view ray with the frame before's view-projection, or the nearest trace
// where the history has no such pixel (the screen's edge). The history is
// held to the range of the four traces round it, so a moving cloud leaves
// no trail.
const PI = 3.14159265;  // sky_hosek.wgsl reads it
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var<uniform> S: SlabU;
@group(0) @binding(2) var traceTex: texture_2d<f32>;
@group(0) @binding(3) var historyTex: texture_2d<f32>;
fn slabTrace(cell: vec2<i32>) -> vec4<f32> {
  let size = vec2<i32>(S.sizes.xy);
  return textureLoad(traceTex, clamp(cell, vec2<i32>(0), size - vec2<i32>(1)), 0);
}
// The history along this pixel's ray (a far point seen by the frame before),
// blended between texels; w below 0 when the frame before did not see it
fn slabHistory(pos: vec2<f32>) -> vec4<f32> {
  let dir = cloudSlabDir(pos, S.sizes.zw);
  let clip = S.prevViewProj * vec4<f32>(L.camPos.xyz + dir * 20000.0, 1.0);
  if (clip.w <= 0.0) { return vec4<f32>(-1.0); }
  let ndc = clip.xy / clip.w;
  let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) { return vec4<f32>(-1.0); }
  let at = uv * S.sizes.zw - 0.5;
  let size = vec2<i32>(S.sizes.zw);
  let cell = clamp(vec2<i32>(floor(at)), vec2<i32>(0), size - vec2<i32>(2));
  let f = clamp(at - vec2<f32>(cell), vec2<f32>(0.0), vec2<f32>(1.0));
  let a = textureLoad(historyTex, cell, 0);
  let b = textureLoad(historyTex, cell + vec2<i32>(1, 0), 0);
  let c = textureLoad(historyTex, cell + vec2<i32>(0, 1), 0);
  let d = textureLoad(historyTex, cell + vec2<i32>(1, 1), 0);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let q = vec2<i32>(floor(pos.xy));
  let tile = q / 2;
  let own = vec2<i32>(S.pattern.xy);
  let near = slabTrace(tile);
  let history = slabHistory(pos.xy);
  if (history.w < 0.0) { return near; }
  // held to the range of the traces round it, so a moving cloud leaves no
  // trail
  let e = slabTrace(tile + vec2<i32>(1, 0));
  let g = slabTrace(tile + vec2<i32>(0, 1));
  let h = slabTrace(tile - vec2<i32>(1, 0));
  let k = slabTrace(tile - vec2<i32>(0, 1));
  let low = min(min(min(near, e), min(g, h)), k);
  let high = max(max(max(near, e), max(g, h)), k);
  let held = clamp(history, low, high);
  // a pixel traced this frame adds its new ray to the history, so the march's
  // jitter averages out over the frames
  if (q.x - tile.x * 2 == own.x && q.y - tile.y * 2 == own.y) {
    return mix(held, near, 0.35);
  }
  return held;
}
