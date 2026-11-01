// compose-after: lib/gpu/computeBounds.wgsl
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,
  fade: vec4<f32>,     // the level cross-fade (GbufferRecord [40..43]); 0 when not fading
};
struct GrassU {
  a: vec4<f32>,
  b: vec4<f32>,
  c: vec4<f32>,
  d: vec4<f32>,
  e: vec4<f32>,
  f: vec4<f32>,
  g: vec4<f32>,
  h: vec4<f32>,
  i: vec4<f32>,
  // the ground palette's dry grass and how dry (app3d/GroundPalette)
  j: vec4<f32>,
  // the air's drift (xy) and the unscaled wind (zw): the gust field's
  k: vec4<f32>,
};
struct Indirect {
  vertexCount: u32,
  instanceCount: atomic<u32>,
  firstVertex: u32,
  firstInstance: u32,
};
// Binding 0 is the bounds uniform of lib/gpu/computeBounds.wgsl (the blade
// count), which every composition lists before this file
// (lib/gpu/GpuCompute.rae, docs/shaders-and-the-compiler.md).
@group(0) @binding(1) var<uniform> G: GrassU;
@group(0) @binding(2) var<storage, read_write> outBuf: array<DrawU>;
@group(0) @binding(3) var<storage, read_write> indirect: Indirect;
// Was a private two-octave Perlin here, unrelated to the terrain the CPU builds.
// Now the shared field from world_biome.wgsl, which is prepended.
fn terrainHeight(x: f32, y: f32, groundZ: f32) -> f32 {
  return raeTerrainHeight(vec2<f32>(x, y), groundZ);
}

