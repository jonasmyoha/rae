// The leaves in the air drawn (lib/LeafParticles.rae, docs/leaf-particles-
// design.md §4.4): one instance per ring slot (leaf_particles.wgsl), each a
// card folded along its midrib (two quads: 12 vertices, no mesh), tumbling
// as it falls, lying nearly flat once it lands and bobbing on the water.
// An empty slot collapses outside the clip volume. The G-buffer entry `fs`
// writes gbufferSurface; the forward main pass composes forward_entry.wgsl
// after this file and shades the same surface.
//
// - The outline is cut out of the card in the fragment shader, one of four
//   shapes the leaf carries (its kind's: an oval, a five-lobed maple, a
//   serrated birch, a ginkgo fan): no texture.
// - Both faces are lit: the normal turns toward the eye. A leaf with the sun
//   behind it glows (the light through it, the way a leaf card in the
//   trees reads backlit): lighter and warmer.
// - It fades out by a dither over its last two seconds (no blending).
// - The toon look writes the toon mode, so it takes the same ramp as the
//   rest of the scene.
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
// MUST match leaf_particles.wgsl LeafU
struct LeafDrawU {
  eye: vec4<f32>,
  air: vec4<f32>,
  ground: vec4<f32>,
  ring: vec4<u32>,
  wind: vec4<f32>,
  drift: vec4<f32>,
  sun: vec4<f32>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> leafRecords: array<LeafRecord>;
@group(0) @binding(2) var<uniform> D: LeafDrawU;

// MUST match leaf_particles.wgsl
const leafRenderLife: f32 = 20.0;
const leafRenderLanded: u32 = 2147483648u;
const leafRenderFloating: u32 = 1073741824u;

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
  // u along the midrib (0 the stem, 1 the tip), v across (-1 .. 1)
  @location(5) leafUv: vec2<f32>,
  @location(6) @interpolate(flat) shape: u32,
  @location(7) @interpolate(flat) fade: f32,
};

// The card's width over its length by shape (an oval and a birch leaf are
// narrow, a maple and a ginkgo leaf about as wide as long)
fn leafAspect(shape: u32) -> f32 {
  if (shape == 1u || shape == 3u) { return 1.0; }
  return 0.62;
}

// A world pose of the card corner `local`: turned to its heading, and in
// the air tumbled by `tumble` about its own axis
fn leafPose(local: vec3<f32>, seed: u32, landed: bool, tumble: f32) -> vec3<f32> {
  let heading = leafRenderRandom(seed) * 6.2831853;
  var turned = leafRotate(local, vec3<f32>(0.0, 0.0, 1.0), heading);
  if (!landed) {
    let axis = normalize(vec3<f32>(leafRenderRandom(seed + 11u) - 0.5,
      leafRenderRandom(seed + 13u) - 0.5, 0.35));
    turned = leafRotate(turned, axis, tumble);
  }
  return turned;
}

