// The grass's rooted response to the app WindVector: a steady lean downwind
// and a slow, smooth sway that rolls across the field with the wind (one
// long wave), a push as a gust crosses it (`gust`: the trees' gust field at
// the blade, from the compute pass's grassGustAt; the field travels with the
// air's drift, so it never jumps, unlike the first gust noise that was taken
// out), plus a small tip flutter.
fn grassWindOffset(base: vec2<f32>, height: f32, time: f32, wind: vec3<f32>, gust: f32) -> vec2<f32> {
  let strength = length(wind.xy);
  if (strength <= 0.0001 || height <= 0.0) { return vec2<f32>(0.0); }
  let direction = wind.xy / strength;
  let acrossDirection = vec2<f32>(-direction.y, direction.x);
  let gustiness = clamp(wind.z, 0.0, 1.5);
  let rooted = height * height;
  // The sway's phase over the ground runs along one fixed way, not the
  // wind's: with the wind's own turning way, a blade far from the world's
  // middle would race through its phase as the wind turns. Its frequencies
  // are whole cycles in the wind's hour (lib/Wind windTimeWrap: 630 and 360
  // cycles), so the clock's wrap does not jump it.
  let roll = dot(base, vec2<f32>(0.8, 0.6)) * 0.21 - time * 1.0995574;
  let sway = 0.5 + 0.35 * sin(roll) + 0.15 * sin(roll * 0.5714286 + 1.3);
  let body = strength * (0.03 + gustiness * 0.035 * sway + 0.07 * gust) * rooted;
  // The only quick motion is a small cross-wind tip flutter. Its world-space
  // phase differs between blades; the lower ribbon stays attached.
  // One frequency (2,005 cycles an hour): one that grew with the wind's
  // strength would race as the strength swells, ever more the longer the clock
  let tipPhase = dot(base, vec2<f32>(1.73, -1.19)) + time * 3.4993605;
  let tip = strength * (0.007 + gustiness * 0.013) * sin(tipPhase) * rooted * rooted;
  return direction * body + acrossDirection * tip;
}
