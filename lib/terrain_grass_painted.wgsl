// Same world-space color field as the app's tufts. No camera-dependent paint.
// Compose grass_field.wgsl and world_biome.wgsl first; supply the app's patch
// constants. The grass is the frame's ground palette (app3d/GroundPalette:
// terrain_splat.wgsl reads it from the draw into raeGroundGrass, raeGroundDry
// and raeGroundDryAmount), mixed toward its dry grass where the soil is dry.
fn terrainGrassBase() -> vec3<f32> {
  return raeGroundGrass;
}
fn terrainGrassColor(position: vec2<f32>, variation: f32) -> vec3<f32> {
  var base = raeGroundGrass;
  if (raeGroundDryAmount > 0.0) {
    base = groundPaletteGrass(raeGroundGrass, raeGroundDry, raeGroundDryAmount, raeBiomeMoisture(position));
  }
  return grassGroundColor(base, position, RAE_GRASS_PATCH_SCALE, RAE_GRASS_PATCH_COVERAGE);
}
