fn hashWord(v0: u32) -> u32 {
  var h = v0;
  h = h ^ (h >> 16u);
  h = h * 0x7feb352du;
  h = h ^ (h >> 15u);
  h = h * 0x846ca68bu;
  h = h ^ (h >> 16u);
  return h;
}
fn hashLattice2(p: vec2<i32>, seed: u32) -> u32 {
  var h = seed ^ 0x9e3779b9u;
  h = hashWord(h ^ (u32(p.x) * 0x85ebca6bu));
  h = hashWord(h ^ (u32(p.y) * 0xc2b2ae35u));
  return h;
}
fn hash2u(p: vec2<i32>, seed: u32) -> f32 { return f32(hashLattice2(p, seed)) / 4294967295.0; }
fn fadeN(t: f32) -> f32 { return t*t*t*(t*(t*6.0-15.0)+10.0); }
fn grad2(hash: u32, p: vec2<f32>) -> f32 {
  let h = hash & 7u;
  if (h == 0u) { return p.x; }
  if (h == 1u) { return -p.x; }
  if (h == 2u) { return p.y; }
  if (h == 3u) { return -p.y; }
  if (h == 4u) { return (p.x + p.y) * 0.70710678; }
  if (h == 5u) { return (-p.x + p.y) * 0.70710678; }
  if (h == 6u) { return (p.x - p.y) * 0.70710678; }
  return (-p.x - p.y) * 0.70710678;
}
fn perlin2(p: vec2<f32>, seed: u32) -> f32 {
  let cell = vec2<i32>(floor(p));
  let f = fract(p);
  let u = vec2<f32>(fadeN(f.x), fadeN(f.y));
  let a = mix(grad2(hashLattice2(cell, seed), f), grad2(hashLattice2(cell + vec2<i32>(1,0), seed), f - vec2<f32>(1.0,0.0)), u.x);
  let b = mix(grad2(hashLattice2(cell + vec2<i32>(0,1), seed), f - vec2<f32>(0.0,1.0)), grad2(hashLattice2(cell + vec2<i32>(1,1), seed), f - vec2<f32>(1.0,1.0)), u.x);
  return mix(a, b, u.y) * 1.41421356;
}
// Shared world-space patch shape: used by placement, ground and tuft color.
override RAE_GRASS_PATCH_SCALE: f32 = 7.5;
override RAE_GRASS_PATCH_COVERAGE: f32 = 0.58;
fn grassPatchValue(position: vec2<f32>, scale: f32) -> f32 {
  let span = max(scale, 0.5);
  return 0.5 + 0.5 * (perlin2(position / span, 173u) * 0.72
    + perlin2(position / (span * 0.43), 227u) * 0.28);
}
// The ground palette (app3d/GroundPalette): the grass color at a point,
// from the frame's grass, its dry grass and how far the driest ground has
// gone dry, by the soil's moisture there (world_biome.wgsl
// raeBiomeMoisture). The terrain, the GPU grass and the meadow (the Rae twin,
// GroundPalette.groundGrassAt) all compute it, so the roots match the ground.
fn groundDryShare(moisture: f32) -> f32 {
  return 1.0 - smoothstep(0.3, 0.55, moisture);
}
fn groundPaletteGrass(grass: vec3<f32>, dry: vec3<f32>, dryAmount: f32, moisture: f32) -> vec3<f32> {
  return mix(grass, dry, dryAmount * groundDryShare(moisture));
}
fn grassGroundColor(base: vec3<f32>, position: vec2<f32>, scale: f32, coverage: f32) -> vec3<f32> {
  let patchValue = grassPatchValue(position, scale);
  let cover = smoothstep(1.0 - coverage - 0.12, 1.0 - coverage + 0.12, patchValue);
  let fine = smoothstep(-0.28, 0.30, perlin2(position * vec2<f32>(1.1, 1.7), 937u));
  let open = base * vec3<f32>(1.08, 1.00, 1.12);
  let tuft = base * vec3<f32>(0.76, 0.84, 1.05);
  return mix(open, tuft, cover) * (0.94 + fine * 0.10);
}

