// The surface shading both main passes run (forward-rae-1): the deferred
// lighting pass (deferred_light.wgsl) calls shadeSurface with what the G-buffer
// holds for a pixel, a forward material's fragment entry (forward_static.wgsl)
// with what it just computed, so the two paths shade the same. A PART of a
// composition: the includer declares the bindings these functions read, `L`
// (LightU), `SH` (ShadowU), `shadowTex` and `shadowSamp`, at its own binding
// numbers, and composes the precision prelude, gbuffer_octahedral.wgsl,
// sky_hosek.wgsl and deferred_uniforms.wgsl in front.
fn shadowCascadeIndex(viewDepth: f32, count: i32) -> i32 {
  var c = 0;
  if (viewDepth > SH.splitFar.x) { c = 1; }
  if (viewDepth > SH.splitFar.y) { c = 2; }
  if (viewDepth > SH.splitFar.z) { c = 3; }
  return c;
}
fn cascadeTexel(c: i32) -> f32 {
  if (c == 1) { return SH.texelWorld.y; }
  if (c == 2) { return SH.texelWorld.z; }
  if (c == 3) { return SH.texelWorld.w; }
  return SH.texelWorld.x;
}
fn cascadeDepthRange(c: i32) -> f32 {
  if (c == 1) { return SH.depthRange.y; }
  if (c == 2) { return SH.depthRange.z; }
  if (c == 3) { return SH.depthRange.w; }
  return SH.depthRange.x;
}
// The receiving surface's light-space depth slope (render-perf-7): the plane
// through the pixel with its G-buffer normal, mapped by the cascade's
// orthographic (so linear) view-projection, as d(depth)/d(uv). Each PCF tap
// compares against the depth of that plane under the tap rather than the
// pixel's own depth, so a slope does not shadow itself and no normal offset
// is needed: contacts stay attached. (dpdx/dpdy would give the same plane,
// but the lighting's early returns make the control flow non-uniform.)
fn receiverSlope(c: i32, N: vec3<f32>, maxSlope: f32) -> vec2<f32> {
  var t1 = cross(N, vec3<f32>(0.0, 0.0, 1.0));
  if (dot(t1, t1) < 1e-6) { t1 = cross(N, vec3<f32>(1.0, 0.0, 0.0)); }
  t1 = normalize(t1);
  let t2 = cross(N, t1);
  let m = SH.lightViewProj[c];
  let a = (m * vec4<f32>(t1, 0.0)).xyz;
  let b = (m * vec4<f32>(t2, 0.0)).xyz;
  let n = cross(a, b);
  if (abs(n.z) < 1e-8) { return vec2<f32>(0.0); }
  // ndc.x = 2u - 1 and ndc.y = 1 - 2v
  let slope = vec2<f32>(-2.0 * n.x / n.z, 2.0 * n.y / n.z);
  let length2 = dot(slope, slope);
  if (length2 > maxSlope * maxSlope) { return slope * (maxSlope / sqrt(length2)); }
  return slope;
}
// Four taps of the hardware 2x2 compare, 0.75 texel either side: a smooth
// footprint of about 3.5 texels for 4 lookups (the PCSS filter was 8 blocker
// + 12 PCF taps per pixel).
fn sunVisibility(wpos: vec3<f32>, N: vec3<f32>, viewDepth: f32) -> f32 {
  let count = i32(SH.shadowCfg.x);
  if (count <= 0) { return 1.0; }
  let c = shadowCascadeIndex(viewDepth, count);
  if (c >= count) { return 1.0; }
  let lp = SH.lightViewProj[c] * vec4<f32>(wpos, 1.0);
  let ndc = lp.xyz / lp.w;
  let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) { return 1.0; }
  let res = SH.shadowCfg.y;
  // One texel of world size, in this cascade's depth units
  let texelDepth = cascadeTexel(c) / max(cascadeDepthRange(c), 1e-4);
  // At most a slope of 8 texel-depths per texel (grazing light)
  let slope = receiverSlope(c, N, 8.0 * texelDepth * res);
  // The hardware compare reads 2x2 texels around each tap, up to 0.7 texel
  // off the tap, where the plane is already deeper or shallower.
  let bias = 0.5 * texelDepth + 0.75 * length(slope) / res;
  var sum = 0.0;
  for (var i = 0; i < 4; i = i + 1) {
    let d = (vec2<f32>(f32(i & 1), f32(i >> 1u)) * 1.5 - vec2<f32>(0.75)) / res;
    let reference = ndc.z + dot(slope, d) - bias;
    sum = sum + textureSampleCompareLevel(shadowTex, shadowSamp, uv + d, c, reference);
  }
  return sum * 0.25;
}
// The cascade debug view (SH.shadowCfg.w, setShadowCascadeDebug): red, green,
// blue and yellow from the camera out
fn cascadeDebugColor(c: i32) -> vec3<f32> {
  if (c == 1) { return vec3<f32>(0.25, 0.95, 0.3); }
  if (c == 2) { return vec3<f32>(0.3, 0.55, 1.0); }
  if (c == 3) { return vec3<f32>(1.0, 0.85, 0.2); }
  return vec3<f32>(1.0, 0.3, 0.25);
}
fn cascadeSplitAt(c: i32) -> f32 {
  if (c == 1) { return SH.splitFar.y; }
  if (c == 2) { return SH.splitFar.z; }
  if (c == 3) { return SH.splitFar.w; }
  return SH.splitFar.x;
}
// Each cascade's area tinted in its colour, a line in that colour where the
// cascade ends (the cascade switch; the last line is where shadows stop) and
// each shadow map's square outlined in its colour, 3 of its texels wide
fn shadowCascadeDebug(radiance: vec3<f32>, wpos: vec3<f32>, viewDepth: f32) -> vec3<f32> {
  let count = i32(SH.shadowCfg.x);
  if (count <= 0) { return radiance; }
  var color = radiance;
  let c = shadowCascadeIndex(viewDepth, count);
  // A colour filter, so the shading and the shadows still read under it
  if (c < count) { color = radiance * (vec3<f32>(0.15) + 0.85 * cascadeDebugColor(c)); }
  let lineWidth = max(viewDepth * 0.004, 0.03);
  for (var i = 0; i < 4; i = i + 1) {
    if (i < count && abs(viewDepth - cascadeSplitAt(i)) < lineWidth) { color = cascadeDebugColor(i) * 0.7; }
  }
  let res = SH.shadowCfg.y;
  for (var i = 0; i < 4; i = i + 1) {
    if (i < count) {
      let lp = SH.lightViewProj[i] * vec4<f32>(wpos, 1.0);
      let ndc = lp.xyz / lp.w;
      let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
      let edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
      if (edge >= 0.0 && edge * res < 3.0) { color = cascadeDebugColor(i) * 0.7; }
    }
  }
  return color;
}
// render-perf-12: colour and shading arithmetic is `half` (the precision
// prelude composed in front: f16 with shader-f16, else f32). Depth, the
// world-position reconstruction, shadows, the AO upsample weights, the sky and
// the GGX specular term stay f32: GGX reaches ~7.8e4 for a smooth surface,
// above f16's 65504, and its small-roughness terms underflow.
const PI = 3.14159265;
const RAE_RIM_STRENGTH = 0.55;
const RAE_RIM_POWER = 3.2;
const RAE_AO_CONTACT = 0.5;
fn dGGX(NoH: f32, rough: f32) -> f32 {
  let a = rough * rough;
  let a2 = a * a;
  let d = NoH * NoH * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d + 1e-5);
}
fn gSmith(NoV: half, NoL: half, rough: half) -> half {
  let k = (rough + 1.0) * (rough + 1.0) / 8.0;
  let gv = NoV / (NoV * (1.0 - k) + k);
  let gl = NoL / (NoL * (1.0 - k) + k);
  return gv * gl;
}
fn fresnel(VoH: half, f0: half3) -> half3 {
  return f0 + (half3(1.0) - f0) * pow(1.0 - VoH, 5.0);
}
fn envBrdf(f0: half3, rough: half, NoV: half) -> half3 {
  let c0 = half4(-1.0, -0.0275, -0.572, 0.022);
  let c1 = half4(1.0, 0.0425, 1.04, -0.04);
  let r = rough * c0 + c1;
  let a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
  let ab = half2(-1.04, 1.04) * a004 + r.zw;
  return f0 * ab.x + half3(ab.y);
}
// One surface to shade. `mode` is the G-buffer's shading-mode channel (lit,
// emissive, unlit, toon); `ao` is the material occlusion (1 when emissive);
// `ssao` the screen-space AO (1 where there is none, as in the forward pass).
struct ShadedSurface {
  wpos: vec3<f32>,
  N: vec3<f32>,
  albedo: vec3<f32>,
  metallic: f32,
  rough: f32,
  ao: f32,
  emissive: f32,
  mode: f32,
  ssao: f32,
};
// The radiance of a surface (linear HDR), fog included
fn shadeSurface(s: ShadedSurface) -> vec3<f32> {
  let mode = s.mode;
  let isUnlit = mode > 0.5 && mode < 0.83;
  let isToon = mode > 0.83;
  if (isUnlit) {
    return s.albedo;
  }
  let albedo = half3(s.albedo);
  let wpos = s.wpos;
  let N = s.N;
  let metallic = half(clamp(s.metallic, 0.0, 1.0));
  let roughF = clamp(s.rough, 0.045, 1.0);
  let rough = half(roughF);
  let ao = half(s.ao);
  let emissive = half(s.emissive);
  let ssao = half(s.ssao);
  let V = normalize(L.camPos.xyz - wpos);
  let Ldir = normalize(-L.sunDir.xyz);
  let H = normalize(V + Ldir);
  let NoVf = max(dot(N, V), 1e-4);
  let NoLf = max(dot(N, Ldir), 0.0);
  let NoV = half(NoVf);
  let NoL = half(NoLf);
  let NoH = max(dot(N, H), 0.0);
  let VoH = half(max(dot(V, H), 0.0));
  let Nh = half3(N);
  let f0 = mix(half3(0.04), albedo, metallic);
  let Fs = fresnel(VoH, f0);
  // The specular lobe in f32 (see above), clamped like the radiance before
  // it joins the half sum
  let specF = dGGX(NoH, roughF) * f32(gSmith(NoV, NoL, rough)) * vec3<f32>(Fs) / max(4.0 * NoVf * NoLf, 1e-4);
  let kd = (half3(1.0) - Fs) * (1.0 - metallic);
  let sunVisF = sunVisibility(wpos, N, length(L.camPos.xyz - wpos));
  let sunVis = half(sunVisF);
  let directF = (vec3<f32>(kd * albedo) / PI + specF) * L.sunColor.rgb * NoLf * sunVisF;
  let direct = half3(min(directF, vec3<f32>(60000.0)));
  let hemi = mix(half3(L.ambGround.rgb), half3(L.ambSky.rgb), Nh.z * 0.5 + 0.5);
  let ambF = envBrdf(f0, rough, NoV);
  let hl = half(dot(N, Ldir) * 0.5 + 0.5);
  let toonT1 = smoothstep(half(0.0), half(0.03), clamp(hl - 0.50, 0.0, 1.0));
  let toonT2 = smoothstep(half(0.0), half(0.07), clamp(hl - 0.62, 0.0, 1.0));
  let shadeTint = mix(half3(1.0), half3(normalize(max(L.ambSky.rgb, vec3<f32>(0.001))) * 1.732), half(0.5));
  let toonLit = albedo;
  let toonMid = albedo * 0.62 * shadeTint;
  let toonDeep = albedo * 0.34 * shadeTint;
  var toonCol = mix(toonMid, toonLit, toonT2);
  toonCol = mix(toonDeep, toonCol, toonT1);
  toonCol = mix(toonDeep, toonCol, mix(half(1.0), sunVis, half(0.85)));
  let toonLight = half3(L.sunColor.rgb) * 0.34 + hemi * 0.75;
  let toonShaded = toonCol * toonLight;
  let ambient = (hemi * albedo * (1.0 - metallic) + hemi * ambF) * ao * ssao;
  let emit = albedo * emissive;
  let rimF = pow(clamp(1.0 - NoV, half(0.0), half(1.0)), half(RAE_RIM_POWER));
  let rimUp = 1.0 - clamp(Nh.z, half(0.0), half(1.0));
  let rim = half3(L.ambSky.rgb) * (rimF * rimUp * RAE_RIM_STRENGTH);
  let contactAO = mix(half(1.0), ao * ssao, half(RAE_AO_CONTACT));
  let castShadowAmb = mix(half(1.0), sunVis, half(SH.shadowCfg.z));
  var lit = direct * contactAO + ambient * castShadowAmb + emit + rim;
  if (isToon) { lit = toonShaded * mix(half(1.0), ao * ssao, half(0.6)); }
  var radiance = clamp(vec3<f32>(lit), vec3<f32>(0.0), vec3<f32>(64000.0));
  if (SH.shadowCfg.w > 0.5) {
    radiance = shadowCascadeDebug(radiance, wpos, length(L.camPos.xyz - wpos));
  }
  if (L.clearColor.w > 0.5) {
    let fogDist = length(L.camPos.xyz - wpos);
    let fogT = clamp((fogDist - L.sunColor.w) / max(L.ambSky.w - L.sunColor.w, 0.001), 0.0, 1.0);
    return mix(radiance, L.clearColor.rgb, fogT);
  }
  return radiance;
}
