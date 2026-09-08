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
};
@group(0) @binding(0) var<uniform> F: Frame;
@group(0) @binding(1) var<uniform> W: Water;
@group(0) @binding(2) var depthTex: texture_depth_2d;
@group(0) @binding(3) var litCopyTex: texture_2d<f32>;   // the opaque frame (#842 snapshot)
@group(0) @binding(4) var litSampler: sampler;

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

@vertex
fn vs(@location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {
  var o: VsOut;
  let extent = W.centreExtent.w;
  let river = W.tuning.z > 0.5;
  // A lake is the unit grid scaled to the body; a river is its own baked
  // world-space ribbon (#833) whose uv is metres along / across the flow.
  var base = vec3<f32>(W.centreExtent.x + p.x * extent, W.centreExtent.y + p.y * extent, W.centreExtent.z);
  if (river) { base = p; }
  let t = W.camera.w;
  var disp = vec3<f32>(0.0, 0.0, 0.0);
  var dn = vec3<f32>(0.0, 0.0, 0.0);
  if (!river && W.tuning.y > 0.5) { gerstner(W.wave0, W.wave0b, base.xy, t, &disp, &dn); }
  if (!river && W.tuning.y > 1.5) { gerstner(W.wave1, W.wave1b, base.xy, t, &disp, &dn); }
  let world = base + disp;
  o.world = world;
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
  var alpha = mix(0.45, 0.95, depthT) * W.deep.w;

  // Toon ripples + shoreline foam: world-space value noise, panned, warped by a
  // second octave, then posterised by a depth-driven cutoff.
  // Distortion (world units) warps the vertex-panned ripple coordinate; the
  // ripple scale (W.foam.y) converts it, so this equals the old per-fragment
  // ((uvW + distortion) * scale + pan) exactly, minus the per-pixel maths.
  let distortion = vec2<f32>(
    raeNoiseValue2(i.distortUvA, 11u) - 0.5,
    raeNoiseValue2(i.distortUvB, 23u) - 0.5) * W.tuning.x;
  let ripple = raeNoiseValue2(i.rippleUv + distortion * W.foam.y, 7u);
  let foamT = clamp(waterDepth / max(W.foam.x, 0.01), 0.0, 1.0);
  let cutoff = foamT * W.foam.w;
  var foam = smoothstep(cutoff - 0.03, cutoff + 0.03, ripple);
  // River rapids: the baked curvature foam, streaked by the same noise.
  foam = max(foam, i.bakedFoam * smoothstep(0.35, 0.75, ripple));
  colour = mix(colour, vec3<f32>(0.96, 0.98, 1.0), foam * 0.85);
  alpha = max(alpha, foam * 0.9);

  // Reflection: the stylised sky hemisphere along the reflected view ray,
  // weighted by Schlick Fresnel (F0 = 0.02, water's normal-incidence reflectance).
  let n = normalize(i.nrm);
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
  } else {
    colour = mix(colour, skyTint, fresnel);
  }

  // Toon shading: a two-band diffuse and a STEPPED Blinn-Phong sun highlight.
  let toSun = normalize(-W.sun.xyz);
  let ndl = clamp(dot(n, toSun), 0.0, 1.0);
  colour *= 0.78 + 0.22 * smoothstep(0.25, 0.32, ndl);
  let h = normalize(toSun + toCamera);
  let spec = pow(clamp(dot(n, h), 0.0, 1.0), W.sunColor.w);
  let highlight = smoothstep(W.sun.w - 0.03, W.sun.w + 0.03, spec);
  colour += W.sunColor.rgb * highlight * 0.7;
  alpha = max(alpha, highlight * 0.8);

  return vec4<f32>(colour, alpha);
}
