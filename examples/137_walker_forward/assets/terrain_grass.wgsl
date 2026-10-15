// Painted meadow: broad hue patches, then soft elongated leaf-like mottling.
// All coordinates are world-space, so streaming and camera movement cannot
// slide the paint. Only three extra noise octaves; no texture allocation.
fn terrainGrassColor(position: vec2<f32>, variation: f32) -> vec3<f32> {
  let broad = clamp(0.5 + 0.9 * raeNoiseFbm2(position * 0.10, 2u, 2.0, 0.5, 821u), 0.0, 1.0);
  let rotated = vec2<f32>(position.x * 0.8 + position.y * 0.6,
                         position.y * 0.8 - position.x * 0.6);
  let leaves = smoothstep(0.25, 0.75,
    0.5 + 0.75 * raeNoiseFbm2(rotated * vec2<f32>(0.55, 1.15), 1u, 2.0, 0.5, 937u));
  let coolGreen = RAE_TERRAIN_GRASS * vec3<f32>(0.72, 0.86, 1.30);
  let warmGreen = RAE_TERRAIN_GRASS * vec3<f32>(1.35, 1.08, 0.78);
  let patches = mix(coolGreen, warmGreen, broad);
  return patches * (0.94 + 0.12 * leaves) * (0.98 + 0.04 * variation);
}
