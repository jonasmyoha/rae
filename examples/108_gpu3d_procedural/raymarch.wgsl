// Real-time procedural 3D seed. The host packs camera, Material3d and
// Renderable3d component data into one read-only buffer. This compute path is a
// proving ground; the future raster renderer will retain the scene extraction
// model and replace this kernel with render-graph passes.

struct Params {
  width: u32,
  height: u32,
  objectCount: u32,
  materialCount: u32,
  frame: u32,
  quality: u32,
  _pad0: u32,
  _pad1: u32,
};

@group(0) @binding(0) var<uniform> P: Params;
@group(0) @binding(1) var<storage, read> scene: array<f32>;
@group(0) @binding(2) var<storage, read_write> outBuf: array<u32>;

const CAMERA_FLOATS: u32 = 16u;
const MATERIAL_FLOATS: u32 = 8u;
const OBJECT_FLOATS: u32 = 8u;
const PI: f32 = 3.141592653589793;

struct Material {
  color: vec3<f32>,
  metallic: f32,
  roughness: f32,
  emission: vec3<f32>,
};

struct Surface {
  distance: f32,
  material: u32,
};

fn cameraPos() -> vec3<f32> { return vec3<f32>(scene[0], scene[1], scene[2]); }
fn cameraForward() -> vec3<f32> { return vec3<f32>(scene[3], scene[4], scene[5]); }
fn cameraRight() -> vec3<f32> { return vec3<f32>(scene[6], scene[7], scene[8]); }
fn cameraUp() -> vec3<f32> { return vec3<f32>(scene[9], scene[10], scene[11]); }
fn cameraTanHalfFov() -> f32 { return scene[12]; }
fn sceneTime() -> f32 { return scene[13]; }

fn materialAt(index: u32) -> Material {
  let b = CAMERA_FLOATS + index * MATERIAL_FLOATS;
  var m: Material;
  m.color = vec3<f32>(scene[b], scene[b + 1u], scene[b + 2u]);
  m.metallic = scene[b + 3u];
  m.roughness = scene[b + 4u];
  m.emission = vec3<f32>(scene[b + 5u], scene[b + 6u], scene[b + 7u]);
  return m;
}

fn hash21(p: vec2<f32>) -> f32 {
  let p3 = fract(vec3<f32>(p.x, p.y, p.x) * 0.1031);
  let q = p3 + dot(p3, p3.yzx + vec3<f32>(33.33));
  return fract((q.x + q.y) * q.z);
}

// Quintic value noise and FBM provide a compact, derivative-friendly base for
// terrain, materials, particles and later SSAO kernel rotation textures.
fn valueNoise(p: vec2<f32>) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  return mix(mix(hash21(i), hash21(i + vec2<f32>(1.0, 0.0)), u.x),
             mix(hash21(i + vec2<f32>(0.0, 1.0)), hash21(i + vec2<f32>(1.0, 1.0)), u.x), u.y);
}

fn fbm(p0: vec2<f32>) -> f32 {
  var p = p0;
  var amplitude = 0.5;
  var sum = 0.0;
  for (var octave = 0; octave < 5; octave = octave + 1) {
    sum = sum + amplitude * valueNoise(p);
    p = mat2x2<f32>(1.6, -1.2, 1.2, 1.6) * p + vec2<f32>(11.7, 7.3);
    amplitude = amplitude * 0.5;
  }
  return sum;
}

fn sdSphere(p: vec3<f32>, radius: f32) -> f32 { return length(p) - radius; }

