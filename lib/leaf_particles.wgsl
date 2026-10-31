// The leaves in the air (lib/LeafParticles.rae, docs/leaf-particles-design.md
// §4.3): a ring of leaves in a storage buffer, no entity per leaf. Composed
// after lib/gpu/computeBounds.wgsl (binding 0), lib/noise.wgsl and
// lib/world_biome.wgsl (raeTerrainHeight, the ground they land on).
//
// Two entry points, recorded one after the other in the frame's one
// recording:
// - `spawn` runs once per leaf released this frame: it finds its emit (the
//   CPU's list of crowns letting leaves go, with a running first-leaf
//   index) by a binary search and writes a new leaf into its ring slot;
// - `fly` runs once per ring slot: a live leaf falls through the air in the
//   wind, flutters side to side and tumbles, lands on the ground (or the
//   water) and lies there until its life ends; one past the reach dies.

struct Leaf {
  position: vec3<f32>,
  // seconds since it let go; 0 (as a new buffer is) the slot is empty
  age: f32,
  // (vx, vy) and (vz, the tumble's angle) as half floats
  motion: vec2<u32>,
  // the color's square root in rgb (8 bits each), its size in a
  color: u32,
  // a random in bits 0..23, bit 31 set once it lies on the ground
  seed: u32,
};
struct LeafEmit {
  // the crown's center and radius
  crown: vec4<f32>,
  // the leaves' color (linear) and size (m)
  color: vec4<f32>,
  // x: how many leaves, y: the first one's index among this frame's
  // releases, z: a seed
  info: vec4<u32>,
};
struct LeafU {
  // the eye and the reach past which a leaf dies
  eye: vec4<f32>,
  // the wind (m/s) xy, the time step (s), the clock (s)
  air: vec4<f32>,
  // x: the ground's base height, y: 1 for flat ground at x (no terrain),
  // z: the water's surface (the lowest a leaf lands), w: unused
  ground: vec4<f32>,
  // x: the ring's head (the first slot this frame writes), y: the ring's
  // size, z: the emits, w: this frame's releases
  ring: vec4<u32>,
};
@group(0) @binding(1) var<uniform> U: LeafU;
@group(0) @binding(2) var<storage, read_write> leaves: array<Leaf>;

// How long a leaf lives (s), and how long it lies on the ground at most
// (MUST match lib/LeafParticles.rae leafLifeSeconds and leaf_render.wgsl)
const leafLifeSeconds: f32 = 20.0;
const leafRestSeconds: f32 = 6.0;
const leafLanded: u32 = 2147483648u;

fn leafHash(value: u32) -> u32 {
  let state = value * 747796405u + 2891336453u;
  let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
fn leafRandom(value: u32) -> f32 {
  return f32(leafHash(value) & 16777215u) * 0.000000059604645;
}

fn leafGround(p: vec2<f32>) -> f32 {
  var ground = U.ground.x;
  if (U.ground.y < 0.5) {
    ground = raeTerrainHeight(p, U.ground.x);
  }
  return max(ground, U.ground.z);
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
  let offset = vec3<f32>(cos(angle) * side, sin(angle) * side, up * 0.6) * out;
  var leaf: Leaf;
  leaf.position = emit.crown.xyz + offset;
  leaf.age = 0.001;
  // it starts with the air's motion and a little drop
  leaf.motion = vec2<u32>(pack2x16float(U.air.xy * 0.5), pack2x16float(vec2<f32>(-0.2, angle)));
  let shade = 0.8 + 0.4 * leafRandom(seed + 3u);
  let color = sqrt(clamp(emit.color.rgb * shade, vec3<f32>(0.0), vec3<f32>(1.0)));
  let size = emit.color.w * (0.75 + 0.5 * leafRandom(seed + 4u));
  leaf.color = pack4x8unorm(vec4<f32>(color, clamp(size / 0.3, 0.0, 1.0)));
  leaf.seed = seed & 16777215u;
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
    leaves[slot] = leaf;
    return;
  }
  let planar = unpack2x16float(leaf.motion.x);
  let fall = unpack2x16float(leaf.motion.y);
  var velocity = vec3<f32>(planar, fall.x);
  var tumble = fall.y;
  let random = leafRandom(leaf.seed + 7u);
  // the air it falls through: the wind, and its own side-to-side glide (a
  // pendulum across its heading) as it falls
  let heading = random * 6.2831853;
  let glide = sin(leaf.age * (1.8 + random * 1.4) + random * 20.0) * (0.5 + 0.4 * random);
  let air = vec3<f32>(U.air.xy + vec2<f32>(cos(heading), sin(heading)) * glide, 0.0);
  // it settles toward the air's motion sideways, and toward its own fall
  // speed (0.7 - 1.3 m/s) downward
  let fallSpeed = 0.7 + 0.6 * leafRandom(leaf.seed + 9u);
  let sideways = min(deltaTime / 0.6, 1.0);
  let downward = min(deltaTime / 0.3, 1.0);
  velocity = vec3<f32>(mix(velocity.xy, air.xy, sideways), mix(velocity.z, -fallSpeed, downward));
  leaf.position = leaf.position + velocity * deltaTime;
  tumble = tumble + deltaTime * (2.0 + 3.0 * random);
  if (tumble > 6.2831853) { tumble = tumble - 6.2831853; }
  let ground = leafGround(leaf.position.xy);
  if (leaf.position.z <= ground + 0.01) {
    leaf.position.z = ground + 0.01;
    leaf.seed = leaf.seed | leafLanded;
    velocity = vec3<f32>(0.0);
    leaf.age = max(leaf.age, leafLifeSeconds - leafRestSeconds);
  }
  leaf.motion = vec2<u32>(pack2x16float(velocity.xy), pack2x16float(vec2<f32>(velocity.z, tumble)));
  leaves[slot] = leaf;
}
