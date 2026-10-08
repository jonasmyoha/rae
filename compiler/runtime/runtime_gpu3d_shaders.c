/* gpu3d — the forward renderer's WGSL shader sources (split from
 * runtime_gpu3d.c for the 1 000-line cap): the shared shadow lookup, the scene
 * shader, the tonemap and the TAA resolve. WGSL text only; the pipelines that
 * compile it are in runtime_gpu3d.c, which includes this file in place.
 */

/* Shared shadow lookup (#382/#384). ONE definition used by both the
 * static and the skinned shader — they must agree about shadowing as
 * exactly as they agree about the BRDF, and two copies of thirty lines of
 * WGSL drift. Each shader declares its own bindings (the numbers differ)
 * with these names; the function below references only the names. */
#define G3D_SHADOW_FN_WGSL \
"const SUN_TAN_HALF: f32 = 0.00463;\n" \
"fn poissonDisc(i: i32) -> vec2<f32> {\n" \
"  var p = array<vec2<f32>, 12>(\n" \
"    vec2<f32>(-0.326, -0.406), vec2<f32>(-0.840, -0.074),\n" \
"    vec2<f32>(-0.696,  0.457), vec2<f32>(-0.203,  0.621),\n" \
"    vec2<f32>( 0.962, -0.195), vec2<f32>( 0.473, -0.480),\n" \
"    vec2<f32>( 0.519,  0.767), vec2<f32>( 0.185, -0.893),\n" \
"    vec2<f32>( 0.507,  0.064), vec2<f32>( 0.896,  0.412),\n" \
"    vec2<f32>(-0.322, -0.933), vec2<f32>(-0.792, -0.598));\n" \
"  return p[i];\n" \
"}\n" \
/* Interleaved gradient noise, the same generator the SSAO pass uses, so
 * the two dithers are at least drawn from one family rather than being
 * two arbitrary hashes. */ \
"fn shadowNoise(pix: vec2<f32>) -> f32 {\n" \
"  return fract(52.9829189 * fract(dot(pix, vec2<f32>(0.06711056, 0.00583715))));\n" \
"}\n" \
"fn shadowCascadeIndex(viewDepth: f32, count: i32) -> i32 {\n" \
"  var c = 0;\n" \
"  if (viewDepth > SH.splitFar.x) { c = 1; }\n" \
"  if (viewDepth > SH.splitFar.y) { c = 2; }\n" \
"  if (viewDepth > SH.splitFar.z) { c = 3; }\n" \
"  return c;\n" \
"}\n" \
"fn cascadeTexel(c: i32) -> f32 {\n" \
"  if (c == 1) { return SH.texelWorld.y; }\n" \
"  if (c == 2) { return SH.texelWorld.z; }\n" \
"  if (c == 3) { return SH.texelWorld.w; }\n" \
"  return SH.texelWorld.x;\n" \
"}\n" \
"fn cascadeDepthRange(c: i32) -> f32 {\n" \
"  if (c == 1) { return SH.depthRange.y; }\n" \
"  if (c == 2) { return SH.depthRange.z; }\n" \
"  if (c == 3) { return SH.depthRange.w; }\n" \
"  return SH.depthRange.x;\n" \
"}\n" \
"fn sunVisibility(wpos: vec3<f32>, N: vec3<f32>, viewDepth: f32, pix: vec2<f32>) -> f32 {\n" \
"  let count = i32(SH.shadowCfg.x);\n" \
"  if (count <= 0) { return 1.0; }\n" \
"  let c = shadowCascadeIndex(viewDepth, count);\n" \
"  if (c >= count) { return 1.0; }\n" \
"  let texel = cascadeTexel(c);\n" \
/* Normal-offset bias: push the receiver along its geometric normal by
 * about a texel before projecting. Depth bias alone detaches a contact
 * shadow from its object, which is the artefact this whole system exists
 * to remove. */ \
"  let offset = wpos + N * (texel * 1.5);\n" \
"  let lp = SH.lightViewProj[c] * vec4<f32>(offset, 1.0);\n" \
"  let ndc = lp.xyz / lp.w;\n" \
"  let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);\n" \
"  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) { return 1.0; }\n" \
"  let res = SH.shadowCfg.y;\n" \
"  let extent = texel * res;\n" \
/* BLOCKER SEARCH. Average the depth of everything nearer to the light
 * than this receiver, over a small fixed disc. textureLoad rather than a
 * sampler: this reads depth VALUES, it does not compare them. */ \
