// The wind on the GPU (lib/Wind.rae makes its vector each frame): `wind` is
// (direction times strength xy, gustiness, time). The base is global; the
// gusts are a smooth field over the world, tens of meters across, traveling
// with the wind, so a gust front sweeps across everything that sways. A
// leaf patch rotates by its flexibility (record [43] times pivot.w);
// geometry without attachment metadata stays rigid. Composed before
// the vertex stages that read it (the static G-buffer and forward passes,
// the static shadow pass).

// The tree and bush leaf motion (windLeaf), switched off on 2026-10-22
// because it looked glitchy and back on 2026-10-23 WITHOUT the gust field:
// the callers multiply a leaf's flexibility by this; 0.0 stills every leaf.
// Grass reads the same WindVector but builds a cheaper rooted gust envelope in
// grass_render.wgsl, since this noise function would run once per grass vertex.
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

// How gusty it is at world point `xy`: 0 between gusts, up to 1 in one
const windGustCell: f32 = 55.0;

fn windGustAt(xy: vec2<f32>, wind: vec4<f32>) -> f32 {
  let strength = length(wind.xy);
  let dir = wind.xy / max(strength, 0.0001);
  // The fronts travel with the wind, faster when it is stronger
  let travel = dir * wind.w * (2.5 + 3.5 * strength);
  let p = (xy - travel) / windGustCell;
  let n = windNoise(p) * 0.65 + windNoise(p * 2.3 + vec2<f32>(7.1, 3.4)) * 0.35;
  return smoothstep(0.42, 0.85, n);
}

// A leaf patch rotates as one rigid shape. The pivot/phase is shared by all
// its vertices; no position-dependent wave can stretch its surface.
struct WindLeaf { position: vec3<f32>, normal: vec3<f32> };
fn windRotate(value: vec3<f32>, axis: vec3<f32>, angle: f32) -> vec3<f32> {
  let cosine = cos(angle);
  return value * cosine + cross(axis, value) * sin(angle)
       + axis * dot(axis, value) * (1.0 - cosine);
}
fn windLeaf(world: vec3<f32>, normal: vec3<f32>, pivot: vec3<f32>, foot: vec3<f32>,
            flexibility: f32, wind: vec4<f32>) -> WindLeaf {
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
    let activity = 0.75 + 0.25 * clamp(wind.z, 0.0, 1.0);
    let swell = 0.14 * sin(wind.w * 0.95 + treePhase)
              + 0.06 * sin(wind.w * 1.47 + treePhase * 1.7);
    let branch = 0.065 * sin(wind.w * 1.8 + branchPhase)
               + 0.025 * sin(wind.w * 3.1 + branchPhase * 1.3);
    let angle = clamp(min(strength, 2.0) * abs(flexibility)
                    * (0.065 + activity * (swell + branch)), -0.42, 0.42);
    let axis = vec3<f32>(-direction.y, direction.x, 0.0);
    result.position = pivot + windRotate(world - pivot, axis, angle);
    result.normal = windRotate(normal, axis, angle);
    return result;
  }
  let phase = windHash(pivot.xy + vec2<f32>(pivot.z * 0.71, pivot.z * 0.37)) * 6.2831853;
  // A steady gust level, not windGustAt: that field moves by the drifting
  // direction times the total time and jumps between frames, which made
  // the leaves (and the grass) twitch (docs/wind-design.md §5)
  let gust = 0.5;
  // Turning leaf patches, not bobbing crowns: in a steady breeze (strength
  // 1) a patch turns about 6 degrees each way, in a gust about 15, and a
  // gust also leans it downwind a little. Smaller angles (under 1 degree
  // between gusts) moved a leaf a few millimeters and could not be seen.
  let activity = 0.45 + 0.55 * clamp(wind.z, 0.0, 1.0) * gust;
  let response = sin(wind.w * 2.1 + phase) * 0.5
               + sin(wind.w * 3.7 + phase * 1.43) * 0.3
               + sin(wind.w * 6.3 + phase * 2.17) * 0.2;
  let angle = clamp(flexibility * min(strength, 2.0) * (0.08 * gust + 0.22 * activity * response), -0.35, 0.35);
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
fn leafCameraRight(viewProj: mat4x4<f32>) -> vec3<f32> {
  return normalize(vec3<f32>(viewProj[0].x, viewProj[1].x, viewProj[2].x));
}
fn leafCameraUp(viewProj: mat4x4<f32>) -> vec3<f32> {
  return normalize(vec3<f32>(viewProj[0].y, viewProj[1].y, viewProj[2].y));
}
