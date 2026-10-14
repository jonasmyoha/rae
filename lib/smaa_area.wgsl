// Ported from SMAA: Copyright (c) 2013 Jorge Jimenez et al., MIT License (lib/LICENSE-smaa.md)
// SMAA's area texture (render-perf-9), generated once on the GPU instead of
// bundled: the subsample-0 band of Scripts/AreaTex.py, which is all SMAA 1x
// reads. 160 x 80 texels, RG = the areas above and below the line:
//   x 0-79:   orthogonal patterns, a 5 x 5 grid of 16 x 16 tiles (the tile is
//             the crossing-edge pair, the texel the left/right distance,
//             compressed quadratically: texel d holds distance d * d);
//   x 80-159: diagonal patterns, a 4 x 4 grid of 20 x 20 tiles.
// Values are truncated to 8 bits like the script's int(255 * a).
struct AreaU {
  p0: vec4<f32>,  // y: the subsample offset (0 for SMAA 1x)
};
@group(0) @binding(0) var<uniform> U: AreaU;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
const SMOOTH_MAX_DISTANCE: f32 = 32.0;
const SAMPLES_DIAG: i32 = 30;

// The area under the line p1 -> p2 for the pixel x .. x + 1 (AreaTex.py area()).
fn lineArea(p1: vec2<f32>, p2: vec2<f32>, x: f32) -> vec2<f32> {
  let d = p2 - p1;
  let x1 = x;
  let x2 = x + 1.0;
  let y1 = p1.y + d.y * (x1 - p1.x) / d.x;
  let y2 = p1.y + d.y * (x2 - p1.x) / d.x;
  let inside = (x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x);
  if (!inside) { return vec2<f32>(0.0); }
  let trapezoid = ((y1 < 0.0) == (y2 < 0.0)) || abs(y1) < 1e-4 || abs(y2) < 1e-4;
  if (trapezoid) {
    let a = (y1 + y2) / 2.0;
    if (a < 0.0) { return vec2<f32>(abs(a), 0.0); }
    return vec2<f32>(0.0, abs(a));
  }
  // Two triangles
  let crossing = -p1.y * d.x / d.y + p1.x;
  let fraction = crossing - trunc(crossing);
  var a1 = 0.0;
  if (crossing > p1.x) { a1 = y1 * fraction / 2.0; }
  var a2 = 0.0;
  if (crossing < p2.x) { a2 = y2 * (1.0 - fraction) / 2.0; }
  var a = -a2;
  if (abs(a1) > abs(a2)) { a = a1; }
  if (a < 0.0) { return vec2<f32>(abs(a1), abs(a2)); }
  return vec2<f32>(abs(a2), abs(a1));
}

// The smoothing of short U shapes, both halves summed (smootharea()).
fn smoothSum(d: f32, a1: vec2<f32>, a2: vec2<f32>) -> vec2<f32> {
  let b1 = sqrt(a1 * 2.0) * 0.5;
  let b2 = sqrt(a2 * 2.0) * 0.5;
  let p = clamp(d / SMOOTH_MAX_DISTANCE, 0.0, 1.0);
  return mix(b1, a1, p) + mix(b2, a2, p);
}

