// The leaves in the air drawn (lib/LeafParticles.rae, docs/leaf-particles-
// design.md §4.4): one instance per ring slot (leaf_particles.wgsl), each a
// small leaf card folded along its midrib (two halves of two triangles: 12
// vertices, no mesh), tumbling as it falls and lying flat once it lands.
// An empty slot collapses outside the clip volume. The G-buffer entry `fs`
// writes gbufferSurface; the forward main pass composes forward_entry.wgsl
// after this file and shades the same surface.
struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,    // z: the wind clock last frame
  wind: vec4<f32>,      // w: the wind clock (s)
  previousWind: vec4<f32>,
};
struct LeafRecord {
  position: vec3<f32>,
  age: f32,
  motion: vec2<u32>,
  color: u32,
  seed: u32,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> leafRecords: array<LeafRecord>;

// MUST match leaf_particles.wgsl
const leafRenderLife: f32 = 20.0;
const leafRenderLanded: u32 = 2147483648u;

fn leafOctWrap(v: vec2<f32>) -> vec2<f32> {
  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
  return (vec2<f32>(1.0) - abs(v.yx)) * s;
}
fn leafOctEncode(n: vec3<f32>) -> vec2<f32> {
  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
  if (n.z < 0.0) { p = leafOctWrap(p); }
  return p * 0.5 + vec2<f32>(0.5);
}
fn leafRotate(value: vec3<f32>, axis: vec3<f32>, angle: f32) -> vec3<f32> {
  let cosine = cos(angle);
  return value * cosine + cross(axis, value) * sin(angle)
       + axis * dot(axis, value) * (1.0 - cosine);
}
fn leafRenderRandom(value: u32) -> f32 {
  let state = value * 747796405u + 2891336453u;
  let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return f32(((word >> 22u) ^ word) & 16777215u) * 0.000000059604645;
}

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) nrm: vec3<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) color: vec3<f32>,
};

// The card's corners in its own frame (x along the midrib, y across, z up
// the fold): the stem, a near and a far side point, the tip; vertex `vi`
// of the 12 (two halves of two triangles)
fn leafCorner(vi: u32, fold: f32) -> vec3<f32> {
  let side = vi / 6u;
  let corner = vi % 6u;
  var index = 0u;
  if (corner == 1u) { index = 1u; }
  else if (corner == 2u) { index = 2u; }
  else if (corner == 4u) { index = 2u; }
  else if (corner == 5u) { index = 3u; }
  let across = select(1.0, -1.0, side == 1u);
  var p = vec3<f32>(-0.5, 0.0, 0.0);
  if (index == 1u) { p = vec3<f32>(-0.15, 0.32 * across, 0.0); }
  if (index == 2u) { p = vec3<f32>(0.22, 0.26 * across, 0.0); }
  if (index == 3u) { p = vec3<f32>(0.5, 0.0, 0.0); }
  p.z = abs(p.y) * fold;
  return p;
}

@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
  let leaf = leafRecords[ii];
  var o: VsOut;
  o.inst = ii;
  o.nrm = vec3<f32>(0.0, 0.0, 1.0);
  o.color = vec3<f32>(0.0);
  if (leaf.age <= 0.0) {
    o.pos = vec4<f32>(0.0, 0.0, -2.0, 1.0);
    o.clipNow = o.pos;
    o.clipPrev = o.pos;
    return o;
  }
  let packed = unpack4x8unorm(leaf.color);
  o.color = packed.rgb * packed.rgb;
  // it shrinks away over its last second and a half
  let size = packed.a * 0.3 * clamp((leafRenderLife - leaf.age) / 1.5, 0.0, 1.0);
  let landed = (leaf.seed & leafRenderLanded) != 0u;
  let random = leafRenderRandom(leaf.seed);
  let fall = unpack2x16float(leaf.motion.y);
  // a fallen leaf lies almost flat; one in the air folds and tumbles about
  // its own axis
  var fold = 0.55;
  if (landed) { fold = 0.15; }
  let local = leafCorner(vi, fold) * size;
  let heading = random * 6.2831853;
  var turned = leafRotate(local, vec3<f32>(0.0, 0.0, 1.0), heading);
  var normalLocal = normalize(cross(leafCorner(vi - vi % 3u + 1u, fold) - leafCorner(vi - vi % 3u, fold),
    leafCorner(vi - vi % 3u + 2u, fold) - leafCorner(vi - vi % 3u, fold)));
  if (normalLocal.z < 0.0) { normalLocal = -normalLocal; }
  var normal = leafRotate(normalLocal, vec3<f32>(0.0, 0.0, 1.0), heading);
  if (!landed) {
    let axis = normalize(vec3<f32>(leafRenderRandom(leaf.seed + 11u) - 0.5,
      leafRenderRandom(leaf.seed + 13u) - 0.5, 0.35));
    turned = leafRotate(turned, axis, fall.y);
    normal = leafRotate(normal, axis, fall.y);
  }
  let world = leaf.position + turned;
  // last frame's place: back along its motion by the frame's time
  let deltaTime = clamp(F.wind.w - F.jitter.z, 0.0, 0.1);
  let velocity = vec3<f32>(unpack2x16float(leaf.motion.x), fall.x);
  let previous = world - velocity * deltaTime;
  o.pos = F.viewProj * vec4<f32>(world, 1.0);
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * vec4<f32>(previous, 1.0);
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  // both faces take the daylight: the normal leans to the sky, as the
  // trees' leaf cards do
  o.nrm = normalize(mix(normal, vec3<f32>(0.0, 0.0, 1.0), 0.45));
  return o;
}

struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};
fn gbufferSurface(in: VsOut) -> FsOut {
  let oct = leafOctEncode(normalize(in.nrm));
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let motionCode = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  // z = 1: TAA-reactive, as the grass (a small moving card ghosts on history)
  o.gba = vec4<f32>(oct.x, oct.y, 1.0, 0.0);
  o.gbb = vec4<f32>(in.color, 0.8);
  o.gbc = vec4<f32>(motionCode.x, motionCode.y, 0.0, 1.0);
  return o;
}
@fragment
fn fs(in: VsOut) -> FsOut {
  return gbufferSurface(in);
}
