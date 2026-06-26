// Progressive accumulation kernel — one path-traced sample per pixel per
// dispatch, summed into a persistent HDR accum buffer, then tone-mapped to a
// packed-RGBA8 framebuffer. The host resets (sampleIndex=0) when the camera
// moves and otherwise lets the image refine. Same shading as raytrace.wgsl.

struct Params {
  width: u32,
  height: u32,
  sampleIndex: u32,   // samples already accumulated (0 => overwrite, reset)
  maxDepth: u32,
  sphereCount: u32,
  seed: u32,
  samplesPerFrame: u32,   // new samples to take this dispatch (adaptive)
  skyMode: u32,           // 0 = bright gradient, 1 = dim (scene lit by emitters)
  boxCount: u32,          // axis-aligned boxes in `boxes` (12 f32 each)
  _p0: u32,
  _p1: u32,
  _p2: u32,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read>       scene: array<f32>; // camera(19)+spheres
@group(0) @binding(2) var<storage, read_write>  accum: array<f32>; // 4 per pixel (rgb)
@group(0) @binding(3) var<storage, read_write>  outBuf: array<u32>; // packed RGBA8
@group(0) @binding(4) var<storage, read>        boxes: array<f32>; // lo(3) hi(3) albedo(3) kind fuzz ior
@group(0) @binding(5) var<storage, read>        bvh: array<f32>;     // 8 per node (sphere BVH)
@group(0) @binding(6) var<storage, read>        primRef: array<f32>; // reordered prim refs (sphere idx, or 1e6+tri idx)
@group(0) @binding(7) var<storage, read>        tris: array<f32>;    // v0(3) v1(3) v2(3) albedo(3) kind fuzz ior

const TRI_BASE: u32 = 1000000u;  // primRef >= TRI_BASE => triangle (idx = ref - TRI_BASE)

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

// Axis-aligned box: 12 f32 = lo(3) hi(3) albedo(3) kind fuzz ior.
struct Box { lo: vec3<f32>, hi: vec3<f32>, albedo: vec3<f32>, kind: i32, fuzz: f32, ior: f32 };
fn getBox(j: u32) -> Box {
  let b: u32 = j * 12u;
  var bx: Box;
  bx.lo = vec3<f32>(boxes[b], boxes[b + 1u], boxes[b + 2u]);
  bx.hi = vec3<f32>(boxes[b + 3u], boxes[b + 4u], boxes[b + 5u]);
  bx.albedo = vec3<f32>(boxes[b + 6u], boxes[b + 7u], boxes[b + 8u]);
  bx.kind = i32(boxes[b + 9u]);
  bx.fuzz = boxes[b + 10u];
  bx.ior = boxes[b + 11u];
  return bx;
}
// Slab intersection; returns nearest positive t (entry, or exit if inside), or -1.
fn hitBox(bx: Box, ro: vec3<f32>, rd: vec3<f32>) -> f32 {
  let inv: vec3<f32> = 1.0 / rd;
  let t0: vec3<f32> = (bx.lo - ro) * inv;
  let t1: vec3<f32> = (bx.hi - ro) * inv;
  let tsm: vec3<f32> = min(t0, t1);
  let tbg: vec3<f32> = max(t0, t1);
  let tn: f32 = max(max(tsm.x, tsm.y), tsm.z);
  let tf: f32 = min(min(tbg.x, tbg.y), tbg.z);
  if (tf < tn || tf < 0.001) { return -1.0; }
  if (tn > 0.001) { return tn; }
  return tf;
}
// Triangle: 15 f32 = v0(3) v1(3) v2(3) albedo(3) kind fuzz ior.
struct Tri { v0: vec3<f32>, v1: vec3<f32>, v2: vec3<f32>, albedo: vec3<f32>, kind: i32, fuzz: f32, ior: f32 };
fn getTri(j: u32) -> Tri {
  let b: u32 = j * 15u;
  var t: Tri;
  t.v0 = vec3<f32>(tris[b], tris[b + 1u], tris[b + 2u]);
  t.v1 = vec3<f32>(tris[b + 3u], tris[b + 4u], tris[b + 5u]);
  t.v2 = vec3<f32>(tris[b + 6u], tris[b + 7u], tris[b + 8u]);
  t.albedo = vec3<f32>(tris[b + 9u], tris[b + 10u], tris[b + 11u]);
  t.kind = i32(tris[b + 12u]);
  t.fuzz = tris[b + 13u];
  t.ior = tris[b + 14u];
  return t;
}
// Möller–Trumbore; returns t along rd (caller checks > 0.001), or -1 on miss.
fn hitTri(t: Tri, ro: vec3<f32>, rd: vec3<f32>) -> f32 {
  let e1: vec3<f32> = t.v1 - t.v0;
  let e2: vec3<f32> = t.v2 - t.v0;
  let pv: vec3<f32> = cross(rd, e2);
  let det: f32 = dot(e1, pv);
  if (abs(det) < 1e-8) { return -1.0; }
  let inv: f32 = 1.0 / det;
  let tv: vec3<f32> = ro - t.v0;
  let u: f32 = dot(tv, pv) * inv;
  if (u < 0.0 || u > 1.0) { return -1.0; }
  let qv: vec3<f32> = cross(tv, e1);
  let v: f32 = dot(rd, qv) * inv;
  if (v < 0.0 || u + v > 1.0) { return -1.0; }
  return dot(e2, qv) * inv;
}
fn hitAABB(mn: vec3<f32>, mx: vec3<f32>, ro: vec3<f32>, invd: vec3<f32>, tmax: f32) -> bool {
  let t0: vec3<f32> = (mn - ro) * invd;
  let t1: vec3<f32> = (mx - ro) * invd;
  let ts: vec3<f32> = min(t0, t1);
  let tb: vec3<f32> = max(t0, t1);
  let tn: f32 = max(max(ts.x, ts.y), ts.z);
  let tf: f32 = min(min(tb.x, tb.y), tb.z);
  return tf >= max(tn, 0.001) && tn <= tmax;
}
fn boxNormal(bx: Box, p: vec3<f32>) -> vec3<f32> {
  let c: vec3<f32> = (bx.lo + bx.hi) * 0.5;
  let d: vec3<f32> = max((bx.hi - bx.lo) * 0.5, vec3<f32>(1e-6));
  let pl: vec3<f32> = (p - c) / d;
  let a: vec3<f32> = abs(pl);
  if (a.x >= a.y && a.x >= a.z) { return vec3<f32>(sign(pl.x), 0.0, 0.0); }
  if (a.y >= a.z) { return vec3<f32>(0.0, sign(pl.y), 0.0); }
  return vec3<f32>(0.0, 0.0, sign(pl.z));
}
fn rayColor(ro0: vec3<f32>, rd0: vec3<f32>) -> vec3<f32> {
  var ro: vec3<f32> = ro0;
  var rd: vec3<f32> = rd0;
  var atten: vec3<f32> = vec3<f32>(1.0, 1.0, 1.0);
  for (var depth: u32 = 0u; depth < P.maxDepth; depth = depth + 1u) {
    // Nearest hit across spheres + boxes. Record normal + material generically.
    var best: f32 = 1e9;
    var n: vec3<f32> = vec3<f32>(0.0, 0.0, 1.0);
    var albedo: vec3<f32> = vec3<f32>(0.0, 0.0, 0.0);
    var kind: i32 = -1;
    var fuzz: f32 = 0.0;
    var ior: f32 = 0.0;
    // Spheres via the BVH (binding 5/6). Iterative stack traversal.
    if (P.sphereCount > 0u) {
      let invd: vec3<f32> = vec3<f32>(1.0 / rd.x, 1.0 / rd.y, 1.0 / rd.z);
      var stack: array<i32, 32>;
      var sptr: i32 = 0;
      stack[0] = 0;
      sptr = 1;
      loop {
        if (sptr <= 0) { break; }
        sptr = sptr - 1;
        let ni: i32 = stack[sptr];
        let base: u32 = u32(ni) * 8u;
        let mn: vec3<f32> = vec3<f32>(bvh[base], bvh[base + 1u], bvh[base + 2u]);
        let mx: vec3<f32> = vec3<f32>(bvh[base + 3u], bvh[base + 4u], bvh[base + 5u]);
        if (!hitAABB(mn, mx, ro, invd, best)) { continue; }
        let cnt: i32 = i32(bvh[base + 7u]);
        if (cnt > 0) {
          let first: i32 = i32(bvh[base + 6u]);
          for (var k: i32 = 0; k < cnt; k = k + 1) {
            let pr: u32 = u32(primRef[u32(first + k)]);
            if (pr >= TRI_BASE) {
              let tr: Tri = getTri(pr - TRI_BASE);
              let t: f32 = hitTri(tr, ro, rd);
              if (t > 0.001 && t < best) {
                best = t;
                var nn: vec3<f32> = normalize(cross(tr.v1 - tr.v0, tr.v2 - tr.v0));
                if (dot(nn, rd) > 0.0) { nn = -nn; }
                n = nn; albedo = tr.albedo; kind = tr.kind; fuzz = tr.fuzz; ior = tr.ior;
              }
            } else {
              let sp: Sphere = getSphere(pr);
              let t: f32 = hitSphere(sp, ro, rd);
              if (t > 0.001 && t < best) {
                best = t;
                n = (ro + rd * t - sp.center) * (1.0 / sp.radius);
                albedo = sp.albedo; kind = sp.kind; fuzz = sp.fuzz; ior = sp.ior;
              }
            }
          }
        } else {
          let right: i32 = i32(bvh[base + 6u]);
          if (sptr < 31) { stack[sptr] = right; sptr = sptr + 1; }
          if (sptr < 31) { stack[sptr] = ni + 1; sptr = sptr + 1; }
        }
      }
    }
    for (var j: u32 = 0u; j < P.boxCount; j = j + 1u) {
      let bx: Box = getBox(j);
      let t: f32 = hitBox(bx, ro, rd);
      if (t > 0.001 && t < best) {
        best = t;
        n = boxNormal(bx, ro + rd * t);
        albedo = bx.albedo; kind = bx.kind; fuzz = bx.fuzz; ior = bx.ior;
      }
    }
    if (kind < 0) {
      if (P.skyMode == 1u) {
        return atten * vec3<f32>(0.015, 0.015, 0.02);  // dim ambient — scene lit by emitters
      }
      let u: vec3<f32> = normalize(rd);
      let tt: f32 = 0.5 * (u.z + 1.0);
      let sky: vec3<f32> = vec3<f32>((1.0 - tt) + tt * 0.5, (1.0 - tt) + tt * 0.7, (1.0 - tt) + tt * 1.0);
      return atten * sky;
    }
    let p: vec3<f32> = ro + rd * best;
    var scatter: vec3<f32>;
    var localAtten: vec3<f32> = albedo;
    if (kind == 3) {
      return atten * albedo;   // emissive light: emit + terminate
    } else if (kind == 0) {
      scatter = n + randomUnit();
    } else if (kind == 1) {
      let ud: vec3<f32> = normalize(rd);
      scatter = reflect(ud, n) + randomUnit() * fuzz;
      if (dot(scatter, n) <= 0.0) { return vec3<f32>(0.0, 0.0, 0.0); }
    } else {
      localAtten = vec3<f32>(1.0, 1.0, 1.0);
      let ud: vec3<f32> = normalize(rd);
      var frontN: vec3<f32> = n;
      var etaRatio: f32 = 1.0 / ior;
      if (dot(ud, n) > 0.0) { frontN = -n; etaRatio = ior; }
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

  let W1: f32 = f32(P.width - 1u);
  let H1: f32 = f32(P.height - 1u);
  let lensR: f32 = camLensR();
  let spp: u32 = max(P.samplesPerFrame, 1u);

  // Take `spp` new samples this dispatch (adaptive — the host raises it when
  // there's framerate headroom so the image converges faster).
  var fresh: vec3<f32> = vec3<f32>(0.0, 0.0, 0.0);
  for (var k: u32 = 0u; k < spp; k = k + 1u) {
    rngState = (x * 1973u) ^ (y * 9277u) ^ ((P.sampleIndex + k) * 26699u) ^ (P.seed * 0x9e3779b9u + 1u);
    let u: f32 = (f32(x) + rand()) / W1;
    let v: f32 = (f32(P.height - 1u - y) + rand()) / H1;
    var ro: vec3<f32> = camOrigin();
    if (lensR > 0.0) {
      let a: f32 = rand() * TAU;
      let r: f32 = lensR * sqrt(rand());
      ro = ro + camRight() * (r * cos(a)) + camUp() * (r * sin(a));
    }
    let tgt: vec3<f32> = camLowerLeft() + camHoriz() * u + camVert() * v;
    fresh = fresh + rayColor(ro, tgt - ro);
  }

  let i: u32 = y * P.width + x;
  let b: u32 = i * 4u;
  var sum: vec3<f32>;
  if (P.sampleIndex == 0u) {
    sum = fresh;
  } else {
    sum = vec3<f32>(accum[b], accum[b + 1u], accum[b + 2u]) + fresh;
  }
  accum[b] = sum.x; accum[b + 1u] = sum.y; accum[b + 2u] = sum.z;

  let inv: f32 = 1.0 / f32(P.sampleIndex + spp);
  let rr: u32 = u32(clamp(255.99 * sqrt(clamp(sum.x * inv, 0.0, 1.0)), 0.0, 255.0));
  let gg: u32 = u32(clamp(255.99 * sqrt(clamp(sum.y * inv, 0.0, 1.0)), 0.0, 255.0));
  let bb: u32 = u32(clamp(255.99 * sqrt(clamp(sum.z * inv, 0.0, 1.0)), 0.0, 255.0));
  // Pack 0xRRGGBB (what the SDL3 backend's sdlUpdatePixels expects) — this path
  // hands the u32 straight through gpuDownloadU32, no host-side repack.
  outBuf[i] = (rr << 16u) | (gg << 8u) | bb;
}