"  let searchTexels = 4.0;\n" \
"  let searchUv = searchTexels / res;\n" \
"  var blockerSum = 0.0;\n" \
"  var blockerCount = 0.0;\n" \
"  let ang = shadowNoise(pix) * 6.2831853;\n" \
"  let ca = cos(ang);\n" \
"  let sa = sin(ang);\n" \
"  for (var i = 0; i < 8; i = i + 1) {\n" \
"    let d0 = poissonDisc(i);\n" \
"    let d = vec2<f32>(d0.x * ca - d0.y * sa, d0.x * sa + d0.y * ca) * searchUv;\n" \
"    let t = uv + d;\n" \
"    if (t.x < 0.0 || t.x > 1.0 || t.y < 0.0 || t.y > 1.0) { continue; }\n" \
"    let px = vec2<i32>(i32(t.x * res), i32(t.y * res));\n" \
"    let z = textureLoad(shadowTex, px, c, 0);\n" \
"    if (z < ndc.z) { blockerSum = blockerSum + z; blockerCount = blockerCount + 1.0; }\n" \
"  }\n" \
/* Fully lit: nothing between this point and the sun. */ \
"  if (blockerCount < 0.5) { return 1.0; }\n" \
"  let blockerZ = blockerSum / blockerCount;\n" \
/* PENUMBRA FOR A DIRECTIONAL LIGHT: width grows with the receiver-to-
 * blocker GAP alone, not with the ratio of distances. The PCSS ratio form
 * is for an area light at finite distance and is far too soft at contact,
 * which is precisely where a shadow must stay sharp to read as contact.
 * See docs/shadow-system-design.md §3 A2. */ \
"  let gapWorld = (ndc.z - blockerZ) * cascadeDepthRange(c);\n" \
"  let penumbraWorld = gapWorld * SUN_TAN_HALF * 2.0;\n" \
"  var radiusUv = penumbraWorld / max(extent, 1e-4);\n" \
/* Floor at one texel so the filter always covers the hardware's own\n" \
 * bilinear footprint; ceiling bounds the cost and stops a distant blocker
 * smearing a shadow across the cascade. */ \
"  radiusUv = clamp(radiusUv, 1.0 / res, 16.0 / res);\n" \
"  var sum = 0.0;\n" \
"  for (var i = 0; i < 12; i = i + 1) {\n" \
"    let d0 = poissonDisc(i);\n" \
"    let d = vec2<f32>(d0.x * ca - d0.y * sa, d0.x * sa + d0.y * ca) * radiusUv;\n" \
"    sum = sum + textureSampleCompareLevel(shadowTex, shadowSamp, uv + d, c, ndc.z);\n" \
"  }\n" \
"  return sum / 12.0;\n" \
"}\n"

static const char* G3D_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"     /* xyz cam, w time */
"  sunDir: vec4<f32>,\n"     /* xyz dir (toward scene), w exposure */
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"  invViewProj: mat4x4<f32>,\n"
"  prevViewProj: mat4x4<f32>,\n"
"  jitter: vec4<f32>,\n"   /* xy = this frame's sub-pixel clip offset (#335) */
"};\n"
"struct DrawU {\n"
"  model: mat4x4<f32>,\n"
"  prevModel: mat4x4<f32>,\n"
"  baseColorMetallic: vec4<f32>,\n"
"  emissiveRoughness: vec4<f32>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> draws: array<DrawU>;\n"
/* Shadow cascades (#382). `shadowCfg.x` is the live cascade count, so
 * zero disables the whole thing without swapping pipelines. */
"struct ShadowU {\n"
"  lightViewProj: array<mat4x4<f32>, 4>,\n"
"  splitFar: vec4<f32>,\n"
"  texelWorld: vec4<f32>,\n"
"  depthRange: vec4<f32>,\n"
"  shadowCfg: vec4<f32>,\n"
"};\n"
"@group(0) @binding(2) var<uniform> SH: ShadowU;\n"
"@group(0) @binding(3) var shadowTex: texture_depth_2d_array;\n"
"@group(0) @binding(4) var shadowSamp: sampler_comparison;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) wpos: vec3<f32>,\n"
"  @location(1) nrm: vec3<f32>,\n"
"  @location(2) uv: vec2<f32>,\n"
"  @location(3) @interpolate(flat) inst: u32,\n"
"  @location(4) clipNow: vec4<f32>,\n"
"  @location(5) clipPrev: vec4<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32,\n"
"      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {\n"
"  let d = draws[ii];\n"
"  let wp = d.model * vec4<f32>(p, 1.0);\n"
"  var o: VsOut;\n"
"  o.pos = F.viewProj * wp;\n"
"  o.wpos = wp.xyz;\n"
/* Uniform-scale normal transform (mat3 of model). Non-uniform scales
 * need transpose(inverse) — document as a gpu3d constraint for now. */
"  o.nrm = normalize((d.model * vec4<f32>(n, 0.0)).xyz);\n"
"  o.uv = uv;\n"
"  o.inst = ii;\n"
/* Motion vectors use UNJITTERED clip positions: jitter is a rasterisation
 * trick, and letting it leak into velocity would make every static pixel
 * report sub-pixel motion and smear the history. clipPrev runs the
 * PREVIOUS model matrix through the PREVIOUS viewProj, so a moving object
 * reports its own motion, not just the camera's (#335/#336). */