// The painted grass's bands: a value (0 to 1) stepped into this many flat bands
// (0: smooth), with steps this soft (0: hard, 1: as smooth as the bands allow).
// Cel shading for the painted grass: the patches it stands in (grassPaintAt, so
// the ground painted as grass steps the same) and its gradient up each blade
// (grass_render.wgsl) step together. Three bands with soft-ish steps: hard
// steps gave each blade a dark lower half, soft ones barely showed the
// patches; this gives broad flat areas of light and dark, as in a painting.
const grassPaintBands: f32 = 3.0;
const grassPaintBandSoftness: f32 = 0.3;
fn grassPaintBand(value: f32) -> f32 {
  if (grassPaintBands < 1.5) { return value; }
  let scaled = clamp(value, 0.0, 1.0) * grassPaintBands;
  let band = min(floor(scaled), grassPaintBands - 1.0);
  let within = scaled - band;
  let spread = 0.5 * clamp(grassPaintBandSoftness, 0.001, 1.0);
  return (band + smoothstep(0.5 - spread, 0.5 + spread, within)) / grassPaintBands;
}
// The painted grass's field at a point on the ground (GrassShape.painted).
// Sampled at a blade's ROOT, never where its bent tip is, so a ground shader
// calling it at the same point gets the same answer and the blades melt into
// the ground. x: the patch value, 0 dark to 1 light, from soft patches at two
// scales (about 14 m and 4 m) and thin streaks along x (brush strokes); y: the
// hue, 0 cool to 1 warm, over a larger scale (about 45 m).
fn grassPaintAt(position: vec2<f32>) -> vec2<f32> {
  let broad = perlin2(position / 14.0, 701u);
  let fine = perlin2(position / 4.0, 709u);
  let streak = perlin2(position * vec2<f32>(0.35, 1.4), 719u);
  // brush strokes: a fine, long noise across the field; the bands turn it into
  // ragged, painted edges between the patches
  let stroke = perlin2(position * vec2<f32>(0.9, 3.2), 751u);
  let patchValue = grassPaintBand(clamp(0.5 + 0.75 * (broad * 0.60 + fine * 0.26 + streak * 0.08 + stroke * 0.16), 0.0, 1.0));
  let hue = clamp(0.5 + 0.6 * perlin2(position / 45.0, 727u), 0.0, 1.0);
  return vec2<f32>(patchValue, hue);
}
// The painted palette: a value (0 the darkest root, 1 the lightest tip) and a
// hue to a color, by multiply and add. Two straight pieces: from dark to `base`
// (the grass's own green) up to 0.5, then from `base` to the light tip, which
// is `lighten` times as light. A root's value stays under 0.5, so its color
// needs only `base`: a ground painted as grass (gbuffer_static_surface.wgsl)
// matches the blades' roots without knowing how light their tips are. Within
// each piece it is linear, so a blade blends between its root's and its tip's
// colors (grass_render.wgsl). The hue tints from cool blue-green to warm
// yellow-green.
fn grassPaintColor(value: f32, hue: f32, base: vec3<f32>, lighten: f32) -> vec3<f32> {
  let tint = mix(vec3<f32>(0.84, 1.0, 1.10), vec3<f32>(1.10, 1.04, 0.80), hue);
  let dark = base * 0.42 * vec3<f32>(0.85, 1.0, 1.10);
  let light = base * lighten + vec3<f32>(0.06, 0.08, 0.03);
  let lower = mix(dark, base, clamp(value * 2.0, 0.0, 1.0));
  let upper = mix(base, light, clamp(value * 2.0 - 1.0, 0.0, 1.0));
  return select(lower, upper, value > 0.5) * tint;
}
// The painted grass's wind shimmer at a point: bands of lighter grass rolling
// downwind across the field, about 28 m apart at 3 m/s, broken into patches,
// so a distant field painted as grass moves too. `wind` is the wind's
// direction times its strength (lib/wind.wgsl's, unscaled: the static
// surface's F.wind.xy, the grass compute's G.k.zw) and `time` its clock, so
// the ground and the blades' roots and tips shimmer as one. Added to the
// painted value; 0 with no wind.
const grassShimmerSpacing: f32 = 28.0;
const grassShimmerSpeed: f32 = 3.0;
const grassShimmerLight: f32 = 0.30;
fn grassPaintShimmer(position: vec2<f32>, wind: vec2<f32>, time: f32) -> f32 {
  let strength = length(wind);
  if (strength < 0.05) { return 0.0; }
  let direction = wind / strength;
  let warp = perlin2(position / 60.0, 739u) * 14.0;
  let along = dot(position, direction) + warp - time * grassShimmerSpeed;
  let wave = 0.5 + 0.5 * sin(along * 6.2831853 / grassShimmerSpacing);
  let patchy = smoothstep(-0.2, 0.4, perlin2(position / 45.0 - direction * time * 0.05, 743u));
  return smoothstep(0.35, 1.0, wave) * patchy * grassShimmerLight;
}
// The painted value at a blade's root, and of the ground there (under 0.5,
// so its color is the lower piece of grassPaintColor, which needs only the
// base color): what a ground shader paints under and past the blades
fn grassPaintRootValue(paint: vec2<f32>) -> f32 {
  return clamp(0.12 + (paint.x - 0.5) * 0.60, 0.0, 1.0);
}

// Integer world cells, never camera-relative seeds. Exclusions run AFTER this.
fn grassCandidatePosition(cell: vec2<i32>, spacing: f32, clusterStrength: f32) -> vec2<f32> {
  let position = vec2<f32>(cell) * spacing;
  let jitter = vec2<f32>(hash2u(cell, 7u), hash2u(cell, 13u)) - vec2<f32>(0.5);
  let warp = vec2<f32>(perlin2(position * 1.6, 461u), perlin2(position * 1.6, 463u));
  let scattered = position + jitter * spacing * 0.9 + warp * spacing * 0.65;
  let groupCell = vec2<i32>(floor(scattered / 1.6));
  let centerJitter = vec2<f32>(hash2u(groupCell, 479u), hash2u(groupCell, 487u));
  let center = (vec2<f32>(groupCell) + vec2<f32>(0.18) + centerJitter * 0.64) * 1.6;
  let gathering = 0.18 + hash2u(groupCell, 491u) * 0.48;
  let grouped = mix(scattered, center, gathering);
  if (clusterStrength >= 1.0) { return grouped; }
  return mix(position + jitter * spacing * 0.9, grouped, max(clusterStrength, 0.0));
}
