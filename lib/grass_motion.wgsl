// The grass's rooted response to the app WindVector: a steady lean downwind
// and a slow, smooth sway that rolls across the field with the wind (one
// long wave), a push as a gust crosses it (`gust`: the blade's
// lib/wind.wgsl windGustAt, which the grass compute pass evaluates once per
// blade; the field travels with the air's drift, so it never jumps, unlike
// the first gust noise that was taken out), plus a small tip flutter.
fn grassWindOffset(base: vec2<f32>, height: f32, time: f32, wind: vec3<f32>, gust: f32) -> vec2<f32> {
  let strength = length(wind.xy);
  if (strength <= 0.0001 || height <= 0.0) { return vec2<f32>(0.0); }
  let direction = wind.xy / strength;
  let acrossDirection = vec2<f32>(-direction.y, direction.x);
  let gustiness = clamp(wind.z, 0.0, 1.5);
  let rooted = height * height;
  let roll = dot(base, direction) * 0.21 - time * 1.1;
  let sway = 0.5 + 0.35 * sin(roll) + 0.15 * sin(roll * 0.57 + 1.3);
  let body = strength * (0.03 + gustiness * 0.035 * sway + 0.07 * gust) * rooted;
  // The only quick motion is a small cross-wind tip flutter. Its world-space
  // phase differs between blades; the lower ribbon stays attached.
  let tipPhase = dot(base, vec2<f32>(1.73, -1.19)) + time * (3.1 + strength * 0.45);
  let tip = strength * (0.007 + gustiness * 0.013) * sin(tipPhase) * rooted * rooted;
  return direction * body + acrossDirection * tip;
}