"  o.clipNow = o.pos;\n"
"  o.clipPrev = F.prevViewProj * (d.prevModel * vec4<f32>(p, 1.0));\n"
/* Jitter AFTER clipNow is captured. Multiplying by w keeps the offset a
 * constant sub-pixel amount in NDC after the perspective divide. */
"  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);\n"
"  return o;\n"
"}\n"
/* UV-space motion vector from two clip positions. Perspective divide in
 * the fragment stage (dividing in the vertex stage would interpolate
 * wrongly across the triangle). Y flips because NDC is +up, UV is +down. */
"fn motionVec(clipNow: vec4<f32>, clipPrev: vec4<f32>) -> vec2<f32> {\n"
"  let now = clipNow.xy / clipNow.w;\n"
"  let prev = clipPrev.xy / clipPrev.w;\n"
"  return (now - prev) * vec2<f32>(0.5, -0.5);\n"
"}\n"
"const PI: f32 = 3.14159265;\n"
"fn dGGX(NoH: f32, rough: f32) -> f32 {\n"
"  let a = rough * rough;\n"
"  let a2 = a * a;\n"
"  let d = NoH * NoH * (a2 - 1.0) + 1.0;\n"
"  return a2 / (PI * d * d + 1e-5);\n"
"}\n"
"fn gSmith(NoV: f32, NoL: f32, rough: f32) -> f32 {\n"
"  let k = (rough + 1.0) * (rough + 1.0) / 8.0;\n"
"  let gv = NoV / (NoV * (1.0 - k) + k);\n"
"  let gl = NoL / (NoL * (1.0 - k) + k);\n"
"  return gv * gl;\n"
"}\n"
"fn fresnel(VoH: f32, f0: vec3<f32>) -> vec3<f32> {\n"
"  return f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - VoH, 5.0);\n"
"}\n"
G3D_SHADOW_FN_WGSL
"struct FsOut {\n"
"  @location(0) color: vec4<f32>,\n"
"  @location(1) normal: vec4<f32>,\n"
"  @location(2) velocity: vec2<f32>,\n"
"  @location(3) ambient: vec4<f32>,\n"
"};\n"
"@fragment\n"
"fn fs(in: VsOut) -> FsOut {\n"
"  let d = draws[in.inst];\n"
"  let albedo = d.baseColorMetallic.rgb;\n"
"  let metallic = clamp(d.baseColorMetallic.a, 0.0, 1.0);\n"
"  let rough = clamp(d.emissiveRoughness.a, 0.045, 1.0);\n"
"  let N = normalize(in.nrm);\n"
"  let V = normalize(F.camPos.xyz - in.wpos);\n"
"  let L = normalize(-F.sunDir.xyz);\n"
"  let H = normalize(V + L);\n"
"  let NoV = max(dot(N, V), 1e-4);\n"
"  let NoL = max(dot(N, L), 0.0);\n"
"  let NoH = max(dot(N, H), 0.0);\n"
"  let VoH = max(dot(V, H), 0.0);\n"
"  let f0 = mix(vec3<f32>(0.04), albedo, metallic);\n"
"  let Fs = fresnel(VoH, f0);\n"
"  let spec = dGGX(NoH, rough) * gSmith(NoV, NoL, rough) * Fs\n"
"           / max(4.0 * NoV * NoL, 1e-4);\n"
"  let kd = (vec3<f32>(1.0) - Fs) * (1.0 - metallic);\n"
/* Shadow multiplies DIRECT only, never the ambient below. That split is
 * the composition invariant from docs/shadow-system-design.md §5, and it
 * is what keeps AO (indirect) and shadows (direct) from double-darkening. */
"  let viewDepth = length(F.camPos.xyz - in.wpos);\n"
"  let sunVis = sunVisibility(in.wpos, N, viewDepth, in.pos.xy);\n"
"  let direct = (kd * albedo / PI + spec) * F.sunColor.rgb * NoL * sunVis;\n"
"  let hemi = mix(F.ambGround.rgb, F.ambSky.rgb, N.y * 0.5 + 0.5);\n"
"  let ambF = fresnel(NoV, f0);\n"
"  let ambient = hemi * albedo * (1.0 - metallic) + hemi * ambF * (1.0 - rough * 0.7);\n"
/* LINEAR HDR out (#334) — exposure/ACES/gamma live in the tonemap pass.
 * The INDIRECT term goes to its own target instead of into colour (#337):
 * ambient occlusion must attenuate indirect light only. A surface in
 * direct sun inside a crevice is still lit, and keeping the split is also
 * what stops AO double-darkening once real GI lands. */