// Blades grow only where the ground LOOKS like grass. The classify weight alone
// is a near-step at the beach line, but the terrain shader re-splits the
// sand+grass weight over a wide elevation band (terrain_detail.wgsl, #83) so
// the beach dissolves into the meadow — and blades placed by the raw weight
// stood on ground painted as sand. This applies the same band (keep
// GRASS_SANDGRASS_BLUR equal to RAE_SANDGRASS_BLUR) and asks for a clear
// majority of grass, so no blade stands on the sand texture.
const GRASS_MIN_WEIGHT: f32 = 0.70;
const GRASS_SANDGRASS_BLUR: f32 = 0.10;
fn grassDrawnWeight(s: RaeBiome) -> f32 {
  let sandHi = RAE_BIOME_WATER_LEVEL + RAE_BIOME_BEACH_BAND;
  let sgMix = smoothstep(sandHi - GRASS_SANDGRASS_BLUR, sandHi + GRASS_SANDGRASS_BLUR, s.elevation);
  return (s.wSand + s.wGrass) * sgMix;
}
fn mTranslate(t: vec3<f32>) -> mat4x4<f32> {
  return mat4x4<f32>(vec4<f32>(1.0,0.0,0.0,0.0), vec4<f32>(0.0,1.0,0.0,0.0), vec4<f32>(0.0,0.0,1.0,0.0), vec4<f32>(t.x,t.y,t.z,1.0));
}
fn mScale3(s: vec3<f32>) -> mat4x4<f32> {
  return mat4x4<f32>(vec4<f32>(s.x,0.0,0.0,0.0), vec4<f32>(0.0,s.y,0.0,0.0), vec4<f32>(0.0,0.0,s.z,0.0), vec4<f32>(0.0,0.0,0.0,1.0));
}
fn mRotX(a: f32) -> mat4x4<f32> {
  let c = cos(a); let s = sin(a);
  return mat4x4<f32>(vec4<f32>(1.0,0.0,0.0,0.0), vec4<f32>(0.0,c,s,0.0), vec4<f32>(0.0,-s,c,0.0), vec4<f32>(0.0,0.0,0.0,1.0));
}
fn mRotY(a: f32) -> mat4x4<f32> {
  let c = cos(a); let s = sin(a);
  return mat4x4<f32>(vec4<f32>(c,0.0,-s,0.0), vec4<f32>(0.0,1.0,0.0,0.0), vec4<f32>(s,0.0,c,0.0), vec4<f32>(0.0,0.0,0.0,1.0));
}
fn mRotZ(a: f32) -> mat4x4<f32> {
  let c = cos(a); let s = sin(a);
  return mat4x4<f32>(vec4<f32>(c,s,0.0,0.0), vec4<f32>(-s,c,0.0,0.0), vec4<f32>(0.0,0.0,1.0,0.0), vec4<f32>(0.0,0.0,0.0,1.0));
}
fn swayModel(pos: vec3<f32>, yaw: f32, tiltX: f32, tiltY: f32, scale: vec3<f32>) -> mat4x4<f32> {
  return mTranslate(pos) * (mRotZ(yaw) * (mRotX(tiltX) * (mRotY(tiltY) * mScale3(scale))));
}
@compute @workgroup_size(computeWorkgroupSize)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (!computeElementInRange(gid)) { return; }
  let i = computeElementIndex(gid);
  let side = u32(sqrt(G.b.x));
  let gx = i % side;
  let gy = i / side;
  let extent = G.b.y;
  let spacing = extent / f32(side);
  // The grid's center: what the camera looks at (G.a.xy)
  let centerCellX = i32(floor(G.a.x / spacing));
  let centerCellY = i32(floor(G.a.y / spacing));
  let worldCellX = centerCellX + i32(gx) - i32(side) / 2;
  let worldCellY = centerCellY + i32(gy) - i32(side) / 2;
  let cellI = vec2<i32>(worldCellX, worldCellY);
  let hp = hash2u(cellI, 29u);
  let clusterStrength = clamp(G.e.w, 0.0, 1.0);
  let bladePosition = grassCandidatePosition(cellI, spacing, clusterStrength);
  let bx = bladePosition.x;
  let by = bladePosition.y;
  // one biome sample per blade: its grass weight here, its moisture for the
  // ground palette's dry grass below (only the surface's raeBiomeSample is
  // needed, so an app's own surface keeps working)
  let bladeBiome = raeBiomeSample(vec2<f32>(bx, by));
  if (grassDrawnWeight(bladeBiome) < GRASS_MIN_WEIGHT) { return; }
  // Roads overlay the grass biome. Reject their painted footprint outright;
  // shrinking blades leaves conspicuous stubble on dirt. Keep only a narrow
  // fringe where the road overlay is below ten percent.
  if (raeBiomePath(bladePosition) > 0.10) { return; }
  if (raeGrassObstacle(bladePosition) > 0.0) { return; }
  if (clusterStrength > 0.0) {
    let patchValue = grassPatchValue(bladePosition, G.h.x);
    let threshold = 1.0 - clamp(G.h.y, 0.05, 0.95);
    let feather = max(G.h.z, 0.001);
    let patchKeep = smoothstep(threshold - feather, threshold + feather, patchValue);
    if (hash2u(cellI, 179u) > mix(1.0, patchKeep, clusterStrength)) { return; }
  }
  let bz = terrainHeight(bx, by, G.a.z);
  let yaw = hp * 6.2831853;
  let hHeight = hash2u(cellI, 61u);
  let hWidth = hash2u(cellI, 71u);
  // Keep broad variation inside each tuft. The four-segment ribbon supplies
  // the close silhouette; density is deliberately unchanged.
  let variation = clamp(G.h.w, 0.0, 1.5);
  let baseHeight = mix(0.50, 0.28 + hHeight * 0.44, variation) * G.g.x;
  let width = mix(0.079, 0.052 + hWidth * 0.054, variation) * G.g.y;
  let distToCenter = distance(vec2<f32>(bx, by), G.a.xy);
  let radius = extent * 0.5;
  let edgeFade = 1.0 - smoothstep(radius * 0.6, radius * 0.98, distToCenter);
  let patchBody = smoothstep(0.36, 0.61, grassPatchValue(bladePosition, G.h.x));
  var patchHeight = mix(0.62, 1.08, patchBody);
  if (clusterStrength < 1.0) { patchHeight = mix(1.0, patchHeight, clusterStrength); }
  let height = baseHeight * patchHeight * edgeFade;
  let pos = vec3<f32>(bx, by, bz);
  if (height < 0.02) { return; }
  if (G.g.z < 0.999 && hash2u(cellI, 131u) > G.g.z) { return; }
  let toBlade = pos - G.c.xyz;
  let dist = length(toBlade);
  let cosA = dot(toBlade / max(dist, 0.0001), normalize(G.d.xyz));
  if (dist > G.c.w && cosA < G.d.w) { return; }
  // Thin only after the near field. Patch membership stays unchanged, while
  // surviving far ribbons widen a little so the painted ground still reads as
  // meadow instead of a uniformly empty ring.
  let distanceShare = dist / max(radius, 0.001);
  let lodT = smoothstep(clamp(G.i.x, 0.0, 0.9), 0.96, distanceShare);
  let keepProb = mix(1.0, clamp(G.i.y, 0.05, 1.0), lodT);
  if (hash2u(cellI, 151u) > keepProb) { return; }
  let distanceWidth = mix(1.0, max(G.i.z, 1.0), lodT);
  let scale = vec3<f32>(width * distanceWidth, width * distanceWidth, height);
  let slot = atomicAdd(&indirect.instanceCount, 1u);
  let m = swayModel(pos, yaw, 0.0, 0.0, scale);
  outBuf[slot].model = m;
  outBuf[slot].prevModel = m;
  // The terrain's own grass color where the blade stands (the painted
  // ground's field, terrain_grass_painted.wgsl), with no tint of its own:
  // the blade's root is the ground's color, so the grass grows out of the
  // ground instead of standing on it (grass_render.wgsl lightens the tip)
  // (the frame's ground palette, mixed toward its dry grass where the soil
  // is dry: grass_field.wgsl groundPaletteGrass, as the terrain does)
  var groundBase = G.e.rgb;
  if (G.j.w > 0.0) {
    groundBase = groundPaletteGrass(G.e.rgb, G.j.rgb, G.j.w, bladeBiome.moisture);
  }
  let groundColor = grassGroundColor(groundBase, bladePosition, G.h.x, G.h.y);
  outBuf[slot].albedoMetallic = vec4<f32>(groundColor, 0.0);
  let mode = select(0.0, 1.0, G.b.z > 0.5);
  let curve = (hash2u(cellI, 113u) * 2.0 - 1.0) * (0.24 + hash2u(cellI, 127u) * 0.42) * variation;
  outBuf[slot].params = vec4<f32>(0.88, 1.0, mode, curve);
  // x: the gust at the blade (the trees' gust field, lib/wind.wgsl
  // windGustAt, as this file's grassGustAt copy), once per blade, for
  // grass_render.wgsl's sway (the grass draws no level fade)
  outBuf[slot].fade = vec4<f32>(grassGustAt(pos.xy, vec4<f32>(G.k.zw, 0.0, G.f.w), G.k.xy), 0.0, 0.0, 0.0);
}

