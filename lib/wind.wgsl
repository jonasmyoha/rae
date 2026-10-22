// The wind on the GPU (lib/Wind.rae makes its vector each frame): `wind` is
// (direction times strength xy, gustiness, time). The base is global; the
// gusts are a smooth field over the world, tens of metres across, travelling
// with the wind, so a gust front sweeps across everything that sways. A
// leaf part sways by its flexibility (its record's [43]: 0 rigid;
// app3d/PropField's PropPart `sway`); a trunk never bends. Composed before
// the vertex stages that read it (the static G-buffer and forward passes,
// the static shadow pass).

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

// The offset of a swaying leaf vertex at `world`, its prop's foot at
// `foot`, its flexibility `sway` (0: none), in the wind `wind`. Only leaves
// sway: the prop's body (a trunk, its branches) gets sway 0 and stays still,
// so nothing bends as a whole. The leaves move a few centimetres about where
// they grow: a slow sway in clusters a metre or so across, and a faster
// flutter per leaf, both more in a gust and more high in the crown.
fn windOffset(world: vec3<f32>, foot: vec3<f32>, sway: f32, wind: vec4<f32>) -> vec3<f32> {
  let strength = length(wind.xy);
  if (sway <= 0.0 || strength <= 0.0) {
    return vec3<f32>(0.0);
  }
  let dir = wind.xy / strength;
  let h = max(world.z - foot.z, 0.0);
  // A gust pushes harder: up to (1 + 2 * gustiness) times the base
  let push = strength * (1.0 + 2.0 * wind.z * windGustAt(foot.xy, wind));
  let reach = sway * push * (h / (h + 2.0));
  // The slow sway: neighbouring leaves together, other clusters at their
  // own phase
  let cluster = dot(world, vec3<f32>(0.9, 0.7, 0.5));
  let slow = 0.05 * reach * sin(wind.w * 1.7 + cluster);
  // The flutter: fast, per leaf
  let leaf = dot(world, vec3<f32>(3.1, 2.3, 2.7));
  let flutter = 0.02 * reach * sin(wind.w * 7.0 + leaf);
  let across = vec2<f32>(-dir.y, dir.x) * 0.03 * reach * sin(wind.w * 1.3 + cluster * 1.4);
  return vec3<f32>(dir * (slow + flutter) + across, (slow * 0.3 + flutter) * 0.5);
}
