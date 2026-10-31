// The leaves in the air (lib/LeafParticles.rae, docs/leaf-particles-design.md
// §4.3): a ring of leaves in a storage buffer, no entity per leaf. Composed
// after lib/gpu/computeBounds.wgsl (binding 0), lib/noise.wgsl,
// lib/world_biome.wgsl (raeTerrainHeight, the ground they land on) and
// lib/wind.wgsl (windGustAt and its noise: the gusts and the swirls).
//
// Two entry points, recorded one after the other in the frame's one
// recording:
// - `spawn` runs once per leaf released this frame: it finds its emit (the
//   CPU's list of crowns letting leaves go, with a running first-leaf
//   index) by a binary search and writes a new leaf into its ring slot;
// - `fly` runs once per ring slot: a live leaf falls through the air in the
//   air, lands on the ground (on the snow, if any) or floats on the water,
//   and lies there until its life ends; one past the reach dies.
//
// The air a leaf falls through (docs/leaf-particles-design.md §5): the
// mean wind, stronger in a gust (the gust field the trees and the grass
// sway in, so a gust carries the leaves it shakes loose); swirls, the curl
// of a slow noise carried with the air (no divergence, so the leaves do
// not bunch), stronger in a gust; an updraft at a gust's leading edge (the
// gust ahead weaker than here), which lifts leaves over the crowns; and its
// own side-to-side glide. It tumbles faster in a gust.
//
// Storm gusts carry vortices (docs/leaf-particles-design.md §5.3): one
// center in each 30 m cell of a lattice carried with the air, live only
// where a storm gust is at its center, so one or two travel through the
// forest with the gust's front. A vortex spins the leaves round it and
// lifts them at its core, up over the crowns (no higher than about 16 m
// over the ground). An emit marked as a ground lift (info.w bit 4) starts
// its leaves on the ground and throws them up: the leaves already lying
// there, swept up into the swirl.

struct Leaf {
  position: vec3<f32>,
  // seconds since it let go; 0 (as a new buffer is) the slot is empty
  age: f32,
  // (vx, vy) and (vz, the tumble's angle) as half floats
  motion: vec2<u32>,
  // the color's square root in rgb (8 bits each), its size in a
  color: u32,
  // a random in bits 0..23, its shape in bits 24..25 (leaf_render.wgsl),
  // bit 30 set while it floats on the water, bit 31 once it has landed
  seed: u32,
};
struct LeafEmit {
  // the crown's center and radius
  crown: vec4<f32>,
  // the leaves' color (linear) and size (m)
  color: vec4<f32>,
  // x: how many leaves, y: the first one's index among this frame's
  // releases, z: a seed, w: the leaves' shape in bits 0..1 (0 oval,
  // 1 maple, 2 birch, 3 ginkgo), bit 4 a ground lift
  info: vec4<u32>,
};
// MUST match leaf_render.wgsl LeafDrawU and lib/LeafParticles.rae
struct LeafU {
  // the eye and the reach past which a leaf dies
  eye: vec4<f32>,
  // the wind (m/s) xy, the time step (s), the clock (s)
  air: vec4<f32>,
  // x: the ground's base height, y: 1 for flat ground at x (no terrain),
  // z: the water's surface (the lowest a leaf lands), w: the snow's depth
  ground: vec4<f32>,
  // x: the ring's head (the first slot this frame writes), y: the ring's
  // size, z: the emits, w: this frame's releases
  ring: vec4<u32>,
  // the renderer's wind vector (lib/Wind: sway xy, gustiness, its clock)
  wind: vec4<f32>,
  // xy: the air's drift (m), z: 1 for the toon look (leaf_render.wgsl)
  drift: vec4<f32>,
  // xyz: toward the sun (leaf_render.wgsl's backlit glow)
  sun: vec4<f32>,
};
@group(0) @binding(1) var<uniform> U: LeafU;
@group(0) @binding(2) var<storage, read_write> leaves: array<Leaf>;

// How long a leaf lives (s), and how long it lies on the ground at most
// (MUST match lib/LeafParticles.rae leafLifeSeconds and leaf_render.wgsl)
const leafLifeSeconds: f32 = 20.0;
const leafRestSeconds: f32 = 6.0;
const leafLanded: u32 = 2147483648u;
const leafFloating: u32 = 1073741824u;