fn sdBox(p: vec3<f32>, bounds: vec3<f32>) -> f32 {
  let q = abs(p) - bounds;
  return length(max(q, vec3<f32>(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

fn sdTorus(p: vec3<f32>, radii: vec2<f32>) -> f32 {
  let q = vec2<f32>(length(p.xy) - radii.x, p.z);
  return length(q) - radii.y;
}

fn minSurface(a: Surface, b: Surface) -> Surface {
  if (a.distance < b.distance) { return a; }
  return b;
}

fn mapScene(p: vec3<f32>) -> Surface {
  var best = Surface(1e6, 0u);
  let objectBase = CAMERA_FLOATS + P.materialCount * MATERIAL_FLOATS;
  for (var i = 0u; i < P.objectCount; i = i + 1u) {
    let b = objectBase + i * OBJECT_FLOATS;
    let kind = u32(scene[b]);
    var local = p - vec3<f32>(scene[b + 1u], scene[b + 2u], scene[b + 3u]);
    let scale = vec3<f32>(scene[b + 4u], scene[b + 5u], scene[b + 6u]);
    let material = u32(scene[b + 7u]);
    var distance = 1e6;
    if (kind == 0u) {
      distance = sdSphere(local, scale.x);
    } else if (kind == 1u) {
      distance = sdBox(local, scale);
      if (i == 0u) {
        distance = distance + (fbm(local.xy * 0.7) - 0.5) * 0.035;
      }
    } else {
      // Rotate the torus from Z-axis into a more readable standing pose.
      local = vec3<f32>(local.x, local.z, local.y);
      distance = sdTorus(local, vec2<f32>(scale.x * 0.72, scale.x * 0.23));
    }
    best = minSurface(best, Surface(distance, material));
  }
  return best;
}

fn normalAt(p: vec3<f32>) -> vec3<f32> {
  let e = 0.0015;
  let x = mapScene(p + vec3<f32>(e, 0.0, 0.0)).distance - mapScene(p - vec3<f32>(e, 0.0, 0.0)).distance;
  let y = mapScene(p + vec3<f32>(0.0, e, 0.0)).distance - mapScene(p - vec3<f32>(0.0, e, 0.0)).distance;
  let z = mapScene(p + vec3<f32>(0.0, 0.0, e)).distance - mapScene(p - vec3<f32>(0.0, 0.0, e)).distance;
  return normalize(vec3<f32>(x, y, z));
}

fn trace(ro: vec3<f32>, rd: vec3<f32>) -> Surface {
  var travel = 0.0;
  var material = 0u;
  for (var step = 0; step < 104; step = step + 1) {
    let hit = mapScene(ro + rd * travel);
    material = hit.material;
    if (hit.distance < 0.0015 || travel > 40.0) { break; }
    travel = travel + hit.distance * 0.78;
  }
  return Surface(travel, material);
}

fn softShadow(ro: vec3<f32>, rd: vec3<f32>, maxDistance: f32) -> f32 {
  var result = 1.0;
  var travel = 0.025;
  for (var step = 0; step < 40; step = step + 1) {
    let h = mapScene(ro + rd * travel).distance;
    result = min(result, 14.0 * h / travel);
    travel = travel + clamp(h, 0.015, 0.35);
    if (h < 0.001 || travel > maxDistance) { break; }
  }
  return clamp(result, 0.0, 1.0);
}

// Geometry AO from the signed-distance field. True SSAO arrives as a separate
// depth/normal render-graph pass once the raster API exists.
fn ambientOcclusion(p: vec3<f32>, n: vec3<f32>) -> f32 {
  var occlusion = 0.0;
  var weight = 1.0;
  for (var i = 1; i <= 5; i = i + 1) {
    let distance = 0.04 * f32(i);
    occlusion = occlusion + (distance - mapScene(p + n * distance).distance) * weight;
    weight = weight * 0.55;
  }
  return clamp(1.0 - occlusion * 2.2, 0.12, 1.0);
}

fn distributionGgx(nDotH: f32, roughness: f32) -> f32 {
  let a = roughness * roughness;
  let a2 = a * a;
  let denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
  return a2 / max(PI * denom * denom, 0.0001);
}

fn geometrySchlick(nDotV: f32, roughness: f32) -> f32 {
  let k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
  return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

fn fresnelSchlick(cosTheta: f32, f0: vec3<f32>) -> vec3<f32> {
  return f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - cosTheta, 5.0);
}

fn shade(p: vec3<f32>, n: vec3<f32>, viewDir: vec3<f32>, material: Material) -> vec3<f32> {
  let lightPos = vec3<f32>(-3.0 + sin(sceneTime() * 0.35), -2.0, 5.5);
  let lightVec = lightPos - p;
  let lightDir = normalize(lightVec);
  let halfDir = normalize(viewDir + lightDir);
  let nDotL = max(dot(n, lightDir), 0.0);
  let nDotV = max(dot(n, viewDir), 0.0);
  let nDotH = max(dot(n, halfDir), 0.0);
  let vDotH = max(dot(viewDir, halfDir), 0.0);
  let roughness = clamp(material.roughness, 0.06, 1.0);
  let f0 = mix(vec3<f32>(0.04), material.color, material.metallic);
  let f = fresnelSchlick(vDotH, f0);
  let d = distributionGgx(nDotH, roughness);
  let g = geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness);
  let specular = (d * g * f) / max(4.0 * nDotV * nDotL, 0.001);
  let diffuse = (vec3<f32>(1.0) - f) * (1.0 - material.metallic) * material.color / PI;
  let shadow = softShadow(p + n * 0.006, lightDir, length(lightVec));
  let ao = ambientOcclusion(p, n);
  let direct = (diffuse + specular) * vec3<f32>(6.0, 5.4, 4.8) * nDotL * shadow;
  let sky = mix(vec3<f32>(0.035, 0.045, 0.075), vec3<f32>(0.18, 0.26, 0.38), n.z * 0.5 + 0.5);
  return direct + material.color * sky * ao + material.emission * (1.4 + 0.2 * sin(sceneTime() * 2.0));
}

fn skyColor(rd: vec3<f32>) -> vec3<f32> {
  let horizon = pow(max(0.0, 1.0 - abs(rd.z)), 5.0);
  let gradient = mix(vec3<f32>(0.012, 0.018, 0.045), vec3<f32>(0.09, 0.18, 0.31), max(rd.z, 0.0));
  let stars = step(0.9978, hash21(floor((rd.xy / max(abs(rd.z), 0.08)) * 180.0))) * max(rd.z, 0.0);
  return gradient + vec3<f32>(0.08, 0.16, 0.24) * horizon + vec3<f32>(stars);
}

fn renderSample(pixel: vec2<f32>) -> vec3<f32> {
  let resolution = vec2<f32>(f32(P.width), f32(P.height));
  var uv = (pixel * 2.0 - resolution) / resolution.y;
  uv.y = -uv.y;
  let ro = cameraPos();
  let rd = normalize(cameraForward() + cameraRight() * uv.x * cameraTanHalfFov() + cameraUp() * uv.y * cameraTanHalfFov());
  let hit = trace(ro, rd);
  if (hit.distance > 40.0) { return skyColor(rd); }
  let p = ro + rd * hit.distance;
  let n = normalAt(p);
  let color = shade(p, n, normalize(-rd), materialAt(hit.material));
  let fog = 1.0 - exp(-hit.distance * 0.035);
  return mix(color, skyColor(rd), fog);
}

fn tonemap(color: vec3<f32>) -> vec3<f32> {
  // Compact ACES-filmic approximation followed by display gamma.
  let a = 2.51;
  let b = 0.03;
  let c = 2.43;
  let d = 0.59;
  let e = 0.14;
  let mapped = clamp((color * (a * color + vec3<f32>(b))) / (color * (c * color + vec3<f32>(d)) + vec3<f32>(e)), vec3<f32>(0.0), vec3<f32>(1.0));
  return pow(mapped, vec3<f32>(1.0 / 2.2));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (gid.x >= P.width || gid.y >= P.height) { return; }
  let base = vec2<f32>(f32(gid.x), f32(gid.y));
  var color = vec3<f32>(0.0);
  if (P.quality >= 2u) {
    color = renderSample(base + vec2<f32>(0.25, 0.25));
    color = color + renderSample(base + vec2<f32>(0.75, 0.25));
    color = color + renderSample(base + vec2<f32>(0.25, 0.75));
    color = color + renderSample(base + vec2<f32>(0.75, 0.75));
    color = color * 0.25;
  } else {
    // Stable subpixel jitter gives inexpensive temporal edge smoothing while
    // moving; quality 3 uses deterministic 2x2 supersampling.
    let jitter = vec2<f32>(hash21(base + f32(P.frame)), hash21(base.yx + f32(P.frame) + 19.0)) - 0.5;
    color = renderSample(base + vec2<f32>(0.5) + jitter * 0.45);
  }
  color = tonemap(color);
  let r = u32(clamp(color.r * 255.0, 0.0, 255.0));
  let g = u32(clamp(color.g * 255.0, 0.0, 255.0));
  let b = u32(clamp(color.b * 255.0, 0.0, 255.0));
  outBuf[gid.y * P.width + gid.x] = (r << 16u) | (g << 8u) | b;
}
