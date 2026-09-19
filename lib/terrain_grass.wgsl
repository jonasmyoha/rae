// Default grass material. An app supplies an alternative by listing its own
// part in the terrain composition it hands to rendererSetTerrainShader (in
// place of this file). Return linear albedo;
// position is world XY in metres, variation is the shared detail value [0, 1].
fn terrainGrassColor(position: vec2<f32>, variation: f32) -> vec3<f32> {
  return raeTerrainVary(RAE_TERRAIN_GRASS, RAE_TERRAIN_VAR_GRASS, variation);
}
