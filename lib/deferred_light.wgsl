struct LightU {
  invViewProj: mat4x4<f32>,
  camPos: vec4<f32>,
  sunDir: vec4<f32>,
  sunColor: vec4<f32>,
  ambSky: vec4<f32>,
  ambGround: vec4<f32>,
  clearColor: vec4<f32>,
  viewProj: mat4x4<f32>,
  skyParams: vec4<f32>,
  skyZenith: vec4<f32>,
  skyHorizon: vec4<f32>,
  hosek: array<vec4<f32>, 9>,
};
@group(0) @binding(0) var<uniform> L: LightU;
@group(0) @binding(1) var gbaTex: texture_2d<f32>;
@group(0) @binding(2) var gbbTex: texture_2d<f32>;
@group(0) @binding(3) var gbcTex: texture_2d<f32>;
@group(0) @binding(4) var depthTex: texture_depth_2d;
@group(0) @binding(5) var aoTex: texture_2d<f32>;
struct ShadowU {
  lightViewProj: array<mat4x4<f32>, 4>,
  splitFar: vec4<f32>,
  texelWorld: vec4<f32>,
  depthRange: vec4<f32>,
  shadowCfg: vec4<f32>,
};
@group(0) @binding(6) var<uniform> SH: ShadowU;
@group(0) @binding(7) var shadowTex: texture_depth_2d_array;
@group(0) @binding(8) var shadowSamp: sampler_comparison;
// The SSAO's half-resolution depth (deferred_ao_depth.wgsl), for the upsample.
@group(0) @binding(9) var aoDepthTex: texture_2d<f32>;
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
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
@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
  let px = vec2<i32>(pos.xy);
  let depth = textureLoad(depthTex, px, 0);
  if (depth <= 0.0) {
    let uvF = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(textureDimensions(gbbTex, 0));
    let ndcF = vec4<f32>(uvF.x * 2.0 - 1.0, 1.0 - uvF.y * 2.0, 0.0, 1.0);
    if (L.clearColor.w > 0.5) { return vec4<f32>(L.clearColor.rgb, 1.0); }
    let farW = L.invViewProj * ndcF;
    let dirF = normalize(farW.xyz / farW.w - L.camPos.xyz);
    return vec4<f32>(skyColor(dirF), 1.0);
  }
  let gbb = textureLoad(gbbTex, px, 0);
  let albedo = half3(gbb.rgb);
  let gba = textureLoad(gbaTex, px, 0);
  let mode = gba.w;
  let isEmissive = mode > 0.16 && mode < 0.5;
  let isUnlit = mode > 0.5 && mode < 0.83;
  let isToon = mode > 0.83;
  if (isUnlit) {
    return vec4<f32>(gbb.rgb, 1.0);
  }
  let dim = vec2<f32>(textureDimensions(gbbTex, 0));
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / dim;
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  let wpos4 = L.invViewProj * ndc;
  let wpos = wpos4.xyz / wpos4.w;
  let N = octDecode(gba.xy);
  let gbc = textureLoad(gbcTex, px, 0);
  let metallic = half(clamp(gbc.z, 0.0, 1.0));
  let roughF = clamp(gbb.a, 0.045, 1.0);
  let rough = half(roughF);
  var ao = half(gbc.w);
  var emissive = half(0.0);
  if (isEmissive) {
    emissive = half(exp(gbc.w * 6.91) - 1.0);
    ao = 1.0;
  }
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
  // The SSAO is half resolution (render-perf-6): upsample it from the four
  // half texels around this pixel, bilinear weights times depth similarity,
  // so the AO of a surface does not bleed onto the one in front of it.
  var aoSum = 0.0;
  var aoW = 0.0;
  let dims = vec2<i32>(textureDimensions(aoTex));
  let halfPos = (vec2<f32>(px) + vec2<f32>(0.5)) * 0.5 - vec2<f32>(0.5);
  let halfBase = vec2<i32>(floor(halfPos));
  let halfFrac = halfPos - floor(halfPos);
  for (var i = 0; i < 4; i = i + 1) {
    let o = vec2<i32>(i & 1, i >> 1u);
    let q = clamp(halfBase + o, vec2<i32>(0), dims - vec2<i32>(1));
    let qd = textureLoad(aoDepthTex, q, 0).x;
    if (qd <= 0.0) { continue; }
    let bx = select(1.0 - halfFrac.x, halfFrac.x, o.x == 1);
    let by = select(1.0 - halfFrac.y, halfFrac.y, o.y == 1);
    let w = (bx * by + 1e-3) / (1.0 + abs(qd - depth) / depth * 40.0);
    aoSum = aoSum + textureLoad(aoTex, q, 0).r * w;
    aoW = aoW + w;
  }
  var ssao = half(1.0);
  if (aoW > 0.0) { ssao = half(aoSum / aoW); }
  let ambient = (hemi * albedo * (1.0 - metallic) + hemi * ambF) * ao * ssao;
  let emit = albedo * emissive;
  let rimF = pow(clamp(1.0 - NoV, half(0.0), half(1.0)), half(RAE_RIM_POWER));
  let rimUp = 1.0 - clamp(Nh.z, half(0.0), half(1.0));
  let rim = half3(L.ambSky.rgb) * (rimF * rimUp * RAE_RIM_STRENGTH);
  let contactAO = mix(half(1.0), ao * ssao, half(RAE_AO_CONTACT));
  let castShadowAmb = mix(half(1.0), sunVis, half(SH.shadowCfg.z));
  var lit = direct * contactAO + ambient * castShadowAmb + emit + rim;
  if (isToon) { lit = toonShaded * mix(half(1.0), ao * ssao, half(0.6)); }
  let radiance = clamp(vec3<f32>(lit), vec3<f32>(0.0), vec3<f32>(64000.0));
  if (L.clearColor.w > 0.5) {
    let fogDist = length(L.camPos.xyz - wpos);
    let fogT = clamp((fogDist - L.sunColor.w) / max(L.ambSky.w - L.sunColor.w, 0.001), 0.0, 1.0);
    return vec4<f32>(mix(radiance, L.clearColor.rgb, fogT), 1.0);
  }
  return vec4<f32>(radiance, 1.0);
}
