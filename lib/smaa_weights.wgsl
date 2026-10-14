// Ported from SMAA: Copyright (c) 2013 Jorge Jimenez et al., MIT License (lib/LICENSE-smaa.md)
// SMAA 1x, pass 2 (render-perf-9): blending weight calculation
// (SMAABlendingWeightCalculationPS with its search, area and corner
// functions), the "high" preset: 16 search steps, 8 diagonal, corner
// rounding 25. The area texture is the 160 x 80 subsample-0 band
// (smaa_area.wgsl), so SMAA 1x's zero subsample offsets never move off it.
// Offsets the reference passes as texel offsets are added as texel-size
// multiples here (the same bilinear fetch).
@group(0) @binding(0) var edgesTex: texture_2d<f32>;
@group(0) @binding(1) var areaTex: texture_2d<f32>;
@group(0) @binding(2) var searchTex: texture_2d<f32>;
@group(0) @binding(3) var linearSamp: sampler;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
const SMAA_MAX_SEARCH_STEPS: f32 = 16.0;
const SMAA_MAX_SEARCH_STEPS_DIAG: f32 = 8.0;
const SMAA_CORNER_ROUNDING_NORM: f32 = 0.25;
const SMAA_AREATEX_MAX_DISTANCE: f32 = 16.0;
const SMAA_AREATEX_MAX_DISTANCE_DIAG: f32 = 20.0;
const SMAA_AREATEX_PIXEL_SIZE: vec2<f32> = vec2<f32>(1.0 / 160.0, 1.0 / 80.0);
const SMAA_SEARCHTEX_SIZE: vec2<f32> = vec2<f32>(66.0, 33.0);
const SMAA_SEARCHTEX_PACKED_SIZE: vec2<f32> = vec2<f32>(64.0, 16.0);

// (1 / width, 1 / height, width, height) of the scene
var<private> rt: vec4<f32>;

fn edgesAt(coord: vec2<f32>) -> vec2<f32> {
  return textureSampleLevel(edgesTex, linearSamp, coord, 0.0).rg;
}
fn edgesAtOffset(coord: vec2<f32>, offset: vec2<f32>) -> vec2<f32> {
  return textureSampleLevel(edgesTex, linearSamp, coord + offset * rt.xy, 0.0).rg;
}

