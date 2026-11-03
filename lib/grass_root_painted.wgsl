// The color a grass blade's root takes where it stands (grass_compute.wgsl
// grassRootColor) on the PAINTED ground (terrain_detail_painted.wgsl, the
// walkers'): the terrain's own color at that point, from the same function
// the terrain draws with, so the root is the ground under it, also where
// the meadow is partly sand, mud or rock (a blade grows where the ground is
// at least 70 % grass) and not the pure grass green. Compose after
// world_biome.wgsl, grass_field.wgsl, the app's terrain palette,
// terrain_grass_painted.wgsl and terrain_detail_painted.wgsl, in place of
// grass_root_field.wgsl.
// The frame's ground palette, which terrain_splat.wgsl reads from its draw
// and terrain_grass_painted.wgsl uses: here from the grass uniform
var<private> raeGroundGrass: vec3<f32>;
var<private> raeGroundDry: vec3<f32>;
var<private> raeGroundDryAmount: f32;
fn grassRootColor(position: vec2<f32>, biome: RaeBiome) -> vec3<f32> {
  raeGroundGrass = G.e.rgb;
  raeGroundDry = G.j.rgb;
  raeGroundDryAmount = G.j.w;
  return raeTerrainDetailColor(position, vec3<f32>(biome.elevation, biome.moisture, biome.slope), 0.0);
}
