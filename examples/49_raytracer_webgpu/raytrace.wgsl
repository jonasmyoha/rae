// WebGPU compute raytracer — the WGSL port of render.rae's rayColor (step 3:
// diffuse / metal / glass, defocus blur, Z-up sky). One invocation per pixel;
// the recursive Rae rayColor is unrolled into an attenuation loop (WGSL has no
// recursion). Scene + camera arrive as a flat f32 buffer authored by main.rae.

struct Params {
  width: u32,
  height: u32,
  samples: u32,
  maxDepth: u32,
  sphereCount: u32,
  frame: u32,
  _pad0: u32,
  _pad1: u32,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read> scene: array<f32>;   // camera(19) + spheres(count*10)
@group(0) @binding(2) var<storage, read_write> outBuf: array<u32>; // W*H packed RGBA8

const PI: f32 = 3.14159265358979;
const TAU: f32 = 6.28318530717958;
const SPHERE_BASE: u32 = 19u;

fn camOrigin()    -> vec3<f32> { return vec3<f32>(scene[0],  scene[1],  scene[2]); }
fn camLowerLeft() -> vec3<f32> { return vec3<f32>(scene[3],  scene[4],  scene[5]); }
fn camHoriz()     -> vec3<f32> { return vec3<f32>(scene[6],  scene[7],  scene[8]); }
fn camVert()      -> vec3<f32> { return vec3<f32>(scene[9],  scene[10], scene[11]); }
fn camRight()     -> vec3<f32> { return vec3<f32>(scene[12], scene[13], scene[14]); }
fn camUp()        -> vec3<f32> { return vec3<f32>(scene[15], scene[16], scene[17]); }
fn camLensR()     -> f32       { return scene[18]; }

// --- RNG: PCG-ish hash, returns f32 in [0,1). State advances in place. ---
var<private> rngState: u32;
fn randU32() -> u32 {
  rngState = rngState * 747796405u + 2891336453u;
  var w: u32 = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
  return (w >> 22u) ^ w;
}
fn rand() -> f32 { return f32(randU32()) * (1.0 / 4294967296.0); }

fn randomUnit() -> vec3<f32> {
  let a: f32 = rand() * TAU;
  let z: f32 = rand() * 2.0 - 1.0;
  let r: f32 = sqrt(max(0.0, 1.0 - z * z));
  return vec3<f32>(r * cos(a), r * sin(a), z);
}

struct Sphere { center: vec3<f32>, radius: f32, albedo: vec3<f32>, kind: i32, fuzz: f32, ior: f32 };
fn getSphere(j: u32) -> Sphere {
  let b: u32 = SPHERE_BASE + j * 10u;
  var s: Sphere;
  s.center = vec3<f32>(scene[b], scene[b + 1u], scene[b + 2u]);
  s.radius = scene[b + 3u];
  s.albedo = vec3<f32>(scene[b + 4u], scene[b + 5u], scene[b + 6u]);
  s.kind = i32(scene[b + 7u]);
  s.fuzz = scene[b + 8u];
  s.ior = scene[b + 9u];
  return s;
}

fn hitSphere(s: Sphere, ro: vec3<f32>, rd: vec3<f32>) -> f32 {
  let oc: vec3<f32> = ro - s.center;
  let a: f32 = dot(rd, rd);
  let halfB: f32 = dot(oc, rd);
  let c: f32 = dot(oc, oc) - s.radius * s.radius;
  let disc: f32 = halfB * halfB - a * c;
  if (disc < 0.0) { return -1.0; }
  return (-halfB - sqrt(disc)) / a;
}

fn schlick(cosine: f32, refIdx: f32) -> f32 {
  var r0: f32 = (1.0 - refIdx) / (1.0 + refIdx);
  r0 = r0 * r0;
  return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

// Iterative path trace: returns the colour for one primary ray.
fn rayColor(ro0: vec3<f32>, rd0: vec3<f32>) -> vec3<f32> {
  var ro: vec3<f32> = ro0;
  var rd: vec3<f32> = rd0;
  var atten: vec3<f32> = vec3<f32>(1.0, 1.0, 1.0);
  for (var depth: u32 = 0u; depth < P.maxDepth; depth = depth + 1u) {
    var best: f32 = 1e9;
    var hitIdx: i32 = -1;
    for (var i: u32 = 0u; i < P.sphereCount; i = i + 1u) {
      let t: f32 = hitSphere(getSphere(i), ro, rd);
      if (t > 0.001 && t < best) { best = t; hitIdx = i32(i); }
    }
    if (hitIdx < 0) {
      let u: vec3<f32> = normalize(rd);
      let tt: f32 = 0.5 * (u.z + 1.0);
      let sky: vec3<f32> = vec3<f32>((1.0 - tt) + tt * 0.5, (1.0 - tt) + tt * 0.7, (1.0 - tt) + tt * 1.0);
      return atten * sky;
    }
    let sp: Sphere = getSphere(u32(hitIdx));
    let p: vec3<f32> = ro + rd * best;
    let n: vec3<f32> = (p - sp.center) * (1.0 / sp.radius);
    var scatter: vec3<f32>;
    var localAtten: vec3<f32> = sp.albedo;
    if (sp.kind == 0) {
      scatter = n + randomUnit();
    } else if (sp.kind == 1) {
      let ud: vec3<f32> = normalize(rd);
      scatter = reflect(ud, n) + randomUnit() * sp.fuzz;
      if (dot(scatter, n) <= 0.0) { return vec3<f32>(0.0, 0.0, 0.0); }
    } else {
      localAtten = vec3<f32>(1.0, 1.0, 1.0);
      let ud: vec3<f32> = normalize(rd);
      var frontN: vec3<f32> = n;
      var etaRatio: f32 = 1.0 / sp.ior;
      if (dot(ud, n) > 0.0) { frontN = -n; etaRatio = sp.ior; }
      var cosT: f32 = dot(-ud, frontN);
      if (cosT > 1.0) { cosT = 1.0; }
      let sinT: f32 = sqrt(max(0.0, 1.0 - cosT * cosT));
      if (etaRatio * sinT > 1.0 || schlick(cosT, etaRatio) > rand()) {
        scatter = reflect(ud, frontN);
      } else {
        scatter = refract(ud, frontN, etaRatio);
      }
    }
    atten = atten * localAtten;
    ro = p;
    rd = scatter;
  }
  return vec3<f32>(0.0, 0.0, 0.0);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let x: u32 = gid.x;
  let y: u32 = gid.y;
  if (x >= P.width || y >= P.height) { return; }
  // Seed the RNG per pixel + frame so accumulation (future) decorrelates.
  rngState = (x * 1973u) ^ (y * 9277u) ^ (P.frame * 26699u) ^ 0x9e3779b9u;

  let W1: f32 = f32(P.width - 1u);
  let H1: f32 = f32(P.height - 1u);
  let origin: vec3<f32> = camOrigin();
  let ll: vec3<f32> = camLowerLeft();
  let horiz: vec3<f32> = camHoriz();
  let vert: vec3<f32> = camVert();
  let right: vec3<f32> = camRight();
  let up: vec3<f32> = camUp();
  let lensR: f32 = camLensR();

  var acc: vec3<f32> = vec3<f32>(0.0, 0.0, 0.0);
  for (var s: u32 = 0u; s < P.samples; s = s + 1u) {
    let u: f32 = (f32(x) + rand()) / W1;
    let v: f32 = (f32(P.height - 1u - y) + rand()) / H1;
    var ro: vec3<f32> = origin;
    if (lensR > 0.0) {
      let a: f32 = rand() * TAU;
      let r: f32 = lensR * sqrt(rand());
      ro = origin + right * (r * cos(a)) + up * (r * sin(a));
    }
    let tgt: vec3<f32> = ll + horiz * u + vert * v;
    acc = acc + rayColor(ro, tgt - ro);
  }

  let inv: f32 = 1.0 / f32(P.samples);
  let rr: u32 = u32(clamp(255.99 * sqrt(clamp(acc.x * inv, 0.0, 1.0)), 0.0, 255.0));
  let gg: u32 = u32(clamp(255.99 * sqrt(clamp(acc.y * inv, 0.0, 1.0)), 0.0, 255.0));
  let bb: u32 = u32(clamp(255.99 * sqrt(clamp(acc.z * inv, 0.0, 1.0)), 0.0, 255.0));
  // Pack RGBA8 little-endian: R | G<<8 | B<<16 | A<<24.
  outBuf[y * P.width + x] = rr | (gg << 8u) | (bb << 16u) | (255u << 24u);
}
