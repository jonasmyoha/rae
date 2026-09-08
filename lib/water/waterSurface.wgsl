// waterSurface.wgsl — the stylised (toon) water surface (#830), the Roystan
// recipe over a Gerstner grid. Composed AFTER lib/noise.wgsl (raeNoiseValue2).
//
// Vertex: a unit grid scaled to the body's extent, displaced by 1-2 Gerstner
// waves that also give the normal; TAA jitter applied last like the G-buffer.
// Fragment (all toon, no refraction on this tier):
//   * water depth under the pixel = linear(sceneDepth) - linear(surfaceDepth),
//     from the G-buffer depth bound as a texture (the same texture is the
//     pass's read-only depth attachment);
//   * depth-gradient colour shallow -> deep, alpha rising with depth;
//   * shoreline foam + toon ripples: value noise panned in world space, warped
//     by a second octave, POSTERISED by a cutoff that goes to zero at the shore
//     (so the bank is all foam) and to `rippleCutoff` in deep water (sparse
//     ripple flecks);
//   * sky tint through a Fresnel term: the stylised sky's own hemisphere
//     (horizon -> zenith by reflect(v, n).z — what Sky.radiance does on the CPU);
//   * a STEPPED sun highlight (Blinn-Phong thresholded, not smooth).
struct Frame {
  viewProj: mat4x4<f32>,
  prevViewProj: mat4x4<f32>,
  jitter: vec4<f32>,
};
struct Water {
  centreExtent: vec4<f32>,  // xyz = still-surface centre, w = extent
  shallow: vec4<f32>,       // rgb = shallow colour, w = depthMax
  deep: vec4<f32>,          // rgb = deep colour, w = absorb
  foam: vec4<f32>,          // x = foamDistance, y = rippleScale, z = rippleSpeed, w = rippleCutoff
  camera: vec4<f32>,        // xyz = camera position, w = time
  sun: vec4<f32>,           // xyz = sun direction (FROM the sun), w = highlightStep
  sunColor: vec4<f32>,      // rgb, w = highlightPower
  zenith: vec4<f32>,        // rgb, w = nearZ
  horizon: vec4<f32>,       // rgb, w = farZ
  wave0: vec4<f32>,         // dir.xy (unit), amplitude, wavelength
  wave0b: vec4<f32>,        // steepness, speed
  wave1: vec4<f32>,
  wave1b: vec4<f32>,
  tuning: vec4<f32>,        // x = distortion, y = waveCount (0..2), z = river mode, w = flow speed (m/s)
  refract: vec4<f32>,       // x = refraction on (1) / off (0), y = strength, zw = lit target size
  absorb: vec4<f32>,        // rgb = Beer-Lambert absorption per metre
  cascades: vec4<f32>,      // xyz = cascade tile sizes (m), w = realistic path (1) / toon (0)
  centre: vec4<f32>,        // xy = camera position snapped to metres (mesh mode 2), z = cascade count, w = grid cell (m, lakes)
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<uniform> W: Water;
@group(0) @binding(2) var depthTex: texture_depth_2d;
@group(0) @binding(3) var litCopyTex: texture_2d<f32>;   // the opaque frame (#842 snapshot)
@group(0) @binding(4) var litSampler: sampler;
// The FFT cascades (#851): displacement (xyz, w = Jacobian) and normal per cascade.
@group(0) @binding(5) var cascadeDisp0: texture_2d<f32>;
@group(0) @binding(6) var cascadeNorm0: texture_2d<f32>;
@group(0) @binding(7) var cascadeDisp1: texture_2d<f32>;
@group(0) @binding(8) var cascadeNorm1: texture_2d<f32>;
@group(0) @binding(9) var cascadeDisp2: texture_2d<f32>;
@group(0) @binding(10) var cascadeNorm2: texture_2d<f32>;
@group(0) @binding(11) var cascadeSampler: sampler;   // repeat: the cascades tile

// Sum the cascades' displacement at world xy (each tiles its own size).
fn cascadeDisplacement(xy: vec2<f32>) -> vec3<f32> {
  var d = textureSampleLevel(cascadeDisp0, cascadeSampler, xy / W.cascades.x, 0.0).xyz;
  if (W.centre.z > 1.5) { d += textureSampleLevel(cascadeDisp1, cascadeSampler, xy / W.cascades.y, 0.0).xyz; }
  if (W.centre.z > 2.5) { d += textureSampleLevel(cascadeDisp2, cascadeSampler, xy / W.cascades.z, 0.0).xyz; }
  return d;
}
// Slopes add across cascades (a normal is -slope, 1). Whitecaps come from the
// Jacobian of the horizontal displacement: J = 1 is flat, J -> 0 is the
// surface folding over itself, so foam starts near J = 0.5 and is full at 0.
fn foamFromJacobian(j: f32) -> f32 {
  return clamp((0.5 - j) * 2.0, 0.0, 1.0);
}
fn cascadeSlopeFoam(xy: vec2<f32>) -> vec3<f32> {
  var slope = vec2<f32>(0.0);
  var foam = 0.0;
  let n0 = textureSample(cascadeNorm0, cascadeSampler, xy / W.cascades.x).xyz;
  slope += -n0.xy / max(n0.z, 0.05);
  foam += foamFromJacobian(textureSample(cascadeDisp0, cascadeSampler, xy / W.cascades.x).w);
  if (W.centre.z > 1.5) {
    let n1 = textureSample(cascadeNorm1, cascadeSampler, xy / W.cascades.y).xyz;
    slope += -n1.xy / max(n1.z, 0.05);
    foam += foamFromJacobian(textureSample(cascadeDisp1, cascadeSampler, xy / W.cascades.y).w);
  }
  if (W.centre.z > 2.5) {
    let n2 = textureSample(cascadeNorm2, cascadeSampler, xy / W.cascades.z).xyz;
    slope += -n2.xy / max(n2.z, 0.05);
    foam += foamFromJacobian(textureSample(cascadeDisp2, cascadeSampler, xy / W.cascades.z).w);
  }
  return vec3<f32>(slope, foam);
}

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) world: vec3<f32>,
  @location(1) nrm: vec3<f32>,
  // Noise lookup coordinates panned in the VERTEX shader (the Roystan mobile
  // trick, #847): the world->noise scaling and the time pan are per-vertex
  // work, the fragment only samples. Built from the UNDISPLACED grid position
  // so the pattern does not swim with the Gerstner motion.
  @location(2) rippleUv: vec2<f32>,
  @location(3) distortUvA: vec2<f32>,
  @location(4) distortUvB: vec2<f32>,
  @location(5) bakedFoam: f32,   // river: curvature foam baked into the mesh (#833)
  @location(6) gridXy: vec2<f32>, // undisplaced world xy (cascade lookups in the fragment)
};

