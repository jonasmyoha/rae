// Minimal Rae 3D renderer seed.
//
// Compute raymarcher over an ECS-like object/material buffer. This is not the
// future raster path; it is the smallest realtime 3D slice the current WebGPU
// compute API can support cleanly.

struct Params {
  width: u32,
  height: u32,
  frame: u32,
  objectCount: u32,
  materialCount: u32,
  quality: u32,
  _pad0: u32,
  _pad1: u32,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read> camera: array<f32>;    // 16 f32
@group(0) @binding(2) var<storage, read> objects: array<f32>;   // 12 f32 each
@group(0) @binding(3) var<storage, read> materials: array<f32>; // 8 f32 each
@group(0) @binding(4) var<storage, read_write> outBuf: array<u32>;

const MAX_DIST: f32 = 70.0;
const EPS: f32 = 0.0015;
const PI: f32 = 3.14159265358979;

fn camPos() -> vec3<f32> { return vec3<f32>(camera[0], camera[1], camera[2]); }
fn camAspect() -> f32 { return camera[3]; }
fn camFwd() -> vec3<f32> { return normalize(vec3<f32>(camera[4], camera[5], camera[6])); }
fn camRight() -> vec3<f32> { return normalize(vec3<f32>(camera[8], camera[9], camera[10])); }
fn camUp() -> vec3<f32> { return normalize(vec3<f32>(camera[12], camera[13], camera[14])); }

fn hash31(p: vec3<f32>) -> f32 {
  let q: vec3<f32> = fract(p * 0.1031);
  let d: f32 = dot(q, q.yzx + vec3<f32>(33.33, 33.33, 33.33));
  return fract((q.x + q.y) * (q.z + d));
}

fn valueNoise(p: vec3<f32>) -> f32 {
  let i: vec3<f32> = floor(p);
  let f: vec3<f32> = fract(p);
  let u: vec3<f32> = f * f * (vec3<f32>(3.0, 3.0, 3.0) - 2.0 * f);
  let n000: f32 = hash31(i + vec3<f32>(0.0, 0.0, 0.0));
  let n100: f32 = hash31(i + vec3<f32>(1.0, 0.0, 0.0));
  let n010: f32 = hash31(i + vec3<f32>(0.0, 1.0, 0.0));
  let n110: f32 = hash31(i + vec3<f32>(1.0, 1.0, 0.0));
  let n001: f32 = hash31(i + vec3<f32>(0.0, 0.0, 1.0));
  let n101: f32 = hash31(i + vec3<f32>(1.0, 0.0, 1.0));
  let n011: f32 = hash31(i + vec3<f32>(0.0, 1.0, 1.0));
  let n111: f32 = hash31(i + vec3<f32>(1.0, 1.0, 1.0));
  let nx00: f32 = mix(n000, n100, u.x);
  let nx10: f32 = mix(n010, n110, u.x);
  let nx01: f32 = mix(n001, n101, u.x);
  let nx11: f32 = mix(n011, n111, u.x);
  return mix(mix(nx00, nx10, u.y), mix(nx01, nx11, u.y), u.z);
}

fn fbm(p0: vec3<f32>) -> f32 {
  var p: vec3<f32> = p0;
  var a: f32 = 0.5;
  var v: f32 = 0.0;
  for (var i: u32 = 0u; i < 4u; i = i + 1u) {
    v = v + valueNoise(p) * a;
    p = p * 2.03 + vec3<f32>(11.7, 3.1, 5.4);
    a = a * 0.5;
  }
  return v;
}

fn sdSphere(p: vec3<f32>, r: f32) -> f32 {
  return length(p) - r;
}

fn sdBox(p: vec3<f32>, b: vec3<f32>) -> f32 {
  let q: vec3<f32> = abs(p) - b;
  return length(max(q, vec3<f32>(0.0, 0.0, 0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

struct Hit {
  d: f32,
  mat: u32,
};

fn objectDistance(i: u32, p: vec3<f32>, t: f32) -> Hit {
  let b: u32 = i * 12u;
  let shape: u32 = u32(objects[b]);
  let mat: u32 = u32(objects[b + 1u]);
  let pos: vec3<f32> = vec3<f32>(objects[b + 2u], objects[b + 3u], objects[b + 4u]);
  let scl: vec3<f32> = vec3<f32>(objects[b + 5u], objects[b + 6u], objects[b + 7u]);
  let phase: f32 = objects[b + 8u];
  let noiseAmp: f32 = objects[b + 9u];
  var q: vec3<f32> = p - pos;
  q.z = q.z - sin(t * 0.9 + phase) * 0.09;

  var d: f32;
  if (shape == 0u) {
    d = sdSphere(q, scl.x);
  } else if (shape == 1u) {
    let c: f32 = cos(t * 0.45 + phase);
    let s: f32 = sin(t * 0.45 + phase);
    let rq: vec3<f32> = vec3<f32>(q.x * c - q.y * s, q.x * s + q.y * c, q.z);
    d = sdBox(rq, scl * 0.72) - 0.05;
  } else {
    d = p.z - pos.z;
  }

  if (noiseAmp > 0.0) {
    d = d + (fbm(q * 3.2 + vec3<f32>(0.0, 0.0, t * 0.15)) - 0.5) * noiseAmp;
  }
  return Hit(d, mat);
}

fn mapScene(p: vec3<f32>) -> Hit {
  let t: f32 = f32(P.frame) * 0.016;
  var h: Hit;
  h.d = MAX_DIST;
  h.mat = 0u;
  for (var i: u32 = 0u; i < P.objectCount; i = i + 1u) {
    let oh: Hit = objectDistance(i, p, t);
    if (oh.d < h.d) {
      h = oh;
    }
  }
  return h;
}

fn normalAt(p: vec3<f32>) -> vec3<f32> {
  let e: vec2<f32> = vec2<f32>(EPS, 0.0);
  let n: vec3<f32> = vec3<f32>(
    mapScene(p + e.xyy).d - mapScene(p - e.xyy).d,
    mapScene(p + e.yxy).d - mapScene(p - e.yxy).d,
    mapScene(p + e.yyx).d - mapScene(p - e.yyx).d
  );
  return normalize(n);
}

fn ambientOcclusion(p: vec3<f32>, n: vec3<f32>) -> f32 {
  var occ: f32 = 0.0;
  var sca: f32 = 1.0;
  for (var i: u32 = 1u; i <= 5u; i = i + 1u) {
    let h: f32 = 0.045 * f32(i);
    let d: f32 = mapScene(p + n * h).d;
    occ = occ + (h - d) * sca;
    sca = sca * 0.72;
  }
  return clamp(1.0 - occ * 2.0, 0.0, 1.0);
}

fn softShadow(ro: vec3<f32>, rd: vec3<f32>) -> f32 {
  var res: f32 = 1.0;
  var t: f32 = 0.04;
  for (var i: u32 = 0u; i < 42u; i = i + 1u) {
    let h: f32 = mapScene(ro + rd * t).d;
    res = min(res, 12.0 * h / t);
    t = t + clamp(h, 0.035, 0.28);
    if (res < 0.01 || t > 16.0) { break; }
  }
  return clamp(res, 0.0, 1.0);
}

fn materialColor(mat: u32) -> vec3<f32> {
  let b: u32 = min(mat, P.materialCount - 1u) * 8u;
  return vec3<f32>(materials[b], materials[b + 1u], materials[b + 2u]);
}
fn materialRoughness(mat: u32) -> f32 {
  return clamp(materials[min(mat, P.materialCount - 1u) * 8u + 3u], 0.04, 1.0);
}
fn materialMetallic(mat: u32) -> f32 {
  return clamp(materials[min(mat, P.materialCount - 1u) * 8u + 4u], 0.0, 1.0);
}
fn materialEmissive(mat: u32) -> f32 {
  return max(materials[min(mat, P.materialCount - 1u) * 8u + 5u], 0.0);
}

fn shade(p: vec3<f32>, rd: vec3<f32>, mat: u32) -> vec3<f32> {
  let n: vec3<f32> = normalAt(p);
  let v: vec3<f32> = normalize(-rd);
  let lightDir: vec3<f32> = normalize(vec3<f32>(-0.55, -0.40, 0.74));
  let halfDir: vec3<f32> = normalize(lightDir + v);
  let base: vec3<f32> = materialColor(mat);
  let rough: f32 = materialRoughness(mat);
  let metal: f32 = materialMetallic(mat);
  let emit: f32 = materialEmissive(mat);

  let ndl: f32 = max(dot(n, lightDir), 0.0);
  let ndv: f32 = max(dot(n, v), 0.0);
  let ndh: f32 = max(dot(n, halfDir), 0.0);
  let shadow: f32 = softShadow(p + n * 0.01, lightDir);
  let ao: f32 = ambientOcclusion(p, n);

  let diffuse: vec3<f32> = base * (1.0 - metal) * ndl * shadow;
  let specPower: f32 = mix(96.0, 10.0, rough);
  let fresnel: vec3<f32> = mix(vec3<f32>(0.04, 0.04, 0.04), base, metal) + (vec3<f32>(1.0, 1.0, 1.0) - mix(vec3<f32>(0.04, 0.04, 0.04), base, metal)) * pow(1.0 - ndv, 5.0);
  let spec: vec3<f32> = fresnel * pow(ndh, specPower) * shadow * (1.0 - rough * 0.55);
  let ambient: vec3<f32> = base * vec3<f32>(0.08, 0.10, 0.14) * ao;
  let rim: vec3<f32> = vec3<f32>(0.18, 0.35, 0.65) * pow(1.0 - ndv, 2.2) * ao;
  return ambient + diffuse + spec + rim + base * emit;
}

fn sky(rd: vec3<f32>) -> vec3<f32> {
  let t: f32 = clamp(rd.z * 0.5 + 0.5, 0.0, 1.0);
  let low: vec3<f32> = vec3<f32>(0.025, 0.030, 0.045);
  let high: vec3<f32> = vec3<f32>(0.20, 0.32, 0.56);
  let glow: f32 = pow(max(dot(rd, normalize(vec3<f32>(-0.35, -0.20, 0.92))), 0.0), 32.0);
  return mix(low, high, t) + vec3<f32>(0.40, 0.55, 0.85) * glow;
}

fn render(ro: vec3<f32>, rd: vec3<f32>) -> vec3<f32> {
  var t: f32 = 0.0;
  var mat: u32 = 0u;
  var hit: bool = false;
  for (var i: u32 = 0u; i < 104u; i = i + 1u) {
    let p: vec3<f32> = ro + rd * t;
    let h: Hit = mapScene(p);
    if (h.d < EPS) {
      mat = h.mat;
      hit = true;
      break;
    }
    t = t + h.d;
    if (t > MAX_DIST) { break; }
  }
  if (!hit) {
    return sky(rd);
  }
  let p: vec3<f32> = ro + rd * t;
  let fog: f32 = 1.0 - exp(-t * 0.025);
  return mix(shade(p, rd, mat), sky(rd), fog);
}

fn tonemap(c: vec3<f32>) -> vec3<f32> {
  let x: vec3<f32> = max(c, vec3<f32>(0.0, 0.0, 0.0));
  let a: vec3<f32> = x * (2.51 * x + vec3<f32>(0.03, 0.03, 0.03));
  let b: vec3<f32> = x * (2.43 * x + vec3<f32>(0.59, 0.59, 0.59)) + vec3<f32>(0.14, 0.14, 0.14);
  return clamp(a / b, vec3<f32>(0.0, 0.0, 0.0), vec3<f32>(1.0, 1.0, 1.0));
}

fn packRgb(c: vec3<f32>) -> u32 {
  let g: vec3<f32> = pow(tonemap(c), vec3<f32>(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
  let r: u32 = u32(clamp(g.r * 255.99, 0.0, 255.0));
  let gg: u32 = u32(clamp(g.g * 255.99, 0.0, 255.0));
  let b: u32 = u32(clamp(g.b * 255.99, 0.0, 255.0));
  return (r << 16u) | (gg << 8u) | b;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let x: u32 = gid.x;
  let y: u32 = gid.y;
  if (x >= P.width || y >= P.height) { return; }

  let seed: f32 = hash31(vec3<f32>(f32(x), f32(y), f32(P.frame)));
  let jitter: vec2<f32> = vec2<f32>(seed - 0.5, hash31(vec3<f32>(f32(y), f32(P.frame), f32(x))) - 0.5) * 0.45;
  let uv: vec2<f32> = (vec2<f32>(f32(x), f32(y)) + vec2<f32>(0.5, 0.5) + jitter) / vec2<f32>(f32(P.width), f32(P.height));
  let ndc: vec2<f32> = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  let fovScale: f32 = tan(52.0 * PI / 360.0);
  let rd: vec3<f32> = normalize(camFwd() + camRight() * (ndc.x * camAspect() * fovScale) + camUp() * (ndc.y * fovScale));
  let col: vec3<f32> = render(camPos(), rd);
  outBuf[y * P.width + x] = packRgb(col);
}
