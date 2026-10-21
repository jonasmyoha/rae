// The level cross-fade's dither (docs/tree-lod-design.md §2.3). Where two
// levels of detail of a prop overlap (app3d/PropField's fade band), both are
// drawn: the near one fading out keeps the pixels whose dither is at least
// the fade threshold, the far one fading in the pixels below it, so with the
// same threshold and the same per-prop seed every pixel shows exactly one of
// them. The instance record carries the fade (GbufferRecord [40..43]):
// x = threshold, y = 1 when fading in (0: out), z = the prop's seed; all 0
// for an instance that is not fading, which keeps every pixel.
//
// The dither is interleaved gradient noise (Jimenez, 2014): cheap, and spread
// evenly like blue noise, so a half-faded tree reads as a fine stipple, not
// as blotches. `frameOffset` moves it each frame when TAA resolves it (the
// TAA jitter; 0 without TAA, so the pattern then stays put).
fn fadeDither(pixel: vec2<f32>, seed: f32, frameOffset: vec2<f32>) -> f32 {
  let p = floor(pixel) + vec2<f32>(seed * 61.0, seed * 37.0) + frameOffset;
  return fract(52.9829189 * fract(dot(p, vec2<f32>(0.06711056, 0.00583715))));
}

// Does an instance with this fade keep the pixel at `pixel`?
fn fadeKeeps(fade: vec4<f32>, pixel: vec2<f32>, frameOffset: vec2<f32>) -> bool {
  if (fade.x <= 0.0 && fade.y < 0.5) {
    return true;
  }
  let h = fadeDither(pixel, fade.z, frameOffset);
  if (fade.y > 0.5) {
    return h < fade.x;
  }
  return h >= fade.x;
}

// The frame offset from the TAA jitter (clip-space units, 0 without TAA):
// whole pixels, so the stipple moves by a pixel pattern each frame
fn fadeFrameOffset(jitter: vec2<f32>) -> vec2<f32> {
  return floor(jitter * 8192.0);
}