"  let c = direct + d.emissiveRoughness.rgb;\n"
"  var o: FsOut;\n"
"  o.color = vec4<f32>(c, 1.0);\n"
"  o.ambient = vec4<f32>(ambient, 1.0);\n"
"  o.normal = vec4<f32>(N, 1.0);\n"
"  o.velocity = motionVec(in.clipNow, in.clipPrev);\n"
"  return o;\n"
"}\n";

/* Tonemap: HDR (rgba16f, linear) -> LDR offscreen. textureLoad by pixel
 * coordinate — 1:1 fullscreen needs no sampler. Exposure rides in
 * F.sunDir.w exactly as it did inside the material shaders. */
static const char* G3D_TONEMAP_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"
"  sunDir: vec4<f32>,\n"
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"  invViewProj: mat4x4<f32>,\n"
"  prevViewProj: mat4x4<f32>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var hdrTex: texture_2d<f32>;\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n"
"  var points = array<vec2<f32>, 3>(\n"
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n"
"  return vec4<f32>(points[vi], 0.0, 1.0);\n"
"}\n"
"fn aces(x: vec3<f32>) -> vec3<f32> {\n"
"  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),\n"
"               vec3<f32>(0.0), vec3<f32>(1.0));\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let hdr = textureLoad(hdrTex, vec2<i32>(pos.xy), 0).rgb;\n"
"  var c = aces(hdr * F.sunDir.w);\n"
"  c = pow(c, vec3<f32>(1.0 / 2.2));\n"
"  return vec4<f32>(c, 1.0);\n"
"}\n";

/* TAA resolve: reproject last frame through the velocity buffer, clamp it
 * to the neighbourhood of the current pixel, and blend.
 *
 * The neighbourhood clamp is what makes temporal accumulation safe: an
 * unclamped history smears whenever reprojection is wrong (disocclusion,
 * shading changes, anything velocity cannot describe). Clipping history
 * to the min/max of the 3x3 current neighbourhood bounds the error to
 * something the current frame actually contains.
 *
 * Blending is luminance-weighted (Karis): weighting each sample by
 * 1/(1+luma) stops a single very bright pixel from dominating the
 * average and flickering between frames, which plain averaging in HDR
 * does badly. */
static const char* G3D_TAA_WGSL =
"@group(0) @binding(0) var curTex: texture_2d<f32>;\n"
"@group(0) @binding(1) var histTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var velTex: texture_2d<f32>;\n"
"@group(0) @binding(3) var histSampler: sampler;\n"
"@group(0) @binding(4) var<uniform> P: vec4<f32>;\n"   /* x=blend, y=historyValid */
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n"
"  var pts = array<vec2<f32>, 3>(\n"
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n"
"  return vec4<f32>(pts[vi], 0.0, 1.0);\n"
"}\n"
"fn lumaWeight(c: vec3<f32>) -> f32 {\n"
"  return 1.0 / (1.0 + max(max(c.r, c.g), c.b));\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let ipos = vec2<i32>(pos.xy);\n"
"  let dims = vec2<f32>(textureDimensions(curTex));\n"
"  let cur = textureLoad(curTex, ipos, 0).rgb;\n"
"  if (P.y < 0.5) { return vec4<f32>(cur, 1.0); }\n"
/* 3x3 neighbourhood bounds of the current frame. */
"  var lo = cur;\n"
"  var hi = cur;\n"
"  for (var dy = -1; dy <= 1; dy = dy + 1) {\n"
"    for (var dx = -1; dx <= 1; dx = dx + 1) {\n"
"      let q = clamp(ipos + vec2<i32>(dx, dy), vec2<i32>(0), vec2<i32>(dims) - vec2<i32>(1));\n"
"      let s = textureLoad(curTex, q, 0).rgb;\n"
"      lo = min(lo, s); hi = max(hi, s);\n"
"    }\n"
"  }\n"
/* Reproject: velocity is (now - prev) in UV, so the history sample sits
 * at uv - velocity. Sampled bilinearly because it lands off-grid. */
"  let uv = (pos.xy) / dims;\n"
"  let vel = textureLoad(velTex, ipos, 0).rg;\n"
"  let prevUv = uv - vel;\n"
/* Off-screen history is no history: reject rather than clamp to edge. */
"  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {\n"
"    return vec4<f32>(cur, 1.0);\n"
"  }\n"
"  let histRaw = textureSampleLevel(histTex, histSampler, prevUv, 0.0).rgb;\n"
"  let hist = clamp(histRaw, lo, hi);\n"
"  let wc = lumaWeight(cur) * (1.0 - P.x);\n"
"  let wh = lumaWeight(hist) * P.x;\n"
"  let outC = (cur * wc + hist * wh) / max(wc + wh, 1e-5);\n"
"  return vec4<f32>(outC, 1.0);\n"
"}\n";
