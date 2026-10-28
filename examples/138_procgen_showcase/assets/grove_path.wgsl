// Authored grove paths, shared by ground paint and grass rejection.
fn grovePathCenter(y: f32) -> f32 {
  return sin(y * 0.27) * 1.8 + 0.8;
}

// 1 on the path, fading to 0 just past its edge
fn raeBiomePath(p: vec2<f32>) -> f32 {
  if (p.x >= -34.0 && p.x <= 0.0) {
    let fraction = -p.x / 34.0;
    let center = -9.5 + 13.5 * fraction - 5.0 * sin(fraction * 3.14159265);
    if (abs(p.y - center) < 0.8) { return 1.0; }
  }
  if (p.y < -10.4 || p.y > 13.6) { return 0.0; }
  return 1.0 - smoothstep(0.95, 1.15, abs(p.x - grovePathCenter(p.y)));
}