const TAU: f32 = 6.28318530718;

// One Gerstner wave: accumulates displacement + the normal's partial sums.
fn gerstner(w: vec4<f32>, wb: vec4<f32>, xy: vec2<f32>, t: f32,
            disp: ptr<function, vec3<f32>>, dNormal: ptr<function, vec3<f32>>) {
  let d = w.xy;
  let amplitude = w.z;
  let k = TAU / max(w.w, 0.01);
  let steepness = wb.x;
  let f = k * (dot(d, xy) - wb.y * t);
  let c = cos(f);
  let s = sin(f);
  (*disp) += vec3<f32>(steepness * amplitude * d.x * c, steepness * amplitude * d.y * c, amplitude * s);
  // GPU Gems 1 ch.1: n = (-sum d.x k A cos, -sum d.y k A cos, 1 - sum Q k A sin)
  (*dNormal) += vec3<f32>(d.x * k * amplitude * c, d.y * k * amplitude * c, steepness * k * amplitude * s);
}

fn waveGridFactor(wavelength: f32) -> f32 {
  let cell = W.centre.w;
  if (cell <= 0.0) { return 1.0; }
  return smoothstep(2.0, 4.0, wavelength / cell);
}

@vertex
fn vs(@location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {
  var o: VsOut;
  let extent = W.centreExtent.w;
  let mode = W.tuning.z;
  let river = mode > 0.5 && mode < 1.5;
  let centred = mode > 1.5;
  let realistic = W.cascades.w > 0.5;
  // Mesh modes: a lake is the unit grid scaled to the body; a river is its own
  // baked world-space ribbon (#833); an ocean is a camera-centred metres mesh
  // (#851) placed at the snapped camera position every frame.
  var base = vec3<f32>(W.centreExtent.x + p.x * extent, W.centreExtent.y + p.y * extent, W.centreExtent.z);
  if (river) { base = p; }
  if (centred) { base = vec3<f32>(W.centre.x + p.x, W.centre.y + p.y, W.centreExtent.z); }
  let t = W.camera.w;
  var disp = vec3<f32>(0.0, 0.0, 0.0);
  var dn = vec3<f32>(0.0, 0.0, 0.0);
  // A wave the grid cannot sample (under ~2 cells) aliases into wide bands,
  // so its amplitude fades out (Water.waveGridFactor is the CPU mirror, #857).
  var wave0 = W.wave0;
  var wave1 = W.wave1;
  wave0.z *= waveGridFactor(wave0.w);
  wave1.z *= waveGridFactor(wave1.w);
  if (!river && !realistic && W.tuning.y > 0.5) { gerstner(wave0, W.wave0b, base.xy, t, &disp, &dn); }
  if (!river && !realistic && W.tuning.y > 1.5) { gerstner(wave1, W.wave1b, base.xy, t, &disp, &dn); }
  // Realistic: the FFT cascades displace the vertex (world-space lookup, so
  // the waves stay put while the centred mesh moves under them).
  if (realistic) { disp = cascadeDisplacement(base.xy); }
  let world = base + disp;
  o.world = world;
  o.gridXy = base.xy;
  // Noise coordinates: a lake pans in world space; a river ADVECTS along its
  // own u (arc length) at the flow speed — the spline parameter is the flow
  // map, so there is no texture and no phase reset to hide.
  var noiseXy = base.xy;
  var pan = vec2<f32>(t * W.foam.z, t * W.foam.z * 0.7);
  if (river) {
    noiseXy = vec2<f32>(uv.x - t * W.tuning.w, uv.y);
    pan = vec2<f32>(0.0, 0.0);
  }
  o.rippleUv = noiseXy * W.foam.y + pan;
  o.distortUvA = noiseXy * 0.08 + vec2<f32>(t * 0.05, -t * 0.03);
  o.distortUvB = noiseXy * 0.08 + vec2<f32>(-t * 0.04, t * 0.06);
  o.bakedFoam = select(0.0, n.z, river);
  o.nrm = normalize(vec3<f32>(-dn.x, -dn.y, 1.0 - dn.z));
  var clip = F.viewProj * vec4<f32>(world, 1.0);
  // Jitter LAST, matching the G-buffer pass (#397), so the surface sits in the
  // same sub-pixel frame as the depth it tests and TAA does not smear it.
  clip = vec4<f32>(clip.xy + F.jitter.xy * clip.w, clip.zw);
  o.pos = clip;
  return o;
}

// Reverse-Z perspective (Math3d.mat4PerspectiveReverseZ): ndc 1 at near, 0 at far.
fn linearDepth(ndc: f32) -> f32 {
  let near = W.zenith.w;
  let far = W.horizon.w;
  return near * far / (ndc * (far - near) + near);
}

@fragment
fn fs(i: VsOut) -> @location(0) vec4<f32> {
  let px = vec2<i32>(i32(i.pos.x), i32(i.pos.y));
  let sceneNdc = textureLoad(depthTex, px, 0);
  let sceneLinear = linearDepth(sceneNdc);
  let surfaceLinear = linearDepth(i.pos.z);
  let waterDepth = max(sceneLinear - surfaceLinear, 0.0);

  // Depth-gradient body colour.
  let depthT = clamp(waterDepth / max(W.shallow.w, 0.01), 0.0, 1.0);
  var colour = mix(W.shallow.rgb, W.deep.rgb, depthT);
  var alpha = mix(0.7, 0.97, depthT) * W.deep.w;

  // Toon ripples + shoreline foam: world-space value noise, panned, warped by a
  // second octave, then posterised by a depth-driven cutoff.
  // Distortion (world units) warps the vertex-panned ripple coordinate; the
  // ripple scale (W.foam.y) converts it, so this equals the old per-fragment
  // ((uvW + distortion) * scale + pan) exactly, minus the per-pixel maths.
  let distortion = vec2<f32>(
    raeNoiseValue2(i.distortUvA, 11u) - 0.5,
    raeNoiseValue2(i.distortUvB, 23u) - 0.5) * W.tuning.x;
  // Toon ripples are LINES, not blobs: a ridged two-octave noise (1 - |2n-1|)
  // peaks along thin curves, and only its highest values pass the cutoff, so
  // the surface carries sparse cel crests rather than white patches.
  let rippleXy = i.rippleUv + distortion * W.foam.y;
  let ridged = 1.0 - abs(2.0 * raeNoiseValue2(rippleXy, 7u) - 1.0);
  let ridged2 = 1.0 - abs(2.0 * raeNoiseValue2(rippleXy * 2.3 + vec2<f32>(3.1, 7.7), 19u) - 1.0);
  let ripple = ridged * 0.65 + ridged2 * 0.35;
  let foamT = clamp(waterDepth / max(W.foam.x, 0.01), 0.0, 1.0);
  // Shoreline: a solid foam band where the water is shallower than a third of
  // foamDistance, broken up by the noise as it thins out; deep water keeps
  // only the crest lines above rippleCutoff (0.86..0.92 reads well).
  let shorelineBand = 1.0 - smoothstep(0.1, 0.5, foamT);
  let shoreline = max(shorelineBand * smoothstep(0.2, 0.35, ripple), 1.0 - smoothstep(0.05, 0.18, foamT));
  // Crests thin out with distance: the cutoff rises and the lines fade, so the
  // far water is a calm colour field instead of a moire strip at the horizon.
  let viewDistance = length(W.camera.xyz - i.world);
  let crestFade = 1.0 - smoothstep(20.0, 140.0, viewDistance);
  let crestCutoff = W.foam.w + (1.0 - crestFade) * 0.1;
  let crests = smoothstep(crestCutoff - 0.015, crestCutoff + 0.015, ripple) * crestFade;
  var foam = max(shoreline, crests);
  // River rapids: the baked curvature foam, streaked by the same noise.
  foam = max(foam, i.bakedFoam * smoothstep(0.45, 0.8, ripple));
  // Realistic (#851): no toon ripples — whitecaps where a cascade folds
  // (Jacobian < 1 -> foam), streaked by the noise; the shoreline depth foam
  // (cutoff ~0 in the shallows) stays.
  let realisticPath = W.cascades.w > 0.5;
  var cascade = vec3<f32>(0.0);
  if (realisticPath) {
    cascade = cascadeSlopeFoam(i.gridXy);
    let shoreline = smoothstep(-0.03, 0.03, 0.15 - foamT);
    let whitecap = clamp(cascade.z, 0.0, 1.0) * smoothstep(0.25, 0.65, ripple);
    foam = max(shoreline, whitecap);
  }
  colour = mix(colour, vec3<f32>(0.96, 0.98, 1.0), foam * 0.85);
  alpha = max(alpha, foam * 0.9);

  // Reflection: the stylised sky hemisphere along the reflected view ray,
  // weighted by Schlick Fresnel (F0 = 0.02, water's normal-incidence reflectance).
  var n = normalize(i.nrm);
  if (realisticPath) { n = normalize(vec3<f32>(-cascade.xy, 1.0)); }
  // Toon: the Gerstner normal flattens with distance, or the two waves tile a
  // checkerboard through the Fresnel rim and the glint along the horizon.
  if (!realisticPath) { n = normalize(mix(n, vec3<f32>(0.0, 0.0, 1.0), 1.0 - crestFade)); }
  let toCamera = normalize(W.camera.xyz - i.world);
  let r = reflect(-toCamera, n);
  let skyTint = mix(W.horizon.rgb, W.zenith.rgb, clamp(r.z, 0.0, 1.0));
  let ndv = clamp(dot(n, toCamera), 0.0, 1.0);
  let fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);

  // Refraction (#832): read the opaque scene through the surface, offset by the
  // normal. DEPTH MASK: if the pixel the offset lands on is ABOVE the water
  // (closer than the surface), it is an object out of the water and must not
  // bleed into it — fall back to the unrefracted pixel (the Boat Attack/Crest
  // trick). Then Beer-Lambert: light travelling `refrDepth` metres of water is
  // absorbed per channel, the lost part replaced by scattered deep colour.
  let litSize = W.refract.zw;
  let screenUv = i.pos.xy / litSize;
  var refrUv = screenUv + n.xy * W.refract.y;
  refrUv = clamp(refrUv, vec2<f32>(0.001), vec2<f32>(0.999));
  let depthDims = vec2<f32>(textureDimensions(depthTex));
  let refrPx = clamp(vec2<i32>(refrUv * depthDims), vec2<i32>(0), vec2<i32>(depthDims) - vec2<i32>(1));
  var refrSceneLinear = linearDepth(textureLoad(depthTex, refrPx, 0));
  if (refrSceneLinear < surfaceLinear) {
    refrUv = screenUv;
    refrSceneLinear = sceneLinear;
  }
  let refrDepth = max(refrSceneLinear - surfaceLinear, 0.0);
  let sceneColour = textureSample(litCopyTex, litSampler, refrUv).rgb;
  let absorption = exp(-refrDepth * W.absorb.rgb);
  let underwater = sceneColour * absorption + W.deep.rgb * (1.0 - absorption) * 0.7;
  if (W.refract.x > 0.5) {
    // Refracting surface: it composes its own background, so it is opaque.
    let foamMix = foam * 0.85;
    colour = mix(mix(underwater, skyTint, fresnel), vec3<f32>(0.96, 0.98, 1.0), foamMix);
    alpha = 1.0;
  } else if (realisticPath) {
    colour = mix(colour, skyTint, fresnel);
  } else {
    // Toon: the sky is a RIM at grazing angles, never a wash over the body colour.
    colour = mix(colour, skyTint, fresnel * 0.35);
  }

  // Shading. Toon: a two-band diffuse and a STEPPED Blinn-Phong sun highlight.
  // Realistic: smooth diffuse and a sharp smooth Blinn-Phong sun glint.
  let toSun = normalize(-W.sun.xyz);
  let ndl = clamp(dot(n, toSun), 0.0, 1.0);
  let h = normalize(toSun + toCamera);
  let spec = pow(clamp(dot(n, h), 0.0, 1.0), W.sunColor.w);
  var highlight = smoothstep(W.sun.w - 0.03, W.sun.w + 0.03, spec);
  if (realisticPath) {
    colour *= 0.85 + 0.15 * ndl;
    highlight = spec;
  } else {
    colour *= 0.78 + 0.22 * smoothstep(0.25, 0.32, ndl);
    // The stepped glint on the Gerstner normals tiles into a checkerboard at
    // the horizon; let it fade with the crests.
    highlight *= crestFade;
  }
  colour += W.sunColor.rgb * highlight * 0.7;
  alpha = max(alpha, highlight * 0.8);

  return vec4<f32>(colour, alpha);
}