// --- gust field begin: lib/wind.wgsl's gust field (windGustAt), renamed
// grassGust*, so an app's own grass composition needs no extra file; case
// 1280_grass_gust_copy holds the two copies equal
fn grassGustPcg(value: u32) -> u32 {
  let state = value * 747796405u + 2891336453u;
  let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
fn grassGustHash(column: i32, row: i32, seed: u32) -> f32 {
  let inner = grassGustPcg(u32(row & 255) + seed * 256u);
  let mixed = grassGustPcg(u32(column & 255) + inner);
  return f32(mixed & 16777215u) * 0.000000059604645;
}
fn grassGustSmooth(low: f32, scale: f32, value: f32) -> f32 {
  let t = clamp((value - low) * scale, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
fn grassGustNoise(p: vec2<f32>, seed: u32) -> f32 {
  let cell = floor(p);
  let f = p - cell;
  let u = f * f * (3.0 - 2.0 * f);
  let column = i32(cell.x);
  let row = i32(cell.y);
  let a = grassGustHash(column, row, seed);
  let b = grassGustHash(column + 1, row, seed);
  let c = grassGustHash(column, row + 1, seed);
  let d = grassGustHash(column + 1, row + 1, seed);
  let bottom = a + (b - a) * u.x;
  let top = c + (d - c) * u.x;
  return bottom + (top - bottom) * u.y;
}
fn grassGustAt(xy: vec2<f32>, wind: vec4<f32>, drift: vec2<f32>) -> f32 {
  let strength = sqrt(wind.x * wind.x + wind.y * wind.y);
  let air = xy - drift;
  let blend = 0.5 + 0.5 * sin(wind.w * 0.05235988);
  let first = grassGustNoise(air * 0.020833334, 1u);
  let second = grassGustNoise(air * 0.020833334, 2u);
  let fine = grassGustNoise(air * 0.041666668, 3u);
  let field = 0.65 * (first + (second - first) * blend) + 0.35 * fine;
  let ordinary = grassGustSmooth(0.42, 2.631579, field) * grassGustSmooth(0.15, 2.857143, strength);
  let threshold = 0.88 - 0.05 * clamp(strength - 1.0, 0.0, 2.0);
  let storm = grassGustSmooth(threshold, 12.5, grassGustNoise(air * 0.010416667, 4u))
            * grassGustSmooth(0.8, 1.25, strength);
  return max(0.65 * ordinary, storm);
}
// --- gust field end