// ----- diagonal search ------------------------------------------------------
fn decodeDiag2(edges: vec2<f32>) -> vec2<f32> {
  var e = edges;
  e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
  return round(e);
}
fn decodeDiag4(edges: vec4<f32>) -> vec4<f32> {
  var e = edges;
  e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
  e.b = e.b * abs(5.0 * e.b - 5.0 * 0.75);
  return round(e);
}
struct DiagSearch {
  zw: vec2<f32>,
  e: vec2<f32>,
};
fn searchDiag1(texcoord: vec2<f32>, dir: vec2<f32>) -> DiagSearch {
  var coord = vec4<f32>(texcoord, -1.0, 1.0);
  let t = vec3<f32>(rt.xy, 1.0);
  var e = vec2<f32>(0.0);
  loop {
    if (!(coord.z < SMAA_MAX_SEARCH_STEPS_DIAG - 1.0 && coord.w > 0.9)) { break; }
    let xyz = t * vec3<f32>(dir, 1.0) + coord.xyz;
    coord = vec4<f32>(xyz, coord.w);
    e = edgesAt(coord.xy);
    coord.w = dot(e, vec2<f32>(0.5));
  }
  return DiagSearch(coord.zw, e);
}
fn searchDiag2(texcoord: vec2<f32>, dir: vec2<f32>) -> DiagSearch {
  var coord = vec4<f32>(texcoord, -1.0, 1.0);
  coord.x = coord.x + 0.25 * rt.x;
  let t = vec3<f32>(rt.xy, 1.0);
  var e = vec2<f32>(0.0);
  loop {
    if (!(coord.z < SMAA_MAX_SEARCH_STEPS_DIAG - 1.0 && coord.w > 0.9)) { break; }
    let xyz = t * vec3<f32>(dir, 1.0) + coord.xyz;
    coord = vec4<f32>(xyz, coord.w);
    e = decodeDiag2(edgesAt(coord.xy));
    coord.w = dot(e, vec2<f32>(0.5));
  }
  return DiagSearch(coord.zw, e);
}
fn areaDiag(dist: vec2<f32>, e: vec2<f32>) -> vec2<f32> {
  var texcoord = vec2<f32>(SMAA_AREATEX_MAX_DISTANCE_DIAG) * e + dist;
  texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
  texcoord.x = texcoord.x + 0.5;
  return textureSampleLevel(areaTex, linearSamp, texcoord, 0.0).rg;
}
fn calculateDiagWeights(texcoord: vec2<f32>, e: vec2<f32>) -> vec2<f32> {
  var weights = vec2<f32>(0.0);
  var d = vec4<f32>(0.0);
  if (e.r > 0.0) {
    let s = searchDiag1(texcoord, vec2<f32>(-1.0, 1.0));
    d.x = s.zw.x + f32(s.e.y > 0.9);
    d.z = s.zw.y;
  }
  let s1 = searchDiag1(texcoord, vec2<f32>(1.0, -1.0));
  d.y = s1.zw.x;
  d.w = s1.zw.y;
  if (d.x + d.y > 2.0) {
    let coords = vec4<f32>(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * rt.xyxy + texcoord.xyxy;
    let raw = vec4<f32>(edgesAtOffset(coords.xy, vec2<f32>(-1.0, 0.0)), edgesAtOffset(coords.zw, vec2<f32>(1.0, 0.0)));
    let decoded = decodeDiag4(raw);
    // c.yxwz = decoded
    let c = vec4<f32>(decoded.y, decoded.x, decoded.w, decoded.z);
    var cc = vec2<f32>(2.0) * c.xz + c.yw;
    if (d.z >= 0.9) { cc.x = 0.0; }
    if (d.w >= 0.9) { cc.y = 0.0; }
    weights = weights + areaDiag(d.xy, cc);
  }
  let s2 = searchDiag2(texcoord, vec2<f32>(-1.0, -1.0));
  d.x = s2.zw.x;
  d.z = s2.zw.y;
  if (edgesAtOffset(texcoord, vec2<f32>(1.0, 0.0)).r > 0.0) {
    let s3 = searchDiag2(texcoord, vec2<f32>(1.0, 1.0));
    d.y = s3.zw.x + f32(s3.e.y > 0.9);
    d.w = s3.zw.y;
  } else {
    d.y = 0.0;
    d.w = 0.0;
  }
  if (d.x + d.y > 2.0) {
    let coords = vec4<f32>(-d.x, -d.x, d.y, d.y) * rt.xyxy + texcoord.xyxy;
    var c: vec4<f32>;
    c.x = edgesAtOffset(coords.xy, vec2<f32>(-1.0, 0.0)).g;
    c.y = edgesAtOffset(coords.xy, vec2<f32>(0.0, -1.0)).r;
    let zw = edgesAtOffset(coords.zw, vec2<f32>(1.0, 0.0)).gr;
    c.z = zw.x;
    c.w = zw.y;
    var cc = vec2<f32>(2.0) * c.xz + c.yw;
    if (d.z >= 0.9) { cc.x = 0.0; }
    if (d.w >= 0.9) { cc.y = 0.0; }
    weights = weights + areaDiag(d.xy, cc).gr;
  }
  return weights;
}

// ----- horizontal / vertical search -----------------------------------------
fn searchLength(e: vec2<f32>, offset: f32) -> f32 {
  var scale = SMAA_SEARCHTEX_SIZE * vec2<f32>(0.5, -1.0);
  var bias = SMAA_SEARCHTEX_SIZE * vec2<f32>(offset, 1.0);
  scale = scale + vec2<f32>(-1.0, 1.0);
  bias = bias + vec2<f32>(0.5, -0.5);
  scale = scale / SMAA_SEARCHTEX_PACKED_SIZE;
  bias = bias / SMAA_SEARCHTEX_PACKED_SIZE;
  return textureSampleLevel(searchTex, linearSamp, scale * e + bias, 0.0).r;
}
fn searchXLeft(start: vec2<f32>, end: f32) -> f32 {
  var texcoord = start;
  var e = vec2<f32>(0.0, 1.0);
  loop {
    if (!(texcoord.x > end && e.g > 0.8281 && e.r == 0.0)) { break; }
    e = edgesAt(texcoord);
    texcoord = -vec2<f32>(2.0, 0.0) * rt.xy + texcoord;
  }
  let offset = -(255.0 / 127.0) * searchLength(e, 0.0) + 3.25;
  return rt.x * offset + texcoord.x;
}
fn searchXRight(start: vec2<f32>, end: f32) -> f32 {
  var texcoord = start;
  var e = vec2<f32>(0.0, 1.0);
  loop {
    if (!(texcoord.x < end && e.g > 0.8281 && e.r == 0.0)) { break; }
    e = edgesAt(texcoord);
    texcoord = vec2<f32>(2.0, 0.0) * rt.xy + texcoord;
  }
  let offset = -(255.0 / 127.0) * searchLength(e, 0.5) + 3.25;
  return -rt.x * offset + texcoord.x;
}
fn searchYUp(start: vec2<f32>, end: f32) -> f32 {
  var texcoord = start;
  var e = vec2<f32>(1.0, 0.0);
  loop {
    if (!(texcoord.y > end && e.r > 0.8281 && e.g == 0.0)) { break; }
    e = edgesAt(texcoord);
    texcoord = -vec2<f32>(0.0, 2.0) * rt.xy + texcoord;
  }
  let offset = -(255.0 / 127.0) * searchLength(e.gr, 0.0) + 3.25;
  return rt.y * offset + texcoord.y;
}
fn searchYDown(start: vec2<f32>, end: f32) -> f32 {
  var texcoord = start;
  var e = vec2<f32>(1.0, 0.0);
  loop {
    if (!(texcoord.y < end && e.r > 0.8281 && e.g == 0.0)) { break; }
    e = edgesAt(texcoord);
    texcoord = vec2<f32>(0.0, 2.0) * rt.xy + texcoord;
  }
  let offset = -(255.0 / 127.0) * searchLength(e.gr, 0.5) + 3.25;
  return -rt.y * offset + texcoord.y;
}
fn areaOrtho(dist: vec2<f32>, e1: f32, e2: f32) -> vec2<f32> {
  // Rounding prevents precision errors of bilinear filtering
  var texcoord = vec2<f32>(SMAA_AREATEX_MAX_DISTANCE) * round(4.0 * vec2<f32>(e1, e2)) + dist;
  texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
  return textureSampleLevel(areaTex, linearSamp, texcoord, 0.0).rg;
}

// ----- corners ---------------------------------------------------------------
fn horizontalCorner(weights: vec2<f32>, texcoord: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
  let leftRight = step(d.xy, d.yx);
  var rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
  rounding = rounding / (leftRight.x + leftRight.y);
  var factor = vec2<f32>(1.0);
  factor.x = factor.x - rounding.x * edgesAtOffset(texcoord.xy, vec2<f32>(0.0, 1.0)).r;
  factor.x = factor.x - rounding.y * edgesAtOffset(texcoord.zw, vec2<f32>(1.0, 1.0)).r;
  factor.y = factor.y - rounding.x * edgesAtOffset(texcoord.xy, vec2<f32>(0.0, -2.0)).r;
  factor.y = factor.y - rounding.y * edgesAtOffset(texcoord.zw, vec2<f32>(1.0, -2.0)).r;
  return weights * clamp(factor, vec2<f32>(0.0), vec2<f32>(1.0));
}
fn verticalCorner(weights: vec2<f32>, texcoord: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
  let leftRight = step(d.xy, d.yx);
  var rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
  rounding = rounding / (leftRight.x + leftRight.y);
  var factor = vec2<f32>(1.0);
  factor.x = factor.x - rounding.x * edgesAtOffset(texcoord.xy, vec2<f32>(1.0, 0.0)).g;
  factor.x = factor.x - rounding.y * edgesAtOffset(texcoord.zw, vec2<f32>(1.0, 1.0)).g;
  factor.y = factor.y - rounding.x * edgesAtOffset(texcoord.xy, vec2<f32>(-2.0, 0.0)).g;
  factor.y = factor.y - rounding.y * edgesAtOffset(texcoord.zw, vec2<f32>(-2.0, 1.0)).g;
  return weights * clamp(factor, vec2<f32>(0.0), vec2<f32>(1.0));
}

@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = vec2<f32>(textureDimensions(edgesTex));
  rt = vec4<f32>(vec2<f32>(1.0) / dims, dims);
  let texcoord = pos.xy * rt.xy;
  let pixcoord = pos.xy;
  let offset0 = rt.xyxy * vec4<f32>(-0.25, -0.125, 1.25, -0.125) + texcoord.xyxy;
  let offset1 = rt.xyxy * vec4<f32>(-0.125, -0.25, -0.125, 1.25) + texcoord.xyxy;
  let offset2 = rt.xxyy * (vec4<f32>(-2.0, 2.0, -2.0, 2.0) * SMAA_MAX_SEARCH_STEPS)
              + vec4<f32>(offset0.xz, offset1.yw);

  var weights = vec4<f32>(0.0);
  var e = edgesAt(texcoord);
  if (e.g > 0.0) {  // edge at north
    // Diagonals first; a diagonal skips the horizontal/vertical processing
    let diag = calculateDiagWeights(texcoord, e);
    weights = vec4<f32>(diag, weights.zw);
    if (diag.r == -diag.g) {
      var coords: vec3<f32>;
      coords.x = searchXLeft(offset0.xy, offset2.x);
      coords.y = offset1.y;
      var d = vec2<f32>(coords.x, 0.0);
      let e1 = edgesAt(coords.xy).r;
      coords.z = searchXRight(offset0.zw, offset2.y);
      d.y = coords.z;
      d = abs(round(rt.zz * d - pixcoord.xx));
      let sqrtD = sqrt(d);
      let e2 = edgesAtOffset(coords.zy, vec2<f32>(1.0, 0.0)).r;
      var rg = areaOrtho(sqrtD, e1, e2);
      rg = horizontalCorner(rg, vec4<f32>(coords.x, texcoord.y, coords.z, texcoord.y), d);
      weights = vec4<f32>(rg, weights.zw);
    } else {
      e.r = 0.0;  // skip vertical processing
    }
  }
  if (e.r > 0.0) {  // edge at west
    var coords: vec3<f32>;
    coords.y = searchYUp(offset1.xy, offset2.z);
    coords.x = offset0.x;
    var d = vec2<f32>(coords.y, 0.0);
    let e1 = edgesAt(coords.xy).g;
    coords.z = searchYDown(offset1.zw, offset2.w);
    d.y = coords.z;
    d = abs(round(rt.ww * d - pixcoord.yy));
    let sqrtD = sqrt(d);
    let e2 = edgesAtOffset(coords.xz, vec2<f32>(0.0, 1.0)).g;
    var ba = areaOrtho(sqrtD, e1, e2);
    ba = verticalCorner(ba, vec4<f32>(texcoord.x, coords.y, texcoord.x, coords.z), d);
    weights = vec4<f32>(weights.xy, ba);
  }
  return weights;
}