@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {
  let leaf = leafRecords[ii];
  var o: VsOut;
  o.inst = ii;
  o.nrm = vec3<f32>(0.0, 0.0, 1.0);
  o.color = vec3<f32>(0.0);
  o.leafUv = vec2<f32>(0.0);
  o.shape = 0u;
  o.fade = 0.0;
  if (leaf.age <= 0.0) {
    o.pos = vec4<f32>(0.0, 0.0, -2.0, 1.0);
    o.clipNow = o.pos;
    o.clipPrev = o.pos;
    return o;
  }
  let packed = unpack4x8unorm(leaf.color);
  let size = packed.a * 0.3;
  let landed = (leaf.seed & leafRenderLanded) != 0u;
  let floating = (leaf.seed & leafRenderFloating) != 0u;
  let shape = (leaf.seed >> 24u) & 3u;
  let fall = unpack2x16float(leaf.motion.y);
  // the corner: one of the two halves (a quad each) across the midrib
  let side = select(1.0, -1.0, vi >= 6u);
  let corners = array<vec2<f32>, 6>(vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0),
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));
  let uv = corners[vi % 6u];
  var fold = 0.5;
  if (landed) { fold = 0.12; }
  let across = uv.y * side * 0.5 * leafAspect(shape) * size;
  let local = vec3<f32>((uv.x - 0.5) * size, across, abs(across) * fold);
  var normal = normalize(vec3<f32>(0.0, -side * fold, 1.0));
  let posed = leafPose(local, leaf.seed, landed, fall.y);
  normal = leafPose(normal, leaf.seed, landed, fall.y);
  var world = leaf.position + posed;
  if (floating) {
    world.z = world.z + 0.012 * sin(F.wind.w * 2.1 + leafRenderRandom(leaf.seed + 5u) * 6.2831853);
  }
  // last frame: back along its motion, its tumble a frame younger
  let deltaTime = clamp(F.wind.w - F.jitter.z, 0.0, 0.1);
  let velocity = vec3<f32>(unpack2x16float(leaf.motion.x), fall.x);
  let previousPose = leafPose(local, leaf.seed, landed, fall.y - deltaTime * 4.0);
  let previous = leaf.position - velocity * deltaTime + previousPose;
  o.pos = F.viewProj * vec4<f32>(world, 1.0);
  o.clipNow = o.pos;
  o.clipPrev = F.prevViewProj * vec4<f32>(previous, 1.0);
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  // the face toward the eye is lit; the sun behind it shines through
  let toEye = D.eye.xyz - world;
  if (dot(normal, toEye) < 0.0) { normal = -normal; }
  let backlit = clamp(-dot(normal, D.sun.xyz), 0.0, 1.0);
  let base = packed.rgb * packed.rgb;
  o.color = base * (1.0 + 0.9 * backlit) + vec3<f32>(0.05, 0.035, 0.0) * backlit;
  // the normal leans to the sky a little, as the trees' leaf cards do
  o.nrm = normalize(mix(normal, vec3<f32>(0.0, 0.0, 1.0), 0.3));
  o.leafUv = vec2<f32>(uv.x, uv.y * side);
  o.shape = shape;
  o.fade = clamp((leafRenderLife - leaf.age) * 0.5, 0.0, 1.0);
  return o;
}

// Whether (u, v) of the card lies inside the leaf of `shape`
fn leafInside(uv: vec2<f32>, shape: u32) -> bool {
  let u = uv.x;
  let v = abs(uv.y);
  // the stalk: a thin line at the stem end
  if (u < 0.14) { return v < 0.06; }
  let t = (u - 0.14) / 0.86;
  if (shape == 1u) {
    // a maple: five pointed lobes round a center near the stalk
    let p = vec2<f32>(t - 0.42, uv.y * 0.85);
    let angle = atan2(p.y, p.x);
    let lobes = pow(abs(cos(angle * 2.5)), 0.7);
    return length(p) < 0.27 + 0.31 * lobes && angle > -2.6 && angle < 2.6;
  }
  if (shape == 2u) {
    // a birch: a pointed oval with a toothed edge
    let width = 0.95 * sin(3.14159265 * pow(t, 0.75)) * (1.0 - 0.1 * abs(sin(t * 36.0)));
    return v < width;
  }
  if (shape == 3u) {
    // a ginkgo: a fan from the stalk, notched at the middle of its rim
    let p = vec2<f32>(t, uv.y);
    let angle = atan2(p.y, p.x);
    let notch = 0.2 * exp(-angle * angle * 60.0);
    return abs(angle) < 0.95 && length(p) < 1.0 - notch;
  }
  // an oval
  return v < sin(3.14159265 * pow(t, 0.85));
}

struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};
fn gbufferSurface(in: VsOut) -> FsOut {
  if (!leafInside(in.leafUv, in.shape)) { discard; }
  // the fade's dither (interleaved gradient noise, as fade_dither.wgsl)
  let dither = fract(52.9829189 * fract(dot(floor(in.pos.xy), vec2<f32>(0.06711056, 0.00583715))));
  if (dither >= in.fade) { discard; }
  let oct = leafOctEncode(normalize(in.nrm));
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let motionCode = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  // the darker midrib
  let rib = 1.0 - 0.18 * (1.0 - smoothstep(0.0, 0.06, abs(in.leafUv.y)));
  var o: FsOut;
  // z = 1: TAA-reactive, as the grass (a small moving card ghosts on
  // history); w the shading mode (1: toon)
  o.gba = vec4<f32>(oct.x, oct.y, 1.0, select(0.0, 1.0, D.drift.z > 0.5));
  o.gbb = vec4<f32>(in.color * rib, 0.75);
  o.gbc = vec4<f32>(motionCode.x, motionCode.y, 0.0, 1.0);
  return o;
}
@fragment
fn fs(in: VsOut) -> FsOut {
  return gbufferSurface(in);
}
