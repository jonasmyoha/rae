// Same world-space color field as the app's tufts. No camera-dependent paint.
// Compose grass_field.wgsl and world_biome.wgsl first; supply the app's patch
// constants. The grass is the frame's ground palette (app3d/GroundPalette:
// terrain_splat.wgsl reads it from the draw into raeGroundGrass, raeGroundDry
// and raeGroundDryAmount), mixed toward its dry grass where the soil is dry.
//
// Under the painted grass (GrassShape.painted; RAE_GRASS_PAINTED_GROUND 1, the
// default) the ground's grass is the painted grass's OWN color at its root
// (grass_field.wgsl grassPaintAt, grassPaintRootValue, grassPaintColor), lifted
// and warmed and shimmering as the blades' roots are (raeGroundPaintLift,
// raeGroundPaintWarmth, raeGroundPaintShimmer: the frame's values, which the app
// sets from its grass, Gbuffer setGbufferGrassRootLift / Warmth / Shimmer), so
// the ground between and past the blades is their color at every distance, as
// a static ground painted as grass is (gbuffer_static_surface.wgsl). 0: the
// meadow's patch noise (grassGroundColor), for the meadow grass.
override RAE_GRASS_PAINTED_GROUND: f32 = 1.0;
fn terrainGrassBase() -> vec3<f32> {
  return raeGroundGrass;
}
fn terrainGrassColor(position: vec2<f32>, variation: f32) -> vec3<f32> {
  var base = raeGroundGrass;
  if (raeGroundDryAmount > 0.0) {
    base = groundPaletteGrass(raeGroundGrass, raeGroundDry, raeGroundDryAmount, raeBiomeMoisture(position));
  }
  if (RAE_GRASS_PAINTED_GROUND > 0.5) {
    let paint = grassPaintAt(position);
    return grassPaintColor(grassPaintRootValue(paint) + raeGroundPaintLift + raeGroundPaintShimmer, paint.y,
      base, 1.0, raeGroundPaintWarmth);
  }
  return grassGroundColor(base, position, RAE_GRASS_PATCH_SCALE, RAE_GRASS_PATCH_COVERAGE);
}
// The wind's shimmer on the ground painted as grass (terrain_splat.wgsl), as over
// the painted blades' roots and a static ground painted as grass: the gust here
// (lib/wind.wgsl windGustAt, composed before) times the frame's strength
fn terrainGrassShimmer(position: vec2<f32>, wind: vec4<f32>, drift: vec2<f32>, strength: f32) -> f32 {
  if (strength <= 0.0 || RAE_GRASS_PAINTED_GROUND < 0.5) { return 0.0; }
  return grassPaintShimmer(windGustAt(position, wind, drift)) * strength;
}
