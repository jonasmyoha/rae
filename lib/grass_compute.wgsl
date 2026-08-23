struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,
};
struct GrassU {
  a: vec4<f32>,
  b: vec4<f32>,
  c: vec4<f32>,
  d: vec4<f32>,
  e: vec4<f32>,
  f: vec4<f32>,
  g: vec4<f32>,
};
struct Indirect {
  vertexCount: u32,
  instanceCount: atomic<u32>,
  firstVertex: u32,
  firstInstance: u32,
};
@group(0) @binding(0) var<uniform> G: GrassU;
@group(0) @binding(1) var<storage, read_write> outBuf: array<DrawU>;
@group(0) @binding(2) var<storage, read_write> indirect: Indirect;
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
// Was a private two-octave Perlin here, unrelated to the terrain the CPU builds.
// Now the shared field from world_biome.wgsl, which is prepended.
fn terrainHeight(x: f32, y: f32, groundZ: f32) -> f32 {
  return raeTerrainHeight(vec2<f32>(x, y), groundZ);
}

// Blades grow only in the grass biome. Without this they scatter across sand and
// stand out on open water, since the pass places them on a plain grid.
const GRASS_MIN_WEIGHT: f32 = 0.45;
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
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let i = gid.x;
  let count = u32(G.b.x);
  if (i >= count) { return; }
  let side = u32(sqrt(G.b.x));
  let gx = i % side;
  let gy = i / side;
  let extent = G.b.y;
  let spacing = extent / f32(side);
  let playerCellX = i32(floor(G.a.x / spacing));
  let playerCellY = i32(floor(G.a.y / spacing));
  let worldCellX = playerCellX + i32(gx) - i32(side) / 2;
  let worldCellY = playerCellY + i32(gy) - i32(side) / 2;
  let cellX = f32(worldCellX) * spacing;
  let cellY = f32(worldCellY) * spacing;
  let cellI = vec2<i32>(worldCellX, worldCellY);
  let jx = hash2u(cellI, 7u);
  let jy = hash2u(cellI, 13u);
  let hp = hash2u(cellI, 29u);
  let hs = hash2u(cellI, 43u);
  let bx = cellX + (jx - 0.5) * spacing * 0.9;
  let by = cellY + (jy - 0.5) * spacing * 0.9;
  if (raeBiomeGrassWeight(vec2<f32>(bx, by)) < GRASS_MIN_WEIGHT) { return; }
  let bz = terrainHeight(bx, by, G.a.z);
  let yaw = hp * 6.2831853;
  let hHeight = hash2u(cellI, 61u);
  let hWidth = hash2u(cellI, 71u);
  let baseHeight = (0.30 + hHeight * 0.40) * G.g.x;
  let width = (0.055 + hWidth * 0.045) * G.g.y;
  let distToPlayer = distance(vec2<f32>(bx, by), G.a.xy);
  let radius = extent * 0.5;
  let edgeFade = 1.0 - smoothstep(radius * 0.6, radius * 0.98, distToPlayer);
  let height = baseHeight * edgeFade;
  let scale = vec3<f32>(width, width, height);
  let pos = vec3<f32>(bx, by, bz);
  if (height < 0.02) { return; }
  if (G.g.z < 0.999 && hash2u(cellI, 131u) > G.g.z) { return; }
  let toBlade = pos - G.c.xyz;
  let dist = length(toBlade);
  let cosA = dot(toBlade / max(dist, 0.0001), normalize(G.d.xyz));
  if (dist > G.c.w && cosA < G.d.w) { return; }
  let lodT = clamp((dist - 8.0) / max(radius - 8.0, 1.0), 0.0, 1.0);
  let keepProb = 1.0 - lodT * 0.7;
  if (hash2u(cellI, 151u) > keepProb) { return; }
  let slot = atomicAdd(&indirect.instanceCount, 1u);
  let m = swayModel(pos, yaw, 0.0, 0.0, scale);
  outBuf[slot].model = m;
  outBuf[slot].prevModel = m;
  outBuf[slot].albedoMetallic = vec4<f32>(G.e.rgb, 0.0);
  let mode = select(0.0, 1.0, G.b.z > 0.5);
  let hTip = hash2u(cellI, 113u);
  let tipWidth = hTip * hTip * 0.34;
  outBuf[slot].params = vec4<f32>(0.95, 1.0, mode, tipWidth);
}
