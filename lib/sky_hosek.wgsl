// The Hosek-Wilkie sky evaluation shared by the deferred light pass (a PART of
// lib/deferred_light.wgsl's composition). The legacy forward renderer keeps its
// own copy in compiler/runtime/runtime_sky_wgsl.h for the pipeline it builds in C.
fn preethamF(A: f32, B: f32, C: f32, D: f32, E: f32, ct: f32, g: f32) -> f32 {
  let c = max(ct, 0.01);
  let cg = cos(g);
  return (1.0 + A * exp(B / c)) * (1.0 + C * exp(D * g) + E * cg * cg);
}
fn skyProcedural(dir: vec3<f32>, toSun: vec3<f32>, t: f32) -> vec3<f32> {
  let g = acos(clamp(dot(dir, toSun), -1.0, 1.0));
  let ts = acos(clamp(max(toSun.z, 0.01), -1.0, 1.0));
  let Ay = 0.1787 * t - 1.4630; let By = -0.3554 * t + 0.4275;
  let Cy = -0.0227 * t + 5.3251; let Dy = 0.1206 * t - 2.5771;
  let Ey = -0.0670 * t + 0.3703;
  let Ax = -0.0193 * t - 0.2592; let Bx = -0.0665 * t + 0.0008;
  let Cx = -0.0004 * t + 0.2125; let Dx = -0.0641 * t - 0.8989;
  let Ex = -0.0033 * t + 0.0452;
  let Az = -0.0167 * t - 0.2608; let Bz = -0.0950 * t + 0.0092;
  let Cz = -0.0079 * t + 0.2102; let Dz = -0.0441 * t - 1.6537;
  let Ez = -0.0109 * t + 0.0529;
  let fy = preethamF(Ay,By,Cy,Dy,Ey, dir.z, g) / preethamF(Ay,By,Cy,Dy,Ey, 1.0, ts);
  let fx = preethamF(Ax,Bx,Cx,Dx,Ex, dir.z, g) / preethamF(Ax,Bx,Cx,Dx,Ex, 1.0, ts);
  let fz = preethamF(Az,Bz,Cz,Dz,Ez, dir.z, g) / preethamF(Az,Bz,Cz,Dz,Ez, 1.0, ts);
  let ts2 = ts * ts; let ts3 = ts2 * ts;
  let chi = (4.0 / 9.0 - t / 120.0) * (PI - 2.0 * ts);
  let zY = max((4.0453 * t - 4.9710) * tan(chi) - 0.2155 * t + 2.4192, 0.0);
  let zx = (0.00166*ts3 - 0.00375*ts2 + 0.00209*ts) * t * t
         + (-0.02903*ts3 + 0.06377*ts2 - 0.03202*ts + 0.00394) * t
         + (0.11693*ts3 - 0.21196*ts2 + 0.06052*ts + 0.25886);
  let zy = (0.00275*ts3 - 0.00610*ts2 + 0.00317*ts) * t * t
         + (-0.04214*ts3 + 0.08970*ts2 - 0.04153*ts + 0.00516) * t
         + (0.15346*ts3 - 0.26756*ts2 + 0.06670*ts + 0.26688);
  // zY is in kcd/m2: the result is in cd/m2 (LightUnits)
  let xx = zx * fx; let yy = max(zy * fz, 0.0001); let YY = zY * fy * 1000.0;
  let X = xx * YY / yy; let Z = (1.0 - xx - yy) * YY / yy;
  let rgb = vec3<f32>( 3.2406*X - 1.5372*YY - 0.4986*Z,
                      -0.9689*X + 1.8758*YY + 0.0415*Z,
                       0.0557*X - 0.2040*YY + 1.0570*Z);
  return max(rgb, vec3<f32>(0.0));
}
fn skyStylized(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {
  let h = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);
  let bands = max(L.skyZenith.w, 1.0);
  let s = h * bands;
  let i = floor(s);
  let band = (i + smoothstep(0.35, 0.65, s - i)) / bands;
  var c = mix(L.skyHorizon.rgb, L.skyZenith.rgb, band);
  let g = clamp(dot(dir, toSun), 0.0, 1.0);
  c = c + L.sunColor.rgb * pow(g, 6.0) * 0.35;
  return c;
}
fn hosekChannel(base: i32, cosTheta: f32, cosGamma: f32, gamma: f32) -> f32 {
  let c0 = L.hosek[base];
  let c1 = L.hosek[base + 1];
  let c2 = L.hosek[base + 2];
  let expM = exp(c1.x * gamma);
  let rayM = cosGamma * cosGamma;
  let mieDen = 1.0 + c2.x * c2.x - 2.0 * c2.x * cosGamma;
  var mieM = 0.0;
  if (mieDen > 0.0) { mieM = (1.0 + rayM) / (mieDen * sqrt(mieDen)); }
  let ct = max(cosTheta, 0.0);
  let zen = sqrt(ct);
  let widening = 1.0 + c0.x * exp(c0.y / (ct + 0.01));
  let body = c0.z + c0.w * expM + c1.y * rayM + c1.z * mieM + c1.w * zen;
  return widening * body;
}
fn skyHosek(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {
  let cosTheta = clamp(dir.z, -1.0, 1.0);
  let cosGamma = clamp(dot(dir, toSun), -1.0, 1.0);
  let gamma = acos(cosGamma);
  let r = hosekChannel(0, cosTheta, cosGamma, gamma) * L.hosek[2].y;
  let g = hosekChannel(3, cosTheta, cosGamma, gamma) * L.hosek[5].y;
  let b = hosekChannel(6, cosTheta, cosGamma, gamma) * L.hosek[8].y;
  let below = smoothstep(0.0, 0.06, dir.z);
  return mix(L.skyHorizon.rgb, max(vec3<f32>(r, g, b), vec3<f32>(0.0)), below);
}
// The moon's disc at `dir`: a sphere lit on the side that faces the sun, so
// its phase is the geometry of the two directions (full when the sun is
// behind the viewer, dark when it is behind the moon), plus a faint earthshine
// on the dark side. 0 off the disc.
fn moonDisc(dir: vec3<f32>, toMoon: vec3<f32>, toSun: vec3<f32>, radius: f32) -> f32 {
  let ca = dot(dir, toMoon);
  let cutoff = cos(radius);
  if (ca < cutoff) { return 0.0; }
  let q = (dir - toMoon * ca) / max(sin(radius), 1e-5);
  let z = sqrt(max(1.0 - dot(q, q), 0.0));
  let normal = q - toMoon * z;
  let lit = smoothstep(-0.04, 0.12, dot(normal, toSun));
  let edge = smoothstep(cutoff, mix(cutoff, 1.0, 0.12), ca);
  // A little mottling: the maria are darker than the highlands
  let mare = 0.82 + 0.18 * sin(q.x * 9.0 + 1.3) * sin(q.y * 7.0 - 0.6);
  return edge * (lit * mare + 0.025);
}
// How much of `dir` a disc of angular `radius` toward `toward` hides, 0..1
// with a soft edge (a moon in front of the sun: an eclipse)
fn discCover(dir: vec3<f32>, toward: vec3<f32>, radius: f32) -> f32 {
  if (radius <= 0.0) { return 0.0; }
  let cutoff = cos(radius);
  return smoothstep(cutoff, mix(cutoff, 1.0, 0.04), dot(dir, normalize(toward)));
}
// How much of `dir` every moon hides
fn moonsCover(dir: vec3<f32>) -> f32 {
  var cover = 0.0;
  if (L.moon.w > 0.0) { cover = discCover(dir, L.moon.xyz, max(L.skySun.w, 0.001)); }
  for (var i = 0; i < 3; i = i + 1) {
    cover = max(cover, discCover(dir, L.moons[i * 2].xyz, L.moons[i * 2].w));
  }
  return cover;
}
// The earth's shadow on the earth's moon at `dir` (a lunar eclipse): the
// light left, 1 outside it; in the penumbra dimmer toward the umbra, in the
// umbra a dull red, darkest at its center (the sunlight the earth's
// atmosphere bends in, its blue scattered away). Distances as chords, which
// keep their precision at these small angles.
fn earthShadow(dir: vec3<f32>) -> vec3<f32> {
  let umbra = L.moonShadow.w;
  if (umbra <= 0.0) { return vec3<f32>(1.0); }
  let penumbra = max(L.moons[3].w, umbra * 1.01);
  let d = length(dir - normalize(L.moonShadow.xyz));
  let half = mix(0.25, 1.0, pow(clamp((d - umbra) / (penumbra - umbra), 0.0, 1.0), 0.6));
  let core = clamp(d / umbra, 0.0, 1.0);
  let red = vec3<f32>(0.9, 0.32, 0.12) * mix(0.035, 0.11, core * core);
  let inside = 1.0 - smoothstep(umbra * 0.97, umbra * 1.03, d);
  return mix(vec3<f32>(half), red, inside);
}
// The clouds (lib/SkyClouds through L.clouds and L.cloudShape; the cheap
// layer of docs/cloud-rendering-research.md §8.1): a 2D cloud field on a
// plane at the clouds' height, seen along `dir`, drifting with the clock.
// Its kind (cloudShape.x) blends an even stratus sheet (0) into separate
// rounded cumulus puffs (1); cirrus streaks (cloudShape.y) lie high above
// it; a band of cumulus stands on the horizon on a day with cumulus. The
// cover sets how much of it is cloud, the darkness how gray (white without
// rain or a storm). Lit by the sky round it and by the sun: thin parts let
// the sun through, a puff's sunward side is lighter, the edges toward the
// sun shine (a forward-scattering phase, the silver lining), the band is
// gray at its flat base and white at its tops. Returns the cloud's color
// and its share in w.
fn skyCloudHash(p: vec2<f32>) -> f32 {
  var p3 = fract(vec3<f32>(p.xyx) * 0.1031);
  p3 = p3 + dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
fn skyCloudHash3(p: vec2<f32>) -> vec3<f32> {
  var p3 = fract(vec3<f32>(p.xyx) * vec3<f32>(0.1031, 0.1030, 0.0973));
  p3 = p3 + dot(p3, p3.yxz + 33.33);
  return fract((p3.xxy + p3.yzz) * p3.zyx);
}
fn skyCloudNoise(p: vec2<f32>) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let u = f * f * (3.0 - 2.0 * f);
  let a = skyCloudHash(i);
  let b = skyCloudHash(i + vec2<f32>(1.0, 0.0));
  let c = skyCloudHash(i + vec2<f32>(0.0, 1.0));
  let d = skyCloudHash(i + vec2<f32>(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
fn skyCloudFbm(p: vec2<f32>) -> f32 {
  return skyCloudNoise(p) * 0.5 + skyCloudNoise(p * 2.13 + vec2<f32>(5.2, 1.3)) * 0.3
    + skyCloudNoise(p * 4.71 + vec2<f32>(2.7, 8.1)) * 0.2;
}
// Henyey-Greenstein: how much light scatters forward by the angle's cosine
fn skyCloudPhase(cosAngle: f32, g: f32) -> f32 {
  let g2 = g * g;
  return (1.0 - g2) / (12.566 * pow(max(1.0 + g2 - 2.0 * g * cosAngle, 1e-4), 1.5));
}
// Rounded puffs, one in a cell, as many as the cover asks for: x how thick
// (a dome: 1 at its middle, 0 at its rim, below 0 outside it; -1 where no
// puff is near), yz where in its puff the point is (from its middle, in its
// radius)
fn skyCloudPuffs(p: vec2<f32>, cover: f32) -> vec3<f32> {
  let i = floor(p);
  let f = fract(p);
  var best = vec3<f32>(-1.0, 0.0, 0.0);
  for (var y = -1; y <= 1; y = y + 1) {
    for (var x = -1; x <= 1; x = x + 1) {
      let cell = vec2<f32>(f32(x), f32(y));
      let h = skyCloudHash3(i + cell);
      let radius = (0.2 + 0.4 * h.z) * mix(0.75, 1.6, cover);
      let away = (f - cell - 0.2 - 0.6 * h.xy) / radius;
      let present = smoothstep(h.z - 0.15, h.z + 0.15, cover * 1.3);
      let dome = mix(-1.0, 1.0 - dot(away, away), present);
      if (dome > best.x) { best = vec3<f32>(dome, away); }
    }
  }
  return best;
}
// One cloud layer over another (colors not premultiplied)
fn skyCloudOver(top: vec4<f32>, under: vec4<f32>) -> vec4<f32> {
  let a = top.w + under.w * (1.0 - top.w);
  if (a <= 1e-4) { return vec4<f32>(0.0); }
  return vec4<f32>((top.rgb * top.w + under.rgb * under.w * (1.0 - top.w)) / a, a);
}
// The cloud layer at a point of its plane (skyCloudPoint): its share (0
// clear .. 1 cloud), how thick, where in its puff the point is (from the
// puff's middle, in its radius) and the stratus sheet's slow noise. The
// kind (cloudShape.x) blends the even stratus sheet into the cumulus puffs,
// with smaller puffs heaped on them (cauliflower), closing into a sheet at
// full cover. Seen at a slant (`slant` 0 .. 1) along `viewFlat`, a puff's
// far side is its flat base, cut straight; the sun's shadow sees it from
// straight along the light (slant 0).
struct SkyCloudSample {
  share: f32,
  thick: f32,
  away: vec2<f32>,
  sheetNoise: f32,
};
// A point over the world (meters, on the clouds' plane) in the layer's own
// units: a cell is a cumulus puff's spacing (about 1.6 km at size 1),
// drifting with the clock
fn skyCloudPoint(world: vec2<f32>) -> vec2<f32> {
  return world / (1600.0 * max(L.cloudShape.w, 0.1)) + vec2<f32>(L.clouds.z, L.clouds.z * 0.35);
}
fn skyCloudSample(p: vec2<f32>, viewFlat: vec2<f32>, slant: f32) -> SkyCloudSample {
  let cover = clamp(L.clouds.x, 0.0, 1.0);
  let kind = clamp(L.cloudShape.x, 0.0, 1.0);
  let detail = skyCloudFbm(p * 3.1 + vec2<f32>(1.7, 4.4));
  let sheetNoise = skyCloudFbm(p * vec2<f32>(1.4, 2.4));
  let threshold = mix(0.78, -0.1, cover);
  let sheet = smoothstep(threshold, threshold + 0.25, sheetNoise * 0.85 + detail * 0.15);
  let sheetThick = smoothstep(threshold + 0.1, threshold + 0.6, sheetNoise);
  let puff = skyCloudPuffs(p, cover);
  let bud = skyCloudPuffs(p * 2.7 + vec2<f32>(3.3, 1.9), 0.8);
  let flatBase = 1.0 - slant * smoothstep(0.35, 0.55, dot(puff.yz, viewFlat));
  let lump = max(puff.x, 0.0) * 0.6 + max(bud.x, 0.0) * 0.75 * smoothstep(-0.5, 0.2, puff.x);
  let body = max((lump - 0.45 * (1.0 - detail)) * flatBase, (cover - 0.85) / 0.15 * detail);
  let heap = smoothstep(0.0, 0.1, body);
  let heapThick = smoothstep(0.05, 0.6, body);
  return SkyCloudSample(mix(sheet, heap, kind), mix(sheetThick, heapThick, kind), puff.yz, sheetNoise);
}
// How much of the key light gets through the clouds at a point of their
// plane (skyCloudPoint): all of it in a gap, a fifth to a half under a
// cloud (less under a thick one and a dark one). The clouds' shadow on the
// ground (cloud_shadow.wgsl) is made of it.
fn skyCloudThrough(layer: SkyCloudSample) -> f32 {
  let dark = clamp(L.clouds.y, 0.0, 1.0);
  return 1.0 - layer.share * clamp(0.5 + 0.25 * layer.thick + 0.05 * dark, 0.0, 0.8);
}
fn skyCloudSunThrough(p: vec2<f32>) -> f32 {
  if (L.clouds.x <= 0.001) { return 1.0; }
  return skyCloudThrough(skyCloudSample(p, vec2<f32>(1.0, 0.0), 0.0));
}
// The clouds' shadow map (lib/CloudShadow, drawn by cloud_shadow.wgsl, read
// by deferred_shade.wgsl cloudSunLight): cloudShadowTexels square texels
// over cloudShadowSpan meters of the clouds' plane, round the point of the
// plane the key light reaches the eye through, snapped to a texel so the
// shadows do not crawl as the eye moves; one more row holds the map's
// average (texel 0: the light let through, and the share of cloud)
const cloudShadowTexels: f32 = 256.0;
const cloudShadowSpan: f32 = 8000.0;
// The way to the key light, never flatter than 0.1 up (a low sun's shadows
// would reach kilometers away)
fn cloudShadowToLight() -> vec3<f32> {
  let toLight = normalize(-L.sunDir.xyz);
  return vec3<f32>(toLight.xy, max(toLight.z, 0.1));
}
// Where the ray from `wpos` toward the key light meets the clouds' plane
fn cloudShadowPlane(wpos: vec3<f32>) -> vec2<f32> {
  let toLight = cloudShadowToLight();
  return wpos.xy + toLight.xy / toLight.z * max(max(L.cloudShape.z, 100.0) - wpos.z, 0.0);
}
// The map's corner on the plane (meters)
fn cloudShadowOrigin() -> vec2<f32> {
  let texel = cloudShadowSpan / cloudShadowTexels;
  return floor(cloudShadowPlane(L.camPos.xyz) / texel) * texel - vec2<f32>(0.5 * cloudShadowSpan);
}
fn skyClouds(dir: vec3<f32>, sky: vec3<f32>, toSun: vec3<f32>) -> vec4<f32> {
  let cover = clamp(L.clouds.x, 0.0, 1.0);
  let cirrusAmount = clamp(L.cloudShape.y, 0.0, 1.0);
  if ((cover <= 0.001 && cirrusAmount <= 0.001) || dir.z <= -0.02) { return vec4<f32>(0.0); }
  let kind = clamp(L.cloudShape.x, 0.0, 1.0);
  let height = max(L.cloudShape.z, 100.0);
  let size = max(L.cloudShape.w, 0.1);
  let dark = clamp(L.clouds.y, 0.0, 1.0);
  let drift = vec2<f32>(L.clouds.z, L.clouds.z * 0.35);
  // the light: the sky's (whiter than its blue; under a low sun, sunrise or
  // sunset, its own colors, and the sun lights the clouds from the side
  // and below in its reddened color, a while before and after it is up),
  // the sun's (the key light, exposed; the moon at night) and how it
  // scatters toward the eye
  // the sky's and the sun's light reach the clouds through the layer
  // itself: an overcast's own light is the dimmer light under it
  let skyUnder = sky * (1.0 - 0.5 * cover);
  let bright = dot(skyUnder, vec3<f32>(0.2126, 0.7152, 0.0722));
  let toSunZ = toSun.z;
  let lowSun = smoothstep(-0.08, 0.0, toSunZ) * (1.0 - smoothstep(0.06, 0.3, toSunZ));
  let ambient = mix(vec3<f32>(bright), skyUnder, 0.2 + 0.6 * lowSun) * (1.0 + 0.35 * lowSun)
    * vec3<f32>(0.98, 0.99, 1.02);
  let sun = L.sunColor.rgb * (1.0 - 0.85 * dark) * (1.0 - 0.8 * cover);
  // the low sun's light on the clouds: through the long air path, reddened
  // (the clouds see it a while after the ground no longer does), as bright
  // as a few times the sky round them
  let airPath = min(1.0 / (max(toSunZ, 0.0) + 0.025), 40.0);
  let glow = bright * 2.6 * exp(-vec3<f32>(0.012, 0.03, 0.075) * airPath) * lowSun * (1.0 - 0.85 * dark);
  let cosSun = dot(dir, toSun);
  let silver = skyCloudPhase(cosSun, 0.6) * 2.2;
  let sunFlat = normalize(toSun.xy + vec2<f32>(1e-4, 0.0));
  let grayOf = (1.0 - 0.62 * dark);
  var result = vec4<f32>(0.0);
  // cirrus: thin streaks along the drift, at 8 km, white and shining
  if (cirrusAmount > 0.001 && dir.z > 0.0) {
    let q = dir.xy / (dir.z + 0.08) * (8000.0 / (2500.0 * size)) + drift * 0.6;
    let along = vec2<f32>(0.94, 0.33);
    let streak = vec2<f32>(dot(q, along) * 0.35, dot(q, vec2<f32>(-along.y, along.x)) * 3.2);
    let n = skyCloudFbm(streak) * (0.6 + 0.4 * skyCloudNoise(streak * vec2<f32>(4.0, 0.5)));
    let share = smoothstep(0.42, 0.75, n) * cirrusAmount * 0.6 * smoothstep(0.0, 0.15, dir.z);
    let color = (ambient * 1.5 + sun * (0.25 + silver) + glow * 1.2) * grayOf;
    result = vec4<f32>(color, share);
  }
  if (cover <= 0.001) { return result; }
  // the band on the horizon: cumulus far away, seen from the side, standing
  // on a flat base; the tops rounded domes, a few to a cell of azimuth
  let elevation = dir.z;
  let band = kind * smoothstep(0.03, 0.3, cover) * (1.0 - smoothstep(0.8, 1.0, cover));
  if (band > 0.001 && elevation < 0.12) {
    let azimuth = atan2(dir.y, dir.x) / 6.2832 + 0.5 + L.clouds.z * 0.0004;
    let cells = 40.0;
    let u = azimuth * cells;
    var top = 0.0;
    for (var k = -1; k <= 1; k = k + 1) {
      let index = floor(u) + f32(k);
      let h = skyCloudHash3(vec2<f32>(index - floor(index / cells) * cells, 7.0));
      let present = smoothstep(h.z - 0.1, h.z + 0.1, band * 1.2);
      let across = (u - index - 0.2 - 0.6 * h.x) / ((0.35 + 0.5 * h.y) * mix(0.6, 1.0, band));
      let dome = (0.015 + 0.04 * h.y) * size * sqrt(max(1.0 - across * across, 0.0)) * present;
      top = max(top, dome);
    }
    // cauliflower tops: smaller domes heaped on the big ones
    let small = u * 3.3;
    var heap = 0.0;
    for (var k = -1; k <= 1; k = k + 1) {
      let index = floor(small) + f32(k);
      let h = skyCloudHash3(vec2<f32>(index - floor(index / (cells * 3.3)) * cells * 3.3, 11.0));
      let across = (small - index - 0.5) / (0.55 + 0.4 * h.x);
      heap = max(heap, (0.3 + 0.7 * h.y) * sqrt(max(1.0 - across * across, 0.0)));
    }
    top = top * (0.7 + 0.45 * heap);
    let base = 0.006;
    let share = smoothstep(base - 0.004, base + 0.002, elevation)
      * smoothstep(-0.0015, 0.0015, base + top - elevation) * step(0.003, top);
    if (share > 0.0) {
      let up = clamp((elevation - base) / max(top, 1e-4), 0.0, 1.0);
      // lit from the front (the sun behind the eye) white, against the sun
      // darker with a shining rim
      let front = 0.5 - 0.5 * dot(normalize(dir.xy + vec2<f32>(1e-4, 0.0)), sunFlat);
      let rim = 1.0 - smoothstep(0.0, 0.006, base + top - elevation);
      let color = (ambient * (0.9 + 0.55 * up) + sun * (0.08 + 0.22 * front * up + silver * rim * 0.5) + glow * (0.4 + 0.6 * up))
        * grayOf;
      // far away: the haze takes some of it
      result = skyCloudOver(vec4<f32>(mix(color, sky, 0.3), share * band), result);
    }
  }
  if (dir.z <= 0.0) { return result; }
  // the layer overhead: on a plane at the clouds' height over the world,
  // nearer the eye larger (the same field the shadow on the ground comes
  // from, skyCloudSunThrough); seen at a slant, the far side of a puff is
  // its flat base (lower on the screen), cut straight and darker
  let world = L.camPos.xy + dir.xy / (dir.z + 0.08) * max(height - L.camPos.z, 100.0);
  let viewFlat = normalize(dir.xy + vec2<f32>(1e-4, 0.0));
  let slant = 1.0 - smoothstep(0.25, 0.8, dir.z);
  let layer = skyCloudSample(skyCloudPoint(world), viewFlat, slant);
  let share = layer.share * smoothstep(0.0, 0.1, dir.z);
  if (share <= 0.001) { return result; }
  let thick = layer.thick;
  let farSide = dot(layer.away, viewFlat);
  let sheetNoise = layer.sheetNoise;
  // a puff's sunward side lighter, its middle and its base darker
  // underneath; puffs away from the sun lit on the side we see
  let sunward = 0.5 + 0.5 * dot(normalize(layer.away + vec2<f32>(1e-4, 0.0)), sunFlat) * kind;
  let front = 0.5 - 0.5 * dot(viewFlat, sunFlat);
  let baseShade = 1.0 - 0.3 * kind * slant * smoothstep(-0.1, 0.5, farSide);
  let through = (1.0 - thick) * (1.0 - thick);
  let shade = (mix(1.75, 1.45, dark) - 0.55 * thick * mix(0.3, 1.0, dark) - 0.45 * thick * kind - 0.35 * (sheetNoise - 0.5) * (1.0 - kind)) * baseShade;
  let lit = 0.05 + 0.06 * sunward + 0.1 * front * kind * (1.0 - 0.5 * thick);
  let color = (ambient * shade + sun * (lit + silver * through * 0.7) + glow * (1.0 - 0.6 * thick)) * grayOf;
  return skyCloudOver(vec4<f32>(color, share), result);
}

// The sky toward `dir`, ready to sum: the physical models (Preetham and
// Hosek-Wilkie, in cd/m2) and the moon and night sky (cd/m2) times the
// camera's exposure scale (camPos.w, LightUnits), the painted stylized sky
// and the clear color as they are (display colors); the sun's disc rides
// on the sun's light (sunColor, already exposed)
fn skyColor(dir: vec3<f32>) -> vec3<f32> {
  let kind = L.skyParams.x;
  // The sky's own sun; a sky uploaded without one falls back to the key light
  var toSun = normalize(-L.sunDir.xyz);
  if (dot(L.skySun.xyz, L.skySun.xyz) > 0.25) { toSun = normalize(L.skySun.xyz); }
  var c = L.clearColor.rgb;
  if (kind > 1.5 && kind < 2.5) { c = skyProcedural(dir, toSun, L.skyParams.y) * L.camPos.w; }
  if (kind > 2.5 && kind < 3.5) { c = skyStylized(dir, toSun); }
  if (kind > 3.5) { c = skyHosek(dir, toSun) * L.camPos.w; }
  // the sky's own light here, without the sun's disc: what lights a cloud
  let clearSky = c * L.skyParams.z;
  if (kind > 1.5) {
    let ca = dot(dir, toSun);
    let cutoff = cos(max(L.skyParams.w, 0.001));
    // a moon in front of the sun hides its disc (a solar eclipse)
    let disc = smoothstep(cutoff, mix(cutoff, 1.0, 0.35), ca) * (1.0 - moonsCover(dir));
    // what is left of the sun keeps its full surface brightness: the
    // light (sunColor) fell by the covered share, the crescent did not
    let lightLeft = max(1.0 - 0.995 * clamp(L.moons[1].w, 0.0, 1.0), 0.005);
    c = c + L.sunColor.rgb * disc * L.skyHorizon.w / lightLeft;
  }
  c = c * L.skyParams.z;
  // The moon and the night sky come after the daylight fade, so they show at
  // night: a dark blue night that brightens and glows round the moon with its
  // light, and the disc, faint by day against the bright sky
  // A moonless night keeps the night sky's own faint blue (starlight and
  // airglow); the moon brightens it and glows round itself
  let above = smoothstep(-0.01, 0.03, dir.z);
  if (kind > 1.5) {
    let night = 1.0 - clamp(L.skyParams.z, 0.0, 1.0);
    let height = clamp(dir.z, 0.0, 1.0);
    // cd/m2 (the look tuned in the old units at the reference exposure)
    let nightSky = mix(vec3<f32>(400.0, 560.0, 1000.0), vec3<f32>(120.0, 200.0, 520.0), height)
      * (0.35 + 2.4 * L.night.x) * L.camPos.w;
    c = c + nightSky * night * above;
  }
  if (L.moon.w > 0.0 && kind > 1.5) {
    let toMoon = normalize(L.moon.xyz);
    let night = 1.0 - clamp(L.skyParams.z, 0.0, 1.0);
    let moonlight = L.night.x;
    let glow = pow(max(dot(dir, toMoon), 0.0), 48.0) * 2400.0 * moonlight * L.camPos.w;
    c = c + vec3<f32>(0.55, 0.62, 0.80) * glow * night * above;
    let disc = moonDisc(dir, toMoon, toSun, max(L.skySun.w, 0.001));
    // In an eclipse the disc's near side faces away from the sun: dark
    // against the sky, and round it the corona, brighter as the sun goes
    let eclipse = clamp(L.moons[1].w, 0.0, 1.0);
    // moon.w: the disc's luminance in cd/m2
    c = c + vec3<f32>(0.92, 0.94, 1.0) * disc * L.moon.w * L.camPos.w * above * (1.0 - eclipse)
      * earthShadow(dir);
    c = c * (1.0 - discCover(dir, toMoon, max(L.skySun.w, 0.001)) * eclipse * 0.85);
    if (eclipse > 0.97) {
      let fromSun = acos(clamp(dot(dir, toSun), -1.0, 1.0));
      let rim = max(fromSun - L.skySun.w, 0.0) / max(L.skySun.w, 0.0005);
      let corona = smoothstep(0.97, 1.0, eclipse) * exp(-rim * 2.5) * step(L.skySun.w, fromSun);
      c = c + vec3<f32>(0.95, 0.97, 1.0) * corona * L.moon.w * 1.2 * L.camPos.w * above;
    }
    // the extra moons, each in its own size and color, lit by the same sun
    for (var i = 0; i < 3; i = i + 1) {
      let other = L.moons[i * 2];
      if (other.w > 0.0) {
        let otherDisc = moonDisc(dir, normalize(other.xyz), toSun, other.w);
        c = c + L.moons[i * 2 + 1].rgb * otherDisc * L.camPos.w * above;
      }
    }
  }
  // the weather's clouds over all of it: the sun, the moon and the stars
  // go behind them
  if (kind > 1.5) {
    let cloud = skyClouds(dir, clearSky, toSun);
    c = mix(c, cloud.rgb, cloud.w);
  }
  return c;
}
