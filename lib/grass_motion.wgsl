// Shared rooted response to the app WindVector; compose grass_field first.
fn grassGustEnvelope(base: vec2<f32>, time: f32, direction: vec2<f32>, strength: f32) -> f32 {
  let traveled = base - direction * time * (0.8 + strength * 0.35);
  let broad = perlin2(traveled * 0.18, 1301u);
  let local = perlin2(traveled * 0.43 + vec2<f32>(11.3, -7.1), 1307u);
  return smoothstep(-0.38, 0.43, broad * 0.72 + local * 0.28);
}
fn grassWindOffset(base: vec2<f32>, height: f32, time: f32, wind: vec3<f32>) -> vec2<f32> {
  let strength = length(wind.xy);
  if (strength <= 0.0001 || height <= 0.0) { return vec2<f32>(0.0); }
  let direction = wind.xy / strength;
  let acrossDirection = vec2<f32>(-direction.y, direction.x);
  let gustiness = clamp(wind.z, 0.0, 1.5);
  let gust = grassGustEnvelope(base, time, direction, strength);
  let rooted = height * height;
  let body = strength * (0.025 + gustiness * (0.025 + gust * 0.14)) * rooted;
  // The only quick motion is a small cross-wind tip flutter. Its world-space
  // phase differs between blades; the lower ribbon stays attached.
  let tipPhase = dot(base, vec2<f32>(1.73, -1.19)) + time * (3.1 + strength * 0.45);
  let tip = strength * (0.007 + gustiness * 0.013) * sin(tipPhase) * rooted * rooted;
  return direction * body + acrossDirection * tip;
}
