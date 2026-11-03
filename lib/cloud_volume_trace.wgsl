// The volumetric cloud tier's trace (lib/CloudSlab, volumetric mode;
// docs/cloud-rendering-research.md §8.1 tier 3): as the slab's trace, one
// pixel of each 2x2 tile of the half-resolution grid a frame, but through a
// deep cloud volume shaped by 3D noise. The weather map is the field map
// (the layer's 2D field: coverage, how thick, the puff), the shape in height
// the profile strip; a low-frequency 3D noise rounds the towers into
// billows, a fine one erodes their edges where the cloud is thin. The march
// takes SlabU.pattern.w steps (48 at High, 64 at Ultra), cheap ones until it
// meets a cloud; at each sample 5 light samples in a cone toward the sun
// (the last one far, for the shadow of distant towers), two scattering
// octaves (Wrenninge: each less dense and less forward), the two-lobe phase,
// powder darkening and the sky's light growing with height. Out: the
// clouds' light (premultiplied) and how much of the sky they hide.
const PI = 3.14159265;  // sky_hosek.wgsl reads it
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var<uniform> S: SlabU;
@group(0) @binding(2) var fieldTex: texture_2d<f32>;
@group(0) @binding(3) var lutTex: texture_2d<f32>;
fn volumeField(world: vec2<f32>) -> vec4<f32> {
  let at = (world - cloudFieldOrigin()) / cloudFieldSpan * cloudFieldTexels - 0.5;
  if (at.x < 0.0 || at.y < 0.0 || at.x > cloudFieldTexels - 2.0 || at.y > cloudFieldTexels - 2.0) {
    return vec4<f32>(0.0);
  }
  let cell = vec2<i32>(floor(at));
  let f = at - floor(at);
  let a = textureLoad(fieldTex, cell, 0);
  let b = textureLoad(fieldTex, cell + vec2<i32>(1, 0), 0);
  let c = textureLoad(fieldTex, cell + vec2<i32>(0, 1), 0);
  let d = textureLoad(fieldTex, cell + vec2<i32>(1, 1), 0);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
fn volumeProfile(height: f32, kind: f32) -> f32 {
  if (height <= 0.0 || height >= 1.0) { return 0.0; }
  let x = height * 127.0;
  let i = i32(floor(x));
  let f = x - floor(x);
  let p = mix(textureLoad(lutTex, vec2<i32>(i, 0), 0).xyz, textureLoad(lutTex, vec2<i32>(min(i + 1, 127), 0), 0).xyz, f);
  if (kind < 0.5) { return mix(p.x, p.y, kind * 2.0); }
  return mix(p.y, p.z, kind * 2.0 - 1.0);
}
// 3D value noise (0 .. 1), smooth
fn volumeHash(p: vec3<f32>) -> f32 {
  var q = fract(p * 0.1031);
  q = q + dot(q, q.zyx + 31.32);
  return fract((q.x + q.y) * q.z);
}
fn volumeNoise(p: vec3<f32>) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let u = f * f * (3.0 - 2.0 * f);
  let n000 = volumeHash(i);
  let n100 = volumeHash(i + vec3<f32>(1.0, 0.0, 0.0));
  let n010 = volumeHash(i + vec3<f32>(0.0, 1.0, 0.0));
  let n110 = volumeHash(i + vec3<f32>(1.0, 1.0, 0.0));
  let n001 = volumeHash(i + vec3<f32>(0.0, 0.0, 1.0));
  let n101 = volumeHash(i + vec3<f32>(1.0, 0.0, 1.0));
  let n011 = volumeHash(i + vec3<f32>(0.0, 1.0, 1.0));
  let n111 = volumeHash(i + vec3<f32>(1.0, 1.0, 1.0));
  return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}