// areaortho() for one of the 16 patterns.
fn areaOrtho(pattern: i32, left: f32, right: f32, offset: f32) -> vec2<f32> {
  let d = left + right + 1.0;
  let o1 = 0.5 + offset;
  let o2 = 0.5 + offset - 1.0;
  let half = d / 2.0;
  switch pattern {
    case 1: {
      if (left <= right) { return lineArea(vec2<f32>(0.0, o2), vec2<f32>(half, 0.0), left); }
      return vec2<f32>(0.0);
    }
    case 2: {
      if (left >= right) { return lineArea(vec2<f32>(half, 0.0), vec2<f32>(d, o2), left); }
      return vec2<f32>(0.0);
    }
    case 3: {
      return smoothSum(d, lineArea(vec2<f32>(0.0, o2), vec2<f32>(half, 0.0), left),
                          lineArea(vec2<f32>(half, 0.0), vec2<f32>(d, o2), left));
    }
    case 4: {
      if (left <= right) { return lineArea(vec2<f32>(0.0, o1), vec2<f32>(half, 0.0), left); }
      return vec2<f32>(0.0);
    }
    case 6: {
      if (abs(offset) > 0.0) {
        let a1 = lineArea(vec2<f32>(0.0, o1), vec2<f32>(d, o2), left);
        let a2 = lineArea(vec2<f32>(0.0, o1), vec2<f32>(half, 0.0), left)
               + lineArea(vec2<f32>(half, 0.0), vec2<f32>(d, o2), left);
        return (a1 + a2) / 2.0;
      }
      return lineArea(vec2<f32>(0.0, o1), vec2<f32>(d, o2), left);
    }
    case 7: { return lineArea(vec2<f32>(0.0, o1), vec2<f32>(d, o2), left); }
    case 8: {
      if (left >= right) { return lineArea(vec2<f32>(half, 0.0), vec2<f32>(d, o1), left); }
      return vec2<f32>(0.0);
    }
    case 9: {
      if (abs(offset) > 0.0) {
        let a1 = lineArea(vec2<f32>(0.0, o2), vec2<f32>(d, o1), left);
        let a2 = lineArea(vec2<f32>(0.0, o2), vec2<f32>(half, 0.0), left)
               + lineArea(vec2<f32>(half, 0.0), vec2<f32>(d, o1), left);
        return (a1 + a2) / 2.0;
      }
      return lineArea(vec2<f32>(0.0, o2), vec2<f32>(d, o1), left);
    }
    case 11: { return lineArea(vec2<f32>(0.0, o2), vec2<f32>(d, o1), left); }
    case 12: {
      return smoothSum(d, lineArea(vec2<f32>(0.0, o1), vec2<f32>(half, 0.0), left),
                          lineArea(vec2<f32>(half, 0.0), vec2<f32>(d, o1), left));
    }
    case 13: { return lineArea(vec2<f32>(0.0, o2), vec2<f32>(d, o1), left); }
    case 14: { return lineArea(vec2<f32>(0.0, o1), vec2<f32>(d, o2), left); }
    default: { return vec2<f32>(0.0); }  // 0, 5, 10, 15: nothing to filter
  }
}

// The share of the pixel at `p` on the positive side of the line p1 -> p2,
// by 30 x 30 brute-force samples (area() inside areadiag()).
fn diagCoverage(p1: vec2<f32>, p2: vec2<f32>, p: vec2<f32>) -> f32 {
  let mid = (p1 + p2) / 2.0;
  let a = p2.y - p1.y;
  let b = p1.x - p2.x;
  var count = 0.0;
  for (var x = 0; x < SAMPLES_DIAG; x = x + 1) {
    for (var y = 0; y < SAMPLES_DIAG; y = y + 1) {
      let s = p + vec2<f32>(f32(x), f32(y)) / f32(SAMPLES_DIAG - 1);
      if (a * (s.x - mid.x) + b * (s.y - mid.y) > 0.0) { count = count + 1.0; }
    }
  }
  return count / f32(SAMPLES_DIAG * SAMPLES_DIAG);
}

// The area of one diagonal line ending at p1 and p2 + (d, d), for the pixel
// and its opposite (subsample 0: no offset).
fn diagArea(p1: vec2<f32>, p2: vec2<f32>, d: f32, left: f32) -> vec2<f32> {
  let end = p2 + vec2<f32>(d, d);
  let a1 = diagCoverage(p1, end, vec2<f32>(1.0 + left, left));
  let a2 = diagCoverage(p1, end, vec2<f32>(1.0 + left, 1.0 + left));
  return vec2<f32>(1.0 - a1, a2);
}

