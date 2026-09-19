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
@vertex
fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
  var points = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  return vec4<f32>(points[vi], 0.0, 1.0);
}
const SUN_TAN_HALF: f32 = 0.00463;
fn poissonDisc(i: i32) -> vec2<f32> {
  var p = array<vec2<f32>, 12>(
    vec2<f32>(-0.326, -0.406), vec2<f32>(-0.840, -0.074),
    vec2<f32>(-0.696,  0.457), vec2<f32>(-0.203,  0.621),
    vec2<f32>( 0.962, -0.195), vec2<f32>( 0.473, -0.480),
    vec2<f32>( 0.519,  0.767), vec2<f32>( 0.185, -0.893),
    vec2<f32>( 0.507,  0.064), vec2<f32>( 0.896,  0.412),
    vec2<f32>(-0.322, -0.933), vec2<f32>(-0.792, -0.598));
  return p[i];
}
fn shadowNoise(pix: vec2<f32>) -> f32 {
  return fract(52.9829189 * fract(dot(pix, vec2<f32>(0.06711056, 0.00583715))));
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
fn sunVisibility(wpos: vec3<f32>, N: vec3<f32>, viewDepth: f32, pix: vec2<f32>) -> f32 {
  let count = i32(SH.shadowCfg.x);
  if (count <= 0) { return 1.0; }
  let c = shadowCascadeIndex(viewDepth, count);
  if (c >= count) { return 1.0; }
  let texel = cascadeTexel(c);
  let offset = wpos + N * (texel * 1.5);
  let lp = SH.lightViewProj[c] * vec4<f32>(offset, 1.0);
  let ndc = lp.xyz / lp.w;
  let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) { return 1.0; }
  let res = SH.shadowCfg.y;
  let extent = texel * res;
  let searchTexels = 4.0;
  let searchUv = searchTexels / res;
  var blockerSum = 0.0;
  var blockerCount = 0.0;
  let ang = shadowNoise(pix) * 6.2831853;
  let ca = cos(ang);
  let sa = sin(ang);
  for (var i = 0; i < 8; i = i + 1) {
    let d0 = poissonDisc(i);
    let d = vec2<f32>(d0.x * ca - d0.y * sa, d0.x * sa + d0.y * ca) * searchUv;
    let t = uv + d;
    if (t.x < 0.0 || t.x > 1.0 || t.y < 0.0 || t.y > 1.0) { continue; }
    let px = vec2<i32>(i32(t.x * res), i32(t.y * res));
    let z = textureLoad(shadowTex, px, c, 0);
    if (z < ndc.z) { blockerSum = blockerSum + z; blockerCount = blockerCount + 1.0; }
  }
  if (blockerCount < 0.5) { return 1.0; }
  let blockerZ = blockerSum / blockerCount;
  let gapWorld = (ndc.z - blockerZ) * cascadeDepthRange(c);
  let penumbraWorld = gapWorld * SUN_TAN_HALF * 2.0;
  var radiusUv = penumbraWorld / max(extent, 1e-4);
  radiusUv = clamp(radiusUv, 1.0 / res, 16.0 / res);
  var sum = 0.0;
  for (var i = 0; i < 12; i = i + 1) {
    let d0 = poissonDisc(i);
    let d = vec2<f32>(d0.x * ca - d0.y * sa, d0.x * sa + d0.y * ca) * radiusUv;
    sum = sum + textureSampleCompareLevel(shadowTex, shadowSamp, uv + d, c, ndc.z);
  }
  return sum / 12.0;
}
const PI: f32 = 3.14159265;
const RAE_RIM_STRENGTH: f32 = 0.55;
const RAE_RIM_POWER: f32 = 3.2;
const RAE_AO_CONTACT: f32 = 0.5;
fn toonBand(x: f32, bands: f32) -> f32 {
  let s = clamp(x, 0.0, 1.0) * bands;
  let i = floor(s);
  let f = s - i;
  return (i + smoothstep(0.4, 0.6, f)) / bands;
}
fn dGGX(NoH: f32, rough: f32) -> f32 {
  let a = rough * rough;
  let a2 = a * a;
  let d = NoH * NoH * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d + 1e-5);
}
fn gSmith(NoV: f32, NoL: f32, rough: f32) -> f32 {
  let k = (rough + 1.0) * (rough + 1.0) / 8.0;
  let gv = NoV / (NoV * (1.0 - k) + k);
  let gl = NoL / (NoL * (1.0 - k) + k);
  return gv * gl;
}
fn fresnel(VoH: f32, f0: vec3<f32>) -> vec3<f32> {
  return f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - VoH, 5.0);
}
fn envBrdf(f0: vec3<f32>, rough: f32, NoV: f32) -> vec3<f32> {
  let c0 = vec4<f32>(-1.0, -0.0275, -0.572, 0.022);
  let c1 = vec4<f32>(1.0, 0.0425, 1.04, -0.04);
  let r = rough * c0 + c1;
  let a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
  let ab = vec2<f32>(-1.04, 1.04) * a004 + r.zw;
  return f0 * ab.x + vec3<f32>(ab.y);
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
  let albedo = gbb.rgb;
  let gba = textureLoad(gbaTex, px, 0);
  let mode = gba.w;
  let isEmissive = mode > 0.16 && mode < 0.5;
  let isUnlit = mode > 0.5 && mode < 0.83;
  let isToon = mode > 0.83;
  if (isUnlit) {
    return vec4<f32>(albedo, 1.0);
  }
  let dim = vec2<f32>(textureDimensions(gbbTex, 0));
  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / dim;
  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  let wpos4 = L.invViewProj * ndc;
  let wpos = wpos4.xyz / wpos4.w;
  let N = octDecode(gba.xy);
  let gbc = textureLoad(gbcTex, px, 0);
  let metallic = clamp(gbc.z, 0.0, 1.0);
  let rough = clamp(gbb.a, 0.045, 1.0);
  var ao = gbc.w;
  var emissive = 0.0;
  if (isEmissive) {
    emissive = exp(ao * 6.91) - 1.0;
    ao = 1.0;
  }
  let V = normalize(L.camPos.xyz - wpos);
  let Ldir = normalize(-L.sunDir.xyz);
  let H = normalize(V + Ldir);
  let NoV = max(dot(N, V), 1e-4);
  let NoL = max(dot(N, Ldir), 0.0);
  let NoH = max(dot(N, H), 0.0);
  let VoH = max(dot(V, H), 0.0);
  let f0 = mix(vec3<f32>(0.04), albedo, metallic);
  let Fs = fresnel(VoH, f0);
  let spec = dGGX(NoH, rough) * gSmith(NoV, NoL, rough) * Fs
           / max(4.0 * NoV * NoL, 1e-4);
  let kd = (vec3<f32>(1.0) - Fs) * (1.0 - metallic);
  let sunVis = sunVisibility(wpos, N, length(L.camPos.xyz - wpos), vec2<f32>(px));
  let direct = (kd * albedo / PI + spec) * L.sunColor.rgb * NoL * sunVis;
  let hemi = mix(L.ambGround.rgb, L.ambSky.rgb, N.z * 0.5 + 0.5);
  let ambF = envBrdf(f0, rough, NoV);
  let hl = dot(N, Ldir) * 0.5 + 0.5;
  let toonT1 = smoothstep(0.0, 0.03, clamp(hl - 0.50, 0.0, 1.0));
  let toonT2 = smoothstep(0.0, 0.07, clamp(hl - 0.62, 0.0, 1.0));
  let shadeTint = mix(vec3<f32>(1.0), normalize(max(L.ambSky.rgb, vec3<f32>(0.001))) * 1.732, 0.5);
  let toonLit = albedo;
  let toonMid = albedo * 0.62 * shadeTint;
  let toonDeep = albedo * 0.34 * shadeTint;
  var toonCol = mix(toonMid, toonLit, toonT2);
  toonCol = mix(toonDeep, toonCol, toonT1);
  toonCol = mix(toonDeep, toonCol, mix(1.0, sunVis, 0.85));
  let toonLight = L.sunColor.rgb * 0.34 + hemi * 0.75;
  let toonShaded = toonCol * toonLight;
  var aoSum = 0.0;
  var aoW = 0.0;
  let dims = vec2<i32>(textureDimensions(aoTex));
  for (var dy = -1; dy <= 1; dy = dy + 1) {
    for (var dx = -1; dx <= 1; dx = dx + 1) {
      let q = clamp(px + vec2<i32>(dx, dy), vec2<i32>(0), dims - vec2<i32>(1));
      let qd = textureLoad(depthTex, q, 0);
      if (qd <= 0.0) { continue; }
      let w = 1.0 / (1.0 + abs(qd - depth) * 800.0);
      aoSum = aoSum + textureLoad(aoTex, q, 0).r * w;
      aoW = aoW + w;
    }
  }
  var ssao = 1.0;
  if (aoW > 0.0) { ssao = aoSum / aoW; }
  let ambient = (hemi * albedo * (1.0 - metallic) + hemi * ambF) * ao * ssao;
  let emit = albedo * emissive;
  let rimF = pow(clamp(1.0 - NoV, 0.0, 1.0), RAE_RIM_POWER);
  let rimUp = 1.0 - clamp(N.z, 0.0, 1.0);
  let rim = L.ambSky.rgb * (rimF * rimUp * RAE_RIM_STRENGTH);
  let contactAO = mix(1.0, ao * ssao, RAE_AO_CONTACT);
  let castShadowAmb = mix(1.0, sunVis, SH.shadowCfg.z);
  var lit = direct * contactAO + ambient * castShadowAmb + emit + rim;
  if (isToon) { lit = toonShaded * mix(1.0, ao * ssao, 0.6); }
  let radiance = clamp(lit, vec3<f32>(0.0), vec3<f32>(64000.0));
  if (L.clearColor.w > 0.5) {
    let fogDist = length(L.camPos.xyz - wpos);
    let fogT = clamp((fogDist - L.sunColor.w) / max(L.ambSky.w - L.sunColor.w, 0.001), 0.0, 1.0);
    return vec4<f32>(mix(radiance, L.clearColor.rgb, fogT), 1.0);
  }
  return vec4<f32>(radiance, 1.0);
}
