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
  let patchValue = clamp(0.5 + 0.75 * (broad * 0.62 + fine * 0.28 + streak * 0.10), 0.0, 1.0);
  let hue = clamp(0.5 + 0.6 * perlin2(position / 45.0, 727u), 0.0, 1.0);
  return vec2<f32>(patchValue, hue);
}
// The painted palette: a value (0 the darkest root, 1 the lightest tip) and a
// hue to a color, by multiply and add only, so it is linear in the value and a
// blade can blend between its root's and its tip's colors exactly. `base` is
// the grass's own green (the middle of the ramp), `warm` the color warm grass
// leans to, `lighten` how much lighter than `base` the lightest tip is.
fn grassPaintColor(value: f32, hue: f32, base: vec3<f32>, warm: vec3<f32>, lighten: f32) -> vec3<f32> {
  let coolTint = vec3<f32>(0.82, 1.0, 1.12);
  let warmTint = mix(vec3<f32>(1.10, 1.04, 0.82), normalize(warm + vec3<f32>(0.001)) * 1.732, 0.25);
  let tint = mix(coolTint, warmTint, hue);
  let dark = base * 0.42 * vec3<f32>(0.85, 1.0, 1.10);
  let light = base * lighten + vec3<f32>(0.06, 0.08, 0.03);
  return mix(dark, light, value) * tint;
}
// The painted value where a blade's root is, with no blade of its own: what a
// ground shader paints under and past the blades
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
