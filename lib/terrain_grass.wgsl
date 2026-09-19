// Default grass material. Apps may supply an alternative through
// rendererSetTerrainPalette(grassStylePath: ...). Return linear albedo;
// position is world XY in metres, variation is the shared detail value [0, 1].
fn terrainGrassColor(position: vec2<f32>, variation: f32) -> vec3<f32> {
  return raeTerrainVary(RAE_TERRAIN_GRASS, RAE_TERRAIN_VAR_GRASS, variation);
}
