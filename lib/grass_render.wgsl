struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,    // xy the TAA jitter (z: the trees' wind, unused here)
  wind: vec4<f32>,
  previousWind: vec4<f32>,
  // x: the snow depth (m; lib/snow.wgsl, Gbuffer setGbufferSnow)
  scene: vec4<f32>,
};
struct DrawU {
  model: mat4x4<f32>,
  prevModel: mat4x4<f32>,
  albedoMetallic: vec4<f32>,
  params: vec4<f32>,
  fade: vec4<f32>,     // the level cross-fade (GbufferRecord [40..43]); 0 when not fading
};
struct GrassU {
  a: vec4<f32>,
  b: vec4<f32>,
  c: vec4<f32>,
  d: vec4<f32>,
  e: vec4<f32>,
  f: vec4<f32>,
  g: vec4<f32>,
  h: vec4<f32>,
  i: vec4<f32>,
  // the ground palette's dry grass and how dry (app3d/GroundPalette)
  j: vec4<f32>,
  // the air's drift (xy) and the unscaled wind (zw): the gusts' (lib/wind.wgsl)
  k: vec4<f32>,
  // a ring's inner edge (x, m) and the band it fades in over (y, m); 0: none;
  // z the wind shimmer's strength (GrassViewSettings shimmer)
  l: vec4<f32>,
  // what bends the blades near it (GrassCompute grassSetBenders): x, y, the
  // radius (m) and the strength (0 to 1); a radius of 0 is none
  bend: array<vec4<f32>, 8>,
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<storage, read> draws: array<DrawU>;
@group(0) @binding(2) var<uniform> GW: GrassU;
@group(0) @binding(3) var tuftImage: texture_2d_array<f32>;
@group(0) @binding(4) var tuftSampler: sampler;
fn octWrap(v: vec2<f32>) -> vec2<f32> {
  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));
  return (vec2<f32>(1.0) - abs(v.yx)) * s;
}
fn octEncode(n: vec3<f32>) -> vec2<f32> {
  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
  if (n.z < 0.0) { p = octWrap(p); }
  return p * 0.5 + vec2<f32>(0.5);
}
fn octDecode(e: vec2<f32>) -> vec3<f32> {
  let f = e * 2.0 - vec2<f32>(1.0);
  var n = vec3<f32>(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
  let t = max(-n.z, 0.0);
  n = vec3<f32>(n.x + select(t, -t, n.x >= 0.0),
                n.y + select(t, -t, n.y >= 0.0), n.z);
  return normalize(n);
}
struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) nrm: vec3<f32>,
  @location(1) @interpolate(flat) inst: u32,
  @location(2) clipNow: vec4<f32>,
  @location(3) clipPrev: vec4<f32>,
  @location(4) height: f32,
  @location(5) uv: vec2<f32>,
};
const bladeSegments: u32 = 4u;
fn bladeLocal(vi: u32, curve: f32) -> vec3<f32> {
  let segment = vi / 6u;
  let segmentVertex = vi % 6u;
  var corner = 0u;
  if (segmentVertex == 0u) { corner = 0u; }
  else if (segmentVertex == 1u) { corner = 1u; }
  else if (segmentVertex == 2u) { corner = 3u; }
  else if (segmentVertex == 3u) { corner = 0u; }
  else if (segmentVertex == 4u) { corner = 3u; }
  else { corner = 2u; }
  let top = (corner == 2u) || (corner == 3u);
  let right = (corner == 1u) || (corner == 3u);
  let z = (f32(segment) + select(0.0, 1.0, top)) / f32(bladeSegments);
  let halfWidth = 0.025 + 0.475 * pow(max(1.0 - z, 0.0), 0.72);
  let centerX = curve * z * z;
  let x = centerX + select(-halfWidth, halfWidth, right);
  // A shallow bow makes highlights flow across a tuft instead of lighting the
  // entire old flat sheet as one polygon.
  let bow = (0.055 + abs(curve) * 0.045) * sin(z * 3.14159265);
  return vec3<f32>(x, bow, z);
}
fn bladeNormal(z: f32, curve: f32) -> vec3<f32> {
  let bowSlope = (0.055 + abs(curve) * 0.045) * 3.14159265 * cos(z * 3.14159265);
  let surfaceNormal = normalize(vec3<f32>(0.0, 1.0, -bowSlope));
  // Foliage normals lean toward the sky so both sides retain broad, soft
  // daylight while the curved surface still carries a readable highlight.
  return normalize(mix(surfaceNormal, vec3<f32>(0.0, 0.0, 1.0), 0.58));
}
// The alternatives share one bounded instance record and identical placement.
fn quadUv(vertex: u32) -> vec2<f32> {
  let corners = array<vec2<f32>, 6>(vec2<f32>(0,0), vec2<f32>(1,0), vec2<f32>(1,1),
    vec2<f32>(0,0), vec2<f32>(1,1), vec2<f32>(0,1));
  return corners[vertex % 6u];
}
fn tuftLocal(vertex: u32, curve: f32) -> vec3<f32> {
  let uv = quadUv(vertex);
  let angle = f32(vertex / 6u) * 2.0943951;
  let across = (uv.x - 0.5) * 2.8;
  let lean = curve * uv.y * uv.y * 0.45;
  return vec3<f32>(cos(angle) * across + lean, sin(angle) * across, uv.y);
}
fn ribbonTuft(vertex: u32, curve: f32) -> vec3<f32> {
  let blade = vertex / 24u;
  let angle = f32(blade) * 2.399963;
  let lean = curve * 0.35 + sin(f32(blade) * 1.73) * 0.8;
  let local = bladeLocal(vertex % 24u, lean);
  let height = 0.60 + f32((blade * 7u) % 12u) * (0.40 / 11.0);
  let reach = local.z * local.z * (0.85 + f32(blade % 3u) * 0.12);
  let across = local.x * 0.22 + reach;
  return vec3<f32>(cos(angle) * across, sin(angle) * across, local.z * height);
}
// A blade's point `p` (world, its root at `root`) bent at `time`: downwind by
// the wind's gust bands, the same bands that lighten the painted grass
// (grass_field.wgsl grassPaintShimmer, so the bend and the light roll across
// the field together), and away from anything in GW.bend's circles, pushed
// over and lower the nearer its middle. The bend grows with the point's
// height above the root, so the root stays put. w: how much it is pushed
// aside (0 to 1), for the camera tilt to leave it alone.
const grassGustBend: f32 = 0.6;
const grassPushBend: f32 = 1.3;
fn grassBend(p: vec3<f32>, root: vec3<f32>, time: f32) -> vec4<f32> {
  let height = max(p.z - root.z, 0.0);
  var out = p;
  let windStrength = length(GW.k.zw);
  if (windStrength > 0.05 && GW.l.z > 0.0) {
    let band = grassPaintShimmer(root.xy, GW.k.zw, time) / grassShimmerLight * GW.l.z;
    out = vec3<f32>(out.xy + (GW.k.zw / windStrength) * band * grassGustBend * height, out.z);
  }
  var push = 0.0;
  var away = vec2<f32>(0.0);
  for (var index = 0u; index < 8u; index = index + 1u) {
    let circle = GW.bend[index];
    if (circle.z <= 0.0) { continue; }
    let fromMiddle = root.xy - circle.xy;
    let distance = length(fromMiddle);
    if (distance >= circle.z) { continue; }
    let amount = (1.0 - distance / circle.z) * (1.0 - distance / circle.z) * circle.w;
    if (amount > push) {
      push = amount;
      away = fromMiddle / max(distance, 0.05);
    }
  }
  if (push > 0.0) {
    out = vec3<f32>(out.xy + away * push * grassPushBend * height, root.z + (out.z - root.z) * (1.0 - 0.7 * push));
  }
  return vec4<f32>(out, push);
}
// A blade's point `p` (world) tilted toward a camera at `camera` looking down
// at its root `root`: the steeper the view, the further the blade leans away
// from the camera about its root (up to 70 degrees at `tilt` 1, straight
// down). With `turn` (a flat blade), its face also turns to the camera: its
// own across axis `bladeAcross` turns to lie across the view. Its length is
// kept, so it lies down rather than shrinks.
fn grassTiltToCamera(p: vec3<f32>, root: vec3<f32>, bladeAcross: vec2<f32>, camera: vec3<f32>,
    tilt: f32, turn: bool) -> vec3<f32> {
  let toCamera = camera - root;
  let ground = length(toCamera.xy);
  let steep = clamp(toCamera.z / max(length(toCamera), 0.001), 0.0, 1.0);
  let amount = tilt * steep * steep;
  if (amount <= 0.0 || ground < 0.0001) { return p; }
  let away = -toCamera.xy / ground;
  let viewAcross = vec2<f32>(-away.y, away.x);
  var rel = p - root;
  if (turn) {
    let ownAcross = normalize(bladeAcross + vec2<f32>(0.00001, 0.0));
    let signedAcross = dot(rel.xy, ownAcross);
    let rest = rel.xy - ownAcross * signedAcross;
    let facing = select(-1.0, 1.0, dot(ownAcross, viewAcross) >= 0.0);
    rel = vec3<f32>(mix(rel.xy, viewAcross * facing * signedAcross + rest, amount), rel.z);
  }
  let angle = 1.2217305 * amount;
  let along = dot(rel.xy, away);
  let sideways = rel.xy - away * along;
  let newAlong = along * cos(angle) + rel.z * sin(angle);
  let newHeight = rel.z * cos(angle) - along * sin(angle);
  return root + vec3<f32>(sideways + away * newAlong, newHeight);
}
@vertex
fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {
  let d = draws[ii];
  var lp = bladeLocal(vi, d.params.w);
  var uv = vec2<f32>(0.5, 0.5);
  if (GW.i.w < 0.5) {
    let corner = quadUv(vi);
    lp = vec3<f32>((corner.x - 0.5) * mix(1.0, 0.08, corner.y), 0.0, corner.y);
  } else if (GW.i.w > 3.5) {
    // The painted blade: one opaque triangle, its tip leaning by the blade's
    // curve (wind bends it further below)
    let corner = vi % 3u;
    lp = vec3<f32>(select(select(-0.5, 0.5, corner == 1u), d.params.w * 0.6, corner == 2u),
      0.0, select(0.0, 1.0, corner == 2u));
  } else if (GW.i.w > 2.5) {
    lp = tuftLocal(vi, d.params.w);
    let corner = quadUv(vi);
    uv = vec2<f32>(corner.x, 1.0 - corner.y);
  } else if (GW.i.w > 1.5) {
    lp = ribbonTuft(vi, d.params.w);
  }
  var o: VsOut;
  o.height = lp.z;
  o.uv = uv;
  // the blade pushed down under the snow (lib/snow.wgsl): its color keeps
  // its own root-to-tip gradient, its height is what still shows
  lp.z = snowSunkGrass(lp.z, F.scene.x);
  let base = vec2<f32>(d.model[3].x, d.model[3].y);
  // Grass reads the same base WindVector as foliage. Broad fronts travel in
  // that direction, while a small per-blade phase moves only the tip.
  let wind = GW.f.xyz;
  let timeNow = GW.f.w;
  let timePrev = GW.f.w - GW.b.w;
  let wNow = d.model * vec4<f32>(lp, 1.0);
  // the gust at the blade, from the compute pass (grass_compute.wgsl)
  let gust = d.fade.x;
  let offsetNow = grassWindOffset(base, lp.z, timeNow, wind, gust);
  var pNow = vec3<f32>(wNow.xy + offsetNow, wNow.z);
  // the wind's gust bands and anything walking or driving through bend it
  let rootNow = d.model[3].xyz;
  let bentNow = grassBend(pNow, rootNow, timeNow);
  pNow = bentNow.xyz;
  o.pos = F.viewProj * vec4<f32>(pNow, 1.0);
  var localNormal = bladeNormal(lp.z, d.params.w);
  if (GW.i.w < 0.5 || GW.i.w > 1.5) { localNormal = vec3<f32>(0.0, 0.0, 1.0); }
  // The low-poly blade's root faces up, as the ground does, so it is lit as
  // the terrain under it is and the blade grows out of the ground's own
  // shade; its own curved normal takes over toward the tip
  if (GW.i.w > 0.5 && GW.i.w < 1.5) {
    localNormal = normalize(mix(vec3<f32>(0.0, 0.0, 1.0), localNormal, smoothstep(0.0, 0.55, lp.z)));
  }
  // Seen from above, a blade leans away from the camera about its root, and a
  // painted blade turns its face to it, so the blades cover the ground instead
  // of showing their edges (GrassViewSettings cameraTilt, GW.a.w)
  if (GW.a.w > 0.0) {
    let root = d.model[3].xyz;
    // a blade being pushed aside does not turn to the camera
    pNow = grassTiltToCamera(pNow, root, d.model[0].xy, GW.c.xyz, GW.a.w * (1.0 - bentNow.w), GW.i.w > 3.5);
    o.pos = F.viewProj * vec4<f32>(pNow, 1.0);
  }
  let restNormal = normalize((d.model * vec4<f32>(localNormal, 0.0)).xyz);
  o.nrm = normalize(restNormal + vec3<f32>(-offsetNow * lp.z * 0.9, 0.0));
  o.inst = ii;
  o.clipNow = o.pos;
  let wPrev = d.prevModel * vec4<f32>(lp, 1.0);
  let offsetPrev = grassWindOffset(base, lp.z, timePrev, wind, gust);
  var pPrev = vec3<f32>(wPrev.xy + offsetPrev, wPrev.z);
  let bentPrev = grassBend(pPrev, d.prevModel[3].xyz, timePrev);
  pPrev = bentPrev.xyz;
  if (GW.a.w > 0.0) {
    pPrev = grassTiltToCamera(pPrev, d.prevModel[3].xyz, d.prevModel[0].xy, GW.c.xyz, GW.a.w * (1.0 - bentPrev.w), GW.i.w > 3.5);
  }
  o.clipPrev = F.prevViewProj * vec4<f32>(pPrev, 1.0);
  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);
  return o;
}
struct FsOut {
  @location(0) gba: vec4<f32>,
  @location(1) gbb: vec4<f32>,
  @location(2) gbc: vec4<f32>,
};
// The surface at this fragment, in the G-buffer encoding: the G-buffer
// entry below writes it, the forward main pass's fsForward
// (forward_entry.wgsl) decodes and shades it.
fn gbufferSurface(in: VsOut) -> FsOut {
  let d = draws[in.inst];
  let n = normalize(in.nrm);
  let oct = octEncode(n);
  let rough = clamp(d.params.x, 0.045, 1.0);
  let now = in.clipNow.xy / in.clipNow.w;
  let prev = in.clipPrev.xy / in.clipPrev.w;
  let motion = (now - prev) * vec2<f32>(0.5, -0.5);
  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(0.50196078);
  var o: FsOut;
  let botCol = d.albedoMetallic.rgb;
  let topCol = botCol * GW.g.w;
  let height = clamp(in.height, 0.0, 1.0);
  let grounded = mix(0.58, 1.0, smoothstep(0.0, 0.32, height));
  var albedo = mix(botCol, topCol, height) * grounded;
  // Sample outside the uniform branch so derivatives remain valid in both paths.
  let tuft = textureSample(tuftImage, tuftSampler, in.uv, 0);
  if (GW.i.w > 2.5 && GW.i.w < 3.5) {
    if (tuft.a < 0.5) { discard; }
    let tone = clamp(dot(tuft.rgb, vec3<f32>(0.2126, 0.7152, 0.0722)) / 0.16, 0.62, 1.18);
    albedo = mix(botCol, topCol, height) * mix(0.94, tone, smoothstep(0.0, 0.32, height));
  }
  // The flat blade: the root exactly the ground's color (grass_compute.wgsl),
  // no darkening, so it blends into the terrain
  if (GW.i.w < 0.5) { albedo = mix(botCol, topCol, height); }
  // The low-poly blade: the root exactly the ground's color where it stands
  // (grass_compute.wgsl grassRootColor), staying close to it up most of the
  // blade, then brightening to its own green tip (fade.yzw: the meadow's
  // green at the blade's own brightness and warmth, times tipLighten), so
  // the grass grows out of the ground in a gradient, not a flat bright
  // carpet over a darker ground
  if (GW.i.w > 0.5 && GW.i.w < 1.5) {
    let tipCol = d.fade.yzw * GW.g.w;
    albedo = mix(botCol, tipCol, pow(height, 1.8));
  }
  // The painted blade: its root's and its tip's colors from the compute pass
  // (grass_compute.wgsl, grass_field.wgsl's palette), blended up the blade (the
  // palette is two straight pieces, so this runs close to its own path)
  if (GW.i.w > 3.5) { albedo = mix(botCol, d.fade.yzw, grassPaintBand(height)); }
  // z = 1: TAA-reactive (render-perf-10): thin, wind-blown blades ghost on history
  o.gba = vec4<f32>(oct.x, oct.y, 1.0, d.params.z);
  o.gbb = vec4<f32>(albedo, rough);
  o.gbc = vec4<f32>(mEnc.x, mEnc.y, clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);
  return o;
}
@fragment
fn fs(in: VsOut) -> FsOut {
  return gbufferSurface(in);
}
