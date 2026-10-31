// The wind on the GPU (lib/Wind.rae makes its vector each frame): `wind` is
// (direction times strength xy, gustiness, time). The base is global; the
// gusts are a smooth field over the world, tens of meters across, traveling
// with the wind, so a gust front sweeps across everything that sways. A
// leaf patch rotates by its flexibility (record [43] times pivot.w);
// geometry without attachment metadata stays rigid. Composed before
// the vertex stages that read it (the static G-buffer and forward passes,
// the static shadow pass).

// The tree and bush leaf motion (windLeaf), switched off on 2026-10-22
// because it looked glitchy (a gust field that jumped, see windGustAt) and
// back on 2026-10-23; since 2026-10-31 it takes the gusts again, from the
// drift-carried field below. The callers multiply a leaf's flexibility by
// this; 0.0 stills every leaf. Grass reads the same field once per blade
// (grass_render.wgsl).
const windLeafScale: f32 = 1.0;

fn windHash(p: vec2<f32>) -> f32 {
  return fract(sin(dot(p, vec2<f32>(127.1, 311.7))) * 43758.5453);
}

// Smooth value noise, 0..1
fn windNoise(p: vec2<f32>) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let u = f * f * (3.0 - 2.0 * f);
  let a = windHash(i);
  let b = windHash(i + vec2<f32>(1.0, 0.0));
  let c = windHash(i + vec2<f32>(0.0, 1.0));
  let d = windHash(i + vec2<f32>(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// The gust fronts (lib/WindGust.rae, docs/leaf-particles-design.md §2.3):
// 0 between gusts .. 1 at the peak of a storm gust at world `xy` in `wind`,
// the field the air carries: sampled at the point minus the wind's drift
// `drift` (m; lib/Wind WindState.driftX, the integral of its velocity), so
// it travels downwind and a turning wind never jumps it. An integer hash on
// cells wrapped to 256 and only multiplies and adds, in the same order as
// the Rae: MUST match lib/WindGust.rae windGustAt (checks/windGust).
fn windGustPcg(value: u32) -> u32 {
  let state = value * 747796405u + 2891336453u;
  let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
fn windGustHash(column: i32, row: i32, seed: u32) -> f32 {
  let inner = windGustPcg(u32(row & 255) + seed * 256u);
  let mixed = windGustPcg(u32(column & 255) + inner);
  return f32(mixed & 16777215u) * 0.000000059604645;
}
fn windGustSmooth(low: f32, scale: f32, value: f32) -> f32 {
  let t = clamp((value - low) * scale, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
fn windGustNoise(p: vec2<f32>, seed: u32) -> f32 {
  let cell = floor(p);
  let f = p - cell;
  let u = f * f * (3.0 - 2.0 * f);
  let column = i32(cell.x);
  let row = i32(cell.y);
  let a = windGustHash(column, row, seed);
  let b = windGustHash(column + 1, row, seed);
  let c = windGustHash(column, row + 1, seed);
  let d = windGustHash(column + 1, row + 1, seed);
  let bottom = a + (b - a) * u.x;
  let top = c + (d - c) * u.x;
  return bottom + (top - bottom) * u.y;
}
fn windGustAt(xy: vec2<f32>, wind: vec4<f32>, drift: vec2<f32>) -> f32 {
  let strength = sqrt(wind.x * wind.x + wind.y * wind.y);
  let air = xy - drift;
  let blend = 0.5 + 0.5 * sin(wind.w * 0.05235988);
  let first = windGustNoise(air * 0.020833334, 1u);
  let second = windGustNoise(air * 0.020833334, 2u);
  let fine = windGustNoise(air * 0.041666668, 3u);
  let field = 0.65 * (first + (second - first) * blend) + 0.35 * fine;
  let ordinary = windGustSmooth(0.42, 2.631579, field) * windGustSmooth(0.15, 2.857143, strength);
  let threshold = 0.88 - 0.05 * clamp(strength - 1.0, 0.0, 2.0);
  let storm = windGustSmooth(threshold, 12.5, windGustNoise(air * 0.010416667, 4u))
            * windGustSmooth(0.8, 1.25, strength);
  return max(0.65 * ordinary, storm);
}

// A leaf patch rotates as one rigid shape. The pivot/phase is shared by all
// its vertices; no position-dependent wave can stretch its surface.
struct WindLeaf { position: vec3<f32>, normal: vec3<f32> };
fn windRotate(value: vec3<f32>, axis: vec3<f32>, angle: f32) -> vec3<f32> {
  let cosine = cos(angle);
  return value * cosine + cross(axis, value) * sin(angle)
       + axis * dot(axis, value) * (1.0 - cosine);
}
// `gust` is windGustAt at the patch's pivot (the caller computes it once for
// this frame's and last frame's pose).
fn windLeaf(world: vec3<f32>, normal: vec3<f32>, pivot: vec3<f32>, foot: vec3<f32>,
            flexibility: f32, wind: vec4<f32>, gust: f32) -> WindLeaf {
  var result: WindLeaf;
  result.position = world;
  result.normal = normal;
  let strength = length(wind.xy);
  if (flexibility == 0.0 || strength <= 0.0001) { return result; }
  let direction = wind.xy / strength;
  if (flexibility < 0.0) {
    // Broad foliage groups respond slowly and together. Phase is fixed in
    // world space; changing wind direction cannot jump an advected noise field.
    let treePhase = windHash(foot.xy) * 6.2831853;
    let branchPhase = windHash(pivot.xy + vec2<f32>(pivot.z)) * 6.2831853;
    // a passing gust swings the sprays wider and leans them downwind
    let activity = (0.6 + 0.25 * clamp(wind.z, 0.0, 1.0)) * (1.0 + 1.2 * gust);
    let swell = 0.14 * sin(wind.w * 0.95 + treePhase)
              + 0.06 * sin(wind.w * 1.47 + treePhase * 1.7);
    let branch = 0.065 * sin(wind.w * 1.8 + branchPhase)
               + 0.025 * sin(wind.w * 3.1 + branchPhase * 1.3);
    let angle = clamp(min(strength, 2.0) * abs(flexibility)
                    * (0.065 + 0.12 * gust + activity * (swell + branch)), -0.42, 0.42);
    let axis = vec3<f32>(-direction.y, direction.x, 0.0);
    result.position = pivot + windRotate(world - pivot, axis, angle);
    result.normal = windRotate(normal, axis, angle);
    return result;
  }
  let phase = windHash(pivot.xy + vec2<f32>(pivot.z * 0.71, pivot.z * 0.37)) * 6.2831853;
  // The gust field (it travels with the air's drift, so it never jumps):
  // about the old steady 0.5 on average, more as a gust crosses the crown
  let push = 0.3 + gust;
  // Turning leaf patches, not bobbing crowns: in a steady breeze (strength
  // 1) a patch turns about 6 degrees each way, in a gust about 15, and a
  // gust also leans it downwind a little. Smaller angles (under 1 degree
  // between gusts) moved a leaf a few millimeters and could not be seen.
  let activity = 0.45 + 0.55 * clamp(wind.z, 0.0, 1.0) * push;
  let response = sin(wind.w * 2.1 + phase) * 0.5
               + sin(wind.w * 3.7 + phase * 1.43) * 0.3
               + sin(wind.w * 6.3 + phase * 2.17) * 0.2;
  let angle = clamp(flexibility * min(strength, 2.0) * (0.08 * push + 0.22 * activity * response), -0.35, 0.35);
  let axis = normalize(vec3<f32>(-direction.y, direction.x, 0.25 * sin(phase)));
  result.position = pivot + windRotate(world - pivot, axis, angle);
  result.normal = windRotate(normal, axis, angle);
  return result;
}

// Camera-facing cluster geometry, independent of its lighting normal.
// Rest corners are in local X/Z; the center and ordinary UVs stay explicit.
fn leafCardOffset(position: vec3<f32>, center: vec3<f32>, scale: vec2<f32>,
                  right: vec3<f32>, up: vec3<f32>) -> vec3<f32> {
  let offset = position - center;
  return right * (offset.x * scale.x) + up * (offset.z * scale.y);
}
// Leaf loss (docs/seasons-and-weather-design.md §3.2): does a leaf card stay
// on its tree at `loss` (0 all leaves, 1 bare)? Each card gets a key from a
// hash of its center (model space) and the tree's place, biased so the cards
// far from their leaf mass's anchor `pivot` (the mass's outside) fall first.
// app3d/PropSeason propLeafCardKey is the same key in Rae (the cases count it).
const leafLossReach: f32 = 2.0;
fn leafCardKept(center: vec3<f32>, pivot: vec3<f32>, place: vec3<f32>, loss: f32) -> bool {
  if (loss <= 0.0) { return true; }
  if (loss >= 1.0) { return false; }
  let seed = center + fract(place * 0.0137) * 101.0;
  let random = fract(sin(dot(seed, vec3<f32>(12.9898, 78.233, 37.719))) * 43758.5453);
  let outer = clamp(length(center - pivot) / leafLossReach, 0.0, 1.0);
  return 0.7 * random + 0.3 * (1.0 - outer) >= loss;
}
fn leafCameraRight(viewProj: mat4x4<f32>) -> vec3<f32> {
  return normalize(vec3<f32>(viewProj[0].x, viewProj[1].x, viewProj[2].x));
}
fn leafCameraUp(viewProj: mat4x4<f32>) -> vec3<f32> {
  return normalize(vec3<f32>(viewProj[0].y, viewProj[1].y, viewProj[2].y));
}