// areadiag(): each pattern is one line, or the blend of the two possible
// ends of a line (see the script's "Black Magic" note).
fn areaDiag(pattern: i32, left: f32, right: f32) -> vec2<f32> {
  let d = left + right + 1.0;
  let p00 = vec2<f32>(0.0, 0.0);
  let p10 = vec2<f32>(1.0, 0.0);
  let p11 = vec2<f32>(1.0, 1.0);
  switch pattern {
    case 0: { return (diagArea(p11, p11, d, left) + diagArea(p10, p10, d, left)) / 2.0; }
    case 1: { return (diagArea(p10, p00, d, left) + diagArea(p10, p10, d, left)) / 2.0; }
    case 2: { return (diagArea(p00, p10, d, left) + diagArea(p10, p10, d, left)) / 2.0; }
    case 3: { return diagArea(p10, p10, d, left); }
    case 4: { return (diagArea(p11, p00, d, left) + diagArea(p11, p10, d, left)) / 2.0; }
    case 5: { return (diagArea(p11, p00, d, left) + diagArea(p10, p10, d, left)) / 2.0; }
    case 6: { return diagArea(p11, p10, d, left); }
    case 7: { return (diagArea(p11, p10, d, left) + diagArea(p10, p10, d, left)) / 2.0; }
    case 8: { return (diagArea(p00, p11, d, left) + diagArea(p10, p11, d, left)) / 2.0; }
    case 9: { return diagArea(p10, p11, d, left); }
    case 10: { return (diagArea(p00, p11, d, left) + diagArea(p10, p10, d, left)) / 2.0; }
    case 11: { return (diagArea(p10, p11, d, left) + diagArea(p10, p10, d, left)) / 2.0; }
    case 12: { return diagArea(p11, p11, d, left); }
    case 13: { return (diagArea(p11, p11, d, left) + diagArea(p10, p11, d, left)) / 2.0; }
    case 14: { return (diagArea(p11, p11, d, left) + diagArea(p11, p10, d, left)) / 2.0; }
    default: { return (diagArea(p11, p11, d, left) + diagArea(p10, p10, d, left)) / 2.0; }  // 15
  }
}

@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let texel = vec2<i32>(pos.xy);
  var area = vec2<f32>(0.0);
  if (texel.x < 80) {
    // The tile's edge values are 0, 1, 3 or 4 (2 is unused and stays black):
    // 3 / 4 carry the "top" bit, 1 / 4 the "bottom" bit (edgesortho).
    let tile = texel / 16;
    let within = texel % 16;
    let topLeft = tile.x == 3 || tile.x == 4;
    let topRight = tile.y == 3 || tile.y == 4;
    let bottomLeft = tile.x == 1 || tile.x == 4;
    let bottomRight = tile.y == 1 || tile.y == 4;
    if (tile.x != 2 && tile.y != 2) {
      let pattern = select(0, 1, topLeft) + select(0, 2, topRight)
                  + select(0, 4, bottomLeft) + select(0, 8, bottomRight);
      let left = f32(within.x * within.x);
      let right = f32(within.y * within.y);
      area = areaOrtho(pattern, left, right, U.p0.y);
    }
  } else {
    // Edge values 0-3: bit 0 and bit 1 of each end (edgesdiag)
    let tile = (texel - vec2<i32>(80, 0)) / 20;
    let within = (texel - vec2<i32>(80, 0)) % 20;
    let pattern = (tile.x & 1) + 2 * ((tile.y >> 1u) & 1) + 4 * ((tile.x >> 1u) & 1) + 8 * (tile.y & 1);
    area = areaDiag(pattern, f32(within.x), f32(within.y));
  }
  // int(255 * a) in the script: truncate, then the unorm write is exact
  return vec4<f32>(floor(area * 255.0) / 255.0, 0.0, 1.0);
}
