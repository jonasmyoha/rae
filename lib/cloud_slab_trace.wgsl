// The slab tier's trace (lib/CloudSlab): one pixel of each 2x2 tile of the
// half-resolution grid this frame (SlabU.pattern), ray-marched through the
// slab: 8 to 16 steps (more along a flat ray), 3 steps toward the light at
// each sample, a two-lobe phase (the silver lining toward the sun), Beer's
// law with a brighter lobe for the thick parts, powder darkening on the
// edges facing the light, and one more scattering octave for the light that
// bounces inside. The step's light is integrated energy-conserving, so few
// steps do not brighten it. Out: the clouds' light (premultiplied) and how
// much of the sky they hide.
const PI = 3.14159265;  // sky_hosek.wgsl reads it
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var<uniform> S: SlabU;
@group(0) @binding(2) var fieldTex: texture_2d<f32>;
@group(0) @binding(3) var lutTex: texture_2d<f32>;
// The field (share, thick) at a point of the plane, blended between texels;
// none outside the map
fn slabField(world: vec2<f32>) -> vec4<f32> {
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
// The profile strip at a height in the column (0 base .. 1 top): its red
// is stratus, green stratocumulus, blue cumulus; the kind blends them
fn slabProfile(height: f32, kind: f32) -> f32 {
  if (height <= 0.0 || height >= 1.0) { return 0.0; }
  let x = height * 127.0;
  let i = i32(floor(x));
  let f = x - floor(x);
  let p = mix(textureLoad(lutTex, vec2<i32>(i, 0), 0).xyz, textureLoad(lutTex, vec2<i32>(min(i + 1, 127), 0), 0).xyz, f);
  if (kind < 0.5) { return mix(p.x, p.y, kind * 2.0); }
  return mix(p.y, p.z, kind * 2.0 - 1.0);
}
// The cloud's density at a point of the slab (0 .. 1): the footprint's
// share, the column as high as the footprint is thick (cumulus towers up
// where it is thick), its profile, and a fine noise eating its edges
fn slabDensity(p: vec3<f32>, base: f32, depth: f32, kind: f32) -> f32 {
  let field = slabField(p.xy);
  if (field.x <= 0.01) { return 0.0; }
  // a cumulus column as high as its footprint is thick, rounded by where in
  // its puff the point is (a dome: low at its rim, tallest in its middle);
  // a stratus sheet the slab's whole depth
  let dome = sqrt(clamp(1.0 - dot(field.zw, field.zw), 0.0, 1.0));
  let column = mix(1.0, clamp(field.y * (0.35 + 0.75 * dome), 0.0, 1.0), kind);
  if (column <= 0.02) { return 0.0; }
  let height = (p.z - base) / (depth * column);
  let profile = slabProfile(height, kind);
  if (profile <= 0.0) { return 0.0; }
  let q = p.xy / (260.0 * max(L.cloudShape.w, 0.1)) + vec2<f32>(p.z / 170.0, -p.z / 230.0);
  let erosion = skyCloudNoise(q) * 0.65 + skyCloudNoise(q * 2.7 + vec2<f32>(3.1, 7.7)) * 0.35;
  return clamp(field.x * profile * (0.55 + 0.6 * erosion) - 0.2 * (1.0 - erosion), 0.0, 1.0);
}
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  skySlabActive = true;
  let halfPx = floor(pos.xy) * 2.0 + S.pattern.xy + 0.5;
  let dir = cloudSlabDir(halfPx, S.sizes.zw);
  let cover = clamp(L.clouds.x, 0.0, 1.0);
  if (cover <= 0.001 || dir.z <= 0.02) { return vec4<f32>(0.0); }
  let kind = clamp(L.cloudShape.x, 0.0, 1.0);
  let dark = clamp(L.clouds.y, 0.0, 1.0);
  let base = max(L.cloudShape.z, 100.0);
  let depth = cloudSlabThickness();
  let eye = L.camPos.xyz;
  // the ray's stretch inside the slab (at most 30 km)
  let t0 = max((base - eye.z) / dir.z, 0.0);
  let t1 = min((base + depth - eye.z) / dir.z, t0 + 30000.0);
  if (t1 <= t0) { return vec4<f32>(0.0); }
  let steps = i32(mix(16.0, 8.0, smoothstep(0.1, 0.7, dir.z)));
  let dt = (t1 - t0) / f32(steps);
  let jitter = fract(sin(dot(pos.xy + S.pattern.z * 7.3, vec2<f32>(12.9898, 78.233))) * 43758.5453);
  // the light: the sun's (the key light, dimmed through the slab itself and
  // its darkness), the sky's from above, the low sun's reddened glow
  let toSun = normalize(-L.sunDir.xyz);
  let cosSun = dot(dir, toSun);
  let phase = max(skyCloudPhase(cosSun, 0.55), 0.6 * skyCloudPhase(cosSun, 0.92)) * 4.0 * PI;
  let sun = L.sunColor.rgb * (1.0 - 0.85 * dark);
  let up = skyColor(vec3<f32>(0.0, 0.0, 1.0)) * (1.0 - 0.5 * cover);
  let bright = dot(up, vec3<f32>(0.2126, 0.7152, 0.0722));
  let lowSun = smoothstep(-0.08, 0.0, toSun.z) * (1.0 - smoothstep(0.06, 0.3, toSun.z));
  let airPath = min(1.0 / (max(toSun.z, 0.0) + 0.025), 40.0);
  let glow = bright * 2.6 * exp(-vec3<f32>(0.012, 0.03, 0.075) * airPath) * lowSun * (1.0 - 0.85 * dark);
  let ambient = mix(vec3<f32>(bright), up, 0.25) * (1.0 - 0.62 * dark);
  // how dense a full cloud is (per meter): a stratus sheet thinner but
  // denser, a rain cloud denser still
  let sigmaFull = mix(0.012, 0.006, kind) * (1.0 + 1.5 * dark);
  let lightStep = depth * 0.22;
  var transmit = 1.0;
  var light = vec3<f32>(0.0);
  for (var s = 0; s < 16; s = s + 1) {
    if (s >= steps || transmit < 0.02) { break; }
    let t = t0 + (f32(s) + jitter) * dt;
    let p = eye + dir * t;
    let density = slabDensity(p, base, depth, kind);
    if (density <= 0.001) { continue; }
    let sigma = density * sigmaFull;
    // toward the light: three steps through the slab
    var toward = 0.0;
    for (var l = 1; l <= 3; l = l + 1) {
      toward = toward + slabDensity(p + toSun * lightStep * f32(l), base, depth, kind);
    }
    let tauLight = toward * sigmaFull * lightStep;
    // Beer's law, with a brighter lobe that keeps a thick middle from going
    // black, the powder darkening where the light only just entered, and one
    // more octave of the light scattered inside (less dense, less forward)
    let beer = max(exp(-tauLight), 0.7 * exp(-0.25 * tauLight));
    let powder = 1.0 - 0.6 * exp(-2.0 * sigma * dt * 8.0);
    let octave = 0.5 * exp(-0.5 * tauLight);
    let heightIn = clamp((p.z - base) / depth, 0.0, 1.0);
    let scattered = sun * (beer * phase + octave) * powder * 0.18 + glow * (0.5 + 0.4 * beer) * (1.0 + phase * 0.1)
      + ambient * (0.55 + 0.6 * heightIn);
    let through = exp(-sigma * dt);
    light = light + transmit * scattered * (1.0 - through);
    transmit = transmit * through;
  }
  // far away the haze takes some of it, toward the sky's own color there
  let haze = 1.0 - exp(-t0 / 45000.0);
  let skyThere = skyColor(dir);
  let alpha = (1.0 - transmit) * (1.0 - 0.6 * haze);
  return vec4<f32>(mix(light, skyThere * (1.0 - transmit), 0.6 * haze), alpha);
}