fn leafHash(value: u32) -> u32 {
  let state = value * 747796405u + 2891336453u;
  let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
fn leafRandom(value: u32) -> f32 {
  return f32(leafHash(value) & 16777215u) * 0.000000059604645;
}

// The land under `p`: the terrain (or the lab's flat ground) under the
// scene's snow
fn leafLand(p: vec2<f32>) -> f32 {
  var ground = U.ground.x;
  if (U.ground.y < 0.5) {
    ground = raeTerrainHeight(p, U.ground.x);
  }
  return ground + U.ground.w;
}

// The swirls: the curl of a slow noise (cells of 7 m) carried with the air
fn leafSwirl(p: vec2<f32>) -> vec3<f32> {
  let q = (p - U.drift.xy) * 0.14285715;
  let h = 0.1;
  let dx = windGustNoise(q + vec2<f32>(h, 0.0), 5u) - windGustNoise(q - vec2<f32>(h, 0.0), 5u);
  let dy = windGustNoise(q + vec2<f32>(0.0, h), 5u) - windGustNoise(q - vec2<f32>(0.0, h), 5u);
  let up = windGustNoise(q * 1.3 + vec2<f32>(3.7, 1.9), 6u) - 0.5;
  return vec3<f32>(dy, -dx, 0.0) * (0.5 / h) + vec3<f32>(0.0, 0.0, up);
}

// The vortices' flow at `p`, `above` m over the ground, in a wind of
// `speed` m/s: the 2 x 2 lattice cells round it, each center live by the
// storm gust there
fn leafVortices(p: vec3<f32>, above: f32, speed: f32) -> vec3<f32> {
  let air = (p.xy - U.drift.xy) * 0.033333335;
  let corner = floor(air - vec2<f32>(0.5));
  var flow = vec3<f32>(0.0);
  for (var k = 0u; k < 4u; k = k + 1u) {
    let cell = corner + vec2<f32>(f32(k & 1u), f32(k >> 1u));
    let key = leafHash(u32(i32(cell.x) & 1023) * 1031u + u32(i32(cell.y) & 1023) * 7919u + 17u);
    let center = (cell + vec2<f32>(0.2 + 0.6 * leafRandom(key), 0.2 + 0.6 * leafRandom(key + 1u))) * 30.0 + U.drift.xy;
    let storm = smoothstep(0.7, 0.95, windGustAt(center, U.wind, U.drift.xy));
    if (storm <= 0.0) { continue; }
    let toLeaf = p.xy - center;
    let radius = 5.0 + 3.0 * leafRandom(key + 2u);
    let core = exp(-dot(toLeaf, toLeaf) / (radius * radius));
    let spin = select(-1.0, 1.0, leafRandom(key + 3u) > 0.5);
    let circling = vec2<f32>(-toLeaf.y, toLeaf.x) / radius * spin * speed * 1.2;
    let inward = -toLeaf / radius * speed * 0.25;
    let ceiling = clamp((16.0 - above) / 5.0, 0.0, 1.0);
    flow = flow + vec3<f32>((circling + inward) * core, (2.5 + 0.35 * speed) * core * ceiling) * storm;
  }
  return flow;
}

@group(0) @binding(3) var<storage, read> emits: array<LeafEmit>;

@compute @workgroup_size(computeWorkgroupSize)
fn spawn(@builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!computeElementInRange(globalId)) { return; }
  let release = computeElementIndex(globalId);
  // the emit whose releases hold this one: the last whose first index is
  // at most `release`
  var low = 0u;
  var high = U.ring.z;
  loop {
    if (high - low <= 1u) { break; }
    let middle = (low + high) / 2u;
    if (emits[middle].info.y <= release) { low = middle; } else { high = middle; }
  }
  let emit = emits[low];
  let seed = leafHash(emit.info.z * 977u + release - emit.info.y);
  // a point in the crown, more of them toward its outside
  let angle = leafRandom(seed) * 6.2831853;
  let up = leafRandom(seed + 1u) * 2.0 - 1.0;
  let out = emit.crown.w * pow(leafRandom(seed + 2u), 0.4);
  let side = sqrt(max(1.0 - up * up, 0.0));
  var offset = vec3<f32>(cos(angle) * side, sin(angle) * side, up * 0.6) * out;
  // it starts with the air's motion and a little drop; a ground lift's
  // leaf from the ground, thrown up
  var rise = -0.2;
  if ((emit.info.w & 16u) != 0u) {
    offset = vec3<f32>(offset.xy, 0.05 + 0.25 * abs(up));
    rise = 4.0 + 2.0 * leafRandom(seed + 5u);
  }
  var leaf: Leaf;
  leaf.position = emit.crown.xyz + offset;
  leaf.age = 0.001;
  leaf.motion = vec2<u32>(pack2x16float(U.air.xy * 0.5), pack2x16float(vec2<f32>(rise, angle)));
  let shade = 0.8 + 0.4 * leafRandom(seed + 3u);
  let color = sqrt(clamp(emit.color.rgb * shade, vec3<f32>(0.0), vec3<f32>(1.0)));
  let size = emit.color.w * (0.75 + 0.5 * leafRandom(seed + 4u));
  leaf.color = pack4x8unorm(vec4<f32>(color, clamp(size / 0.3, 0.0, 1.0)));
  leaf.seed = (seed & 16777215u) | ((emit.info.w & 3u) << 24u);
  leaves[(U.ring.x + release) % U.ring.y] = leaf;
}

