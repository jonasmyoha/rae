// Uses `raeBiomeSample`, which only a world_biome part defines — the part
// this composition forgot. Line 4 is the reference naga must point at.
fn lighting(p: vec2<f32>) -> f32 {
  return raeBiomeSample(p) + helper(p);
}