// Billows: ridged noise, round heaps with creases between them
fn volumeBillows(p: vec3<f32>) -> f32 {
  let a = 1.0 - abs(volumeNoise(p) * 2.0 - 1.0);
  let b = 1.0 - abs(volumeNoise(p * 2.03 + vec3<f32>(3.1, 1.7, 5.3)) * 2.0 - 1.0);
  return a * 0.65 + b * 0.35;
}
// The cloud's density at a point (0 .. 1), in three levels of detail: 0 the
// shape alone (the footprint and the profile: the search through clear air
// and the far light samples), 1 with the billows (the near light samples),
// 2 with the edge detail too (the view samples)
fn volumeDensity(p: vec3<f32>, base: f32, depth: f32, kind: f32, level: i32) -> f32 {
  let field = volumeField(p.xy);
  if (field.x <= 0.01) { return 0.0; }
  let dome = sqrt(clamp(1.0 - dot(field.zw, field.zw), 0.0, 1.0));
  // the square root rounds the column's top (linear, it would be a cone)
  let column = mix(1.0, sqrt(clamp(field.y * (0.3 + 0.8 * dome), 0.0, 1.0)), kind);
  if (column <= 0.02) { return 0.0; }
  let height = (p.z - base) / (depth * column);
  let profile = volumeProfile(height, kind);
  if (profile <= 0.0) { return 0.0; }
  let scale = max(L.cloudShape.w, 0.1);
  let wind = vec3<f32>(L.clouds.z * 1600.0 * scale, L.clouds.z * 560.0 * scale, 0.0);
  let shape = field.x * profile;
  if (level == 0) { return shape; }
  // the billows round it (towers in cumulus, soft swells in a sheet)
  let billows = volumeBillows((p + wind) / (480.0 * scale)) * 0.7 + volumeBillows((p + wind) / (1300.0 * scale)) * 0.3;
  // Schneider's remap: the billows from (1 - shape) up to 1, so inside a
  // cloud they model its body and at its thin edge only their peaks remain
  let soft = mix(0.55, 1.0, kind);
  let floorAt = 1.0 - shape;
  var density = clamp((mix(1.0, billows, soft) - floorAt) / max(1.0 - floorAt, 0.05), 0.0, 1.0) * shape;
  if (level >= 2 && density > 0.0 && density < 0.6) {
    // the edge detail: where the cloud is thin, a fine noise eats it, more
    // at its base (wisps) than at its top (crisp heaps)
    let detail = volumeNoise((p + wind * 1.4) / (90.0 * scale)) * 0.6 + volumeNoise((p + wind * 1.4) / (40.0 * scale)) * 0.4;
    let erode = mix(0.45, 0.2, clamp(height, 0.0, 1.0)) * (1.0 - density * 1.5);
    density = clamp(density - erode * (1.0 - detail), 0.0, 1.0);
  }
  return density;
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  skySlabActive = true;
  let halfPx = floor(pos.xy) * 2.0 + S.pattern.xy + 0.5;
  let dir = cloudSlabDir(halfPx, S.sizes.zw);
  let cover = clamp(L.clouds.x, 0.0, 1.0);
  if (cover <= 0.001 || dir.z <= 0.015) { return vec4<f32>(0.0); }
  let kind = clamp(L.cloudShape.x, 0.0, 1.0);
  let dark = clamp(L.clouds.y, 0.0, 1.0);
  let base = max(L.cloudShape.z, 100.0);
  // deeper than the slab: cumulus towers 2 km and more, a storm's higher
  let depth = mix(350.0, 1400.0, kind) * (1.0 + 1.5 * max(dark - 0.4, 0.0)) * max(L.cloudShape.w, 0.1);
  let eye = L.camPos.xyz;
  let t0 = max((base - eye.z) / dir.z, 0.0);
  let t1 = min((base + depth - eye.z) / dir.z, t0 + 22000.0);
  if (t1 <= t0) { return vec4<f32>(0.0); }
  let steps = clamp(i32(S.pattern.w), 16, 128);
  let dt = (t1 - t0) / f32(steps);
  let jitter = fract(sin(dot(pos.xy + S.pattern.z * 7.3, vec2<f32>(12.9898, 78.233))) * 43758.5453);
  let toSun = normalize(-L.sunDir.xyz);
  let cosSun = dot(dir, toSun);
  let phase = max(skyCloudPhase(cosSun, 0.6), 0.55 * skyCloudPhase(cosSun, 0.93)) * 4.0 * PI;
  let phase2 = max(skyCloudPhase(cosSun, 0.3), 0.3 * skyCloudPhase(cosSun, 0.6)) * 4.0 * PI;
  let sun = L.sunColor.rgb * (1.0 - 0.85 * dark);
  let up = skyColor(vec3<f32>(0.0, 0.0, 1.0)) * (1.0 - 0.5 * cover);
  let bright = dot(up, vec3<f32>(0.2126, 0.7152, 0.0722));
  let lowSun = smoothstep(-0.08, 0.0, toSun.z) * (1.0 - smoothstep(0.06, 0.3, toSun.z));
  let airPath = min(1.0 / (max(toSun.z, 0.0) + 0.025), 40.0);
  let glow = bright * 2.6 * exp(-vec3<f32>(0.012, 0.03, 0.075) * airPath) * lowSun * (1.0 - 0.85 * dark);
  let ambient = mix(vec3<f32>(bright), up, 0.25) * (1.0 - 0.62 * dark);
  let sigmaFull = mix(0.018, 0.012, kind) * (1.0 + 1.5 * dark);
  let lightStep = depth * 0.12;
  var transmit = 1.0;
  var light = vec3<f32>(0.0);
  var inside = false;
  var s = 0;
  var t = t0 + jitter * dt;
  var step = dt;
  loop {
    if (s >= steps * 2 || transmit < 0.02 || t > t1) { break; }
    let p = eye + dir * t;
    // cheap whole steps through clear air; at the first cloud, a step back
    // and half steps inside it, so its edge does not stair-step
    let coarse = volumeDensity(p, base, depth, kind, 0);
    if (coarse <= 0.001) {
      t = t + step;
      s = s + 1;
      continue;
    }
    if (!inside) {
      inside = true;
      step = dt * 0.5;
      t = max(t - step, t0);
      s = s + 1;
      continue;
    }
    let density = volumeDensity(p, base, depth, kind, 2);
    if (density > 0.001) {
      let sigma = density * sigmaFull;
      // the light: 5 samples in a cone toward the sun, the last one far
      var toward = 0.0;
      for (var l = 1; l <= 4; l = l + 1) {
        let offset = vec3<f32>(sin(f32(l) * 2.4), cos(f32(l) * 2.4), 0.0) * lightStep * 0.15 * f32(l);
        toward = toward + volumeDensity(p + toSun * lightStep * f32(l) + offset, base, depth, kind, select(0, 1, l <= 2));
      }
      toward = toward + 2.5 * volumeDensity(p + toSun * lightStep * 9.0, base, depth, kind, 0);
      let tauLight = toward * sigmaFull * lightStep;
      // two octaves (the second half as dense, half as forward), Beer with
      // a brighter lobe, powder where the light only just entered
      let beer = max(exp(-tauLight), 0.7 * exp(-0.25 * tauLight));
      let octave2 = 0.5 * exp(-0.5 * tauLight);
      let powder = 1.0 - 0.6 * exp(-2.0 * sigma * step * 6.0);
      let heightIn = clamp((p.z - base) / depth, 0.0, 1.0);
      let scattered = sun * (beer * phase + octave2 * phase2) * powder * 0.16
        + glow * (0.5 + 0.4 * beer) + ambient * (0.45 + 0.75 * heightIn);
      let through = exp(-sigma * step);
      light = light + transmit * scattered * (1.0 - through);
      transmit = transmit * through;
    }
    t = t + step;
    s = s + 1;
  }
  let haze = 1.0 - exp(-t0 / 45000.0);
  let skyThere = skyColor(dir);
  let alpha = (1.0 - transmit) * (1.0 - 0.6 * haze);
  return vec4<f32>(mix(light, skyThere * (1.0 - transmit), 0.6 * haze), alpha);
}
