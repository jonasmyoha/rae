// Same world-space colour field as the app's tufts. No camera-dependent paint.
// Compose grass_field.wgsl first; supply the app's patch constants.
fn terrainGrassColor(position: vec2<f32>, variation: f32) -> vec3<f32> {
  return grassGroundColor(RAE_TERRAIN_GRASS, position, RAE_GRASS_PATCH_SCALE, RAE_GRASS_PATCH_COVERAGE);
}
