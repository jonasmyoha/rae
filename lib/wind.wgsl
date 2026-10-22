// The wind on the GPU (lib/Wind.rae makes its vector each frame): `wind` is
// (direction times strength xy, gustiness, time). The base is global; the
// gusts are a smooth field over the world, tens of metres across, travelling
// with the wind, so a gust front sweeps across everything that sways. A prop
// sways by its flexibility (its record's [43]: 0 rigid; app3d/PropField's
// kind `sway`), bending more the higher a vertex stands over its foot, with
// a small, faster flutter on top. Composed before the vertex stages that
// read it (the static G-buffer and forward passes, the static shadow pass).

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

// The offset of a swaying prop's vertex at `world`, its foot at `foot`, its
// flexibility `sway` (0: none), in the wind `wind`
fn windOffset(world: vec3<f32>, foot: vec3<f32>, sway: f32, wind: vec4<f32>) -> vec3<f32> {
  let strength = length(wind.xy);
  if (sway <= 0.0 || strength <= 0.0) {
    return vec3<f32>(0.0);
  }
  let dir = wind.xy / strength;
  let h = max(world.z - foot.z, 0.0);
  // A gust pushes harder: up to (1 + 2 * gustiness) times the base
  let push = strength * (1.0 + 2.0 * wind.z * windGustAt(foot.xy, wind));
  // Each prop sways at its own phase and a little of its own rate
  let seed = windHash(floor(foot.xy * 3.0));
  let rate = 1.0 + 0.5 * seed;
  let sway1 = 0.65 + 0.35 * sin(wind.w * rate + seed * 6.2831853);
  // The lean grows with the height, a tall trunk's top no more than a short
  // one's in proportion (min(h, 7))
  let lean = 0.0045 * sway * push * sway1 * h * min(h, 7.0);
  // Across the wind a little, so a tree circles rather than nods
  let across = vec2<f32>(-dir.y, dir.x) * lean * 0.25 * sin(wind.w * rate * 0.7 + seed * 11.0);
  // The leaves' flutter: faster and per vertex, growing with the height
  let flutter = 0.025 * sway * push * (h / (h + 3.0)) * sin(wind.w * 6.5 + dot(world, vec3<f32>(1.7, 2.3, 1.1)));
  return vec3<f32>(dir * (lean + flutter) + across, flutter * 0.4);
}
