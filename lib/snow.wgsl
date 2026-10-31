// Snow (docs/seasons-and-weather-design.md §4; lib/Climate's snow depth
// where the camera looks, the frame uniform's scene.x, Gbuffer
// setGbufferSnow): what the terrain, the static meshes, the GPU grass and
// the far trees read. Composed before the shader that uses it.
const snowAlbedo: vec3<f32> = vec3<f32>(0.84, 0.86, 0.9);

// How much of an up-facing surface the snow covers: a dusting from a few
// millimeters, all of it from 3 cm
fn snowCover(depth: f32) -> f32 {
  return smoothstep(0.002, 0.03, depth);
}

// `albedo` with snow on what faces up (`normal` in the world, +z up): the
// tops of branches, trunks, the spruce's tiers, rocks
fn snowOnTop(albedo: vec3<f32>, normal: vec3<f32>, depth: f32) -> vec3<f32> {
  if (depth <= 0.0) { return albedo; }
  let up = smoothstep(0.25, 0.7, normal.z);
  return mix(albedo, snowAlbedo, up * snowCover(depth));
}

// How much of a grass blade still shows over snow `depth` deep: it is
// pushed down and gone under about 10 cm
fn snowGrassKeep(depth: f32) -> f32 {
  return 1.0 - smoothstep(0.0, 0.1, depth);
}

// A grass blade's point at height `z` (its own units, 0 at the root)
// pushed down under the snow: flattened to what still shows and sunk
// below the ground as it goes, so a blade gone under the snow is hidden
// by the ground, not lying flat on it
fn snowSunkGrass(z: f32, depth: f32) -> f32 {
  let keep = snowGrassKeep(depth);
  return z * keep - (1.0 - keep) * 0.2;
}

// How much of the ground the snow covers at a point: deeper snow covers
// more, steep ground (`slope`, 0 .. 1) holds less, and a noise (`noise`,
// 0 .. 1, a few meters across) makes the edge of a thin cover patchy
fn snowGroundCover(depth: f32, slope: f32, noise: f32) -> f32 {
  if (depth <= 0.0) { return 0.0; }
  let amount = smoothstep(0.0, 0.08, depth) - smoothstep(0.35, 0.8, slope);
  return smoothstep(0.35, 0.65, amount + (noise - 0.5) * 0.5);
}