@compute @workgroup_size(computeWorkgroupSize)
fn fly(@builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!computeElementInRange(globalId)) { return; }
  let slot = computeElementIndex(globalId);
  var leaf = leaves[slot];
  if (leaf.age <= 0.0) { return; }
  let deltaTime = U.air.z;
  leaf.age = leaf.age + deltaTime;
  let toEye = leaf.position.xy - U.eye.xy;
  if (leaf.age > leafLifeSeconds || dot(toEye, toEye) > U.eye.w * U.eye.w) {
    leaf.age = 0.0;
    leaves[slot] = leaf;
    return;
  }
  if ((leaf.seed & leafLanded) != 0u) {
    // a leaf on the water drifts slowly downwind
    if ((leaf.seed & leafFloating) != 0u) {
      leaf.position = vec3<f32>(leaf.position.xy + U.air.xy * 0.06 * deltaTime, leaf.position.z);
    }
    leaves[slot] = leaf;
    return;
  }
  let planar = unpack2x16float(leaf.motion.x);
  let fall = unpack2x16float(leaf.motion.y);
  var velocity = vec3<f32>(planar, fall.x);
  var tumble = fall.y;
  let random = leafRandom(leaf.seed + 7u);
  let speed = length(U.air.xy);
  let gust = windGustAt(leaf.position.xy, U.wind, U.drift.xy);
  // the mean wind, stronger in a gust
  let wind = U.air.xy * (0.7 + 0.9 * gust);
  // the swirls, and an updraft where a gust arrives (the gust 5 m ahead
  // downwind weaker than here)
  let swirl = leafSwirl(leaf.position.xy) * speed * (0.25 + 1.1 * gust);
  var ahead = leaf.position.xy;
  if (speed > 0.01) { ahead = ahead + U.air.xy / speed * 5.0; }
  let front = max(gust - windGustAt(ahead, U.wind, U.drift.xy), 0.0);
  let updraft = front * speed * 1.4;
  // its own glide, a pendulum across its heading
  let heading = random * 6.2831853;
  let glide = sin(leaf.age * (1.8 + random * 1.4) + random * 20.0) * (0.5 + 0.4 * random);
  let vortex = leafVortices(leaf.position, leaf.position.z - leafLand(leaf.position.xy), speed);
  let air = vec3<f32>(wind + vec2<f32>(cos(heading), sin(heading)) * glide + swirl.xy + vortex.xy,
    swirl.z + updraft + vortex.z);
  // it settles toward the air's motion sideways, and toward its own fall
  // speed (0.7 - 1.3 m/s) through the air downward
  let fallSpeed = 0.7 + 0.6 * leafRandom(leaf.seed + 9u);
  let sideways = min(deltaTime / 0.6, 1.0);
  let downward = min(deltaTime / 0.3, 1.0);
  velocity = vec3<f32>(mix(velocity.xy, air.xy, sideways), mix(velocity.z, air.z - fallSpeed, downward));
  leaf.position = leaf.position + velocity * deltaTime;
  tumble = tumble + deltaTime * (2.0 + 3.0 * random) * (1.0 + 3.0 * gust);
  if (tumble > 6.2831853) { tumble = tumble - 6.2831853; }
  let land = leafLand(leaf.position.xy);
  let water = U.ground.z;
  let lowest = max(land, water);
  if (leaf.position.z <= lowest + 0.01) {
    leaf.position.z = lowest + 0.01;
    leaf.seed = leaf.seed | leafLanded;
    if (water > land) {
      leaf.seed = leaf.seed | leafFloating;
    }
    velocity = vec3<f32>(0.0);
    leaf.age = max(leaf.age, leafLifeSeconds - leafRestSeconds);
  }
  leaf.motion = vec2<u32>(pack2x16float(velocity.xy), pack2x16float(vec2<f32>(velocity.z, tumble)));
  leaves[slot] = leaf;
}
