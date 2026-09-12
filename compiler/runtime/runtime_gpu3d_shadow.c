/* gpu3d shadows — the WGSL of the cascaded shadow-map casters (#382, #925).
 *
 * Since #925 the cascade targets, pipelines, bind groups, uniforms, the caster
 * queue and the per-cascade depth passes are the renderer's Rae ShadowCache
 * (lib/ShadowMaps.rae + lib/ShadowMapsSdf.rae over the gpu/ manager); the
 * cascade FITTING is Rae too (lib/Shadow3d.rae, test 579). What C keeps is the
 * shader source — WGSL has no include, so it lives next to the forward /
 * deferred lighting shaders that must agree with it — read through the three
 * accessors at the end.
 *
 * ONE PIPELINE PER VERTEX FORMAT. Static (32-byte) and skinned (80-byte,
 * #374) geometry each need a depth-only variant, the skinned one running
 * the same palette skinning with no fragment stage. Miss this and the
 * character silently casts no shadow while everything else does — which
 * reads as a shadow bug, not as a missing pipeline.
 *
 * NOT REVERSE-Z. The main pass uses reverse-Z because it buys float
 * precision against perspective's 1/z. Cascades are orthographic, where
 * depth is linear, so reverse-Z would buy nothing and only risk a
 * convention mismatch. Standard Less depth test, clear to 1.0.
 */

static const char* G3D_SHADOW_WGSL =
"struct SU { lightViewProj: mat4x4<f32> };\n"
"@group(0) @binding(0) var<uniform> S: SU;\n"
"@group(0) @binding(1) var<storage, read> models: array<mat4x4<f32>>;\n"
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32, @location(0) p: vec3<f32>) -> @builtin(position) vec4<f32> {\n"
"  return S.lightViewProj * (models[ii] * vec4<f32>(p, 1.0));\n"
"}\n";

/* Skinned variant: same output, but the position is skinned by the joint
 * palette first. The palette layout matches runtime_gpu3d_skin.c exactly
 * (three vec4 rows per joint) — two decoders of one format is a bug
 * waiting to happen, so this one is a literal transcription. */
static const char* G3D_SHADOW_SKIN_WGSL =
"struct SU { lightViewProj: mat4x4<f32> };\n"
"@group(0) @binding(0) var<uniform> S: SU;\n"
"@group(0) @binding(1) var<storage, read> models: array<mat4x4<f32>>;\n"
"@group(0) @binding(2) var<storage, read> palette: array<vec4<f32>>;\n"
"@group(0) @binding(3) var<storage, read> paletteBases: array<u32>;\n"
"fn jointMat(j: u32, base: u32) -> mat4x4<f32> {\n"
"  let r0 = palette[base + j * 3u + 0u];\n"
"  let r1 = palette[base + j * 3u + 1u];\n"
"  let r2 = palette[base + j * 3u + 2u];\n"
"  return mat4x4<f32>(\n"
"    vec4<f32>(r0.x, r1.x, r2.x, 0.0),\n"
"    vec4<f32>(r0.y, r1.y, r2.y, 0.0),\n"
"    vec4<f32>(r0.z, r1.z, r2.z, 0.0),\n"
"    vec4<f32>(r0.w, r1.w, r2.w, 1.0));\n"
"}\n"
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32,\n"
"      @location(0) p: vec3<f32>,\n"
"      @location(3) jf: vec4<f32>, @location(4) w: vec4<f32>) -> @builtin(position) vec4<f32> {\n"
"  let j = vec4<u32>(u32(jf.x), u32(jf.y), u32(jf.z), u32(jf.w));\n"
"  let pbase = paletteBases[ii];\n"
"  var skin = jointMat(j.x, pbase) * w.x;\n"
"  skin = skin + jointMat(j.y, pbase) * w.y;\n"
"  skin = skin + jointMat(j.z, pbase) * w.z;\n"
"  skin = skin + jointMat(j.w, pbase) * w.w;\n"
"  let sp = skin * vec4<f32>(p, 1.0);\n"
"  return S.lightViewProj * (models[ii] * vec4<f32>(sp.xyz, 1.0));\n"
"}\n";

/* ----- metaball clusters as shadow CASTERS ---------------------------
 *
 * Metaballs have no triangles, so the cascade pass cannot rasterise them
 * and they were the one thing in example 110 that floated without a
 * shadow. The design doc assigns SDF shadowing to Layer C (#386), which
 * cone-traces the GI representation — but that is for RECEIVING soft
 * far-field shadows and needs a representation that does not exist yet.
 * Making a metaball CAST into an existing cascade is much smaller: march
 * the same field from the light instead of from the eye, and write depth.
 *
 * ORTHOGRAPHIC RAYS. Under a directional light every ray is parallel, so
 * the ray direction is constant and only the origin varies per shadow-map
 * pixel. That is why this needs the cascade matrix's INVERSE: to turn a
 * shadow-map pixel back into a world-space point on the near plane.
 *
 * The field evaluation is the same transcription as the G-buffer's, for
 * the same reason — WGSL has no include. A divergence here would make a
 * blob cast a shadow shaped differently from the blob, which is worse
 * than no shadow at all. The per-cascade uniform (2 mat4 + lightDir +
 * params + NDC bounds, 176 bytes) and the cluster AABB projection that
 * fills it are Rae (ShadowMapsSdf.rae).
 */

static const char* G3D_SM_SDF_WGSL =
"struct U {\n"
"  lightViewProj: mat4x4<f32>,\n"
"  invLightViewProj: mat4x4<f32>,\n"
"  lightDir: vec4<f32>,\n"
"  params: vec4<f32>,\n"   /* x = ball count, y = smoothing, z = march span */
"  bounds: vec4<f32>,\n"   /* xy = min NDC, zw = max NDC (#398) */
"};\n"
"@group(0) @binding(0) var<uniform> U0: U;\n"
"@group(0) @binding(1) var<storage, read> balls: array<vec4<f32>>;\n"
"fn smoothMin(a: f32, b: f32, k: f32) -> f32 {\n"
"  let h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);\n"
"  return mix(b, a, h) - k * h * (1.0 - h);\n"
"}\n"
"fn mapScene(p: vec3<f32>) -> f32 {\n"
"  var d = 10000.0;\n"
"  var i = 0u;\n"
"  let n = u32(U0.params.x);\n"
"  loop {\n"
"    if (i >= n) { break; }\n"
"    let b = balls[i];\n"
"    d = smoothMin(d, length(p - b.xyz) - b.w, U0.params.y);\n"
"    i = i + 1u;\n"
"  }\n"
"  return d;\n"
"}\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) ndc: vec2<f32>,\n"
"};\n"
/* A QUAD OVER THE CLUSTER'S PROJECTED EXTENT, not a fullscreen triangle
 * (#398). A 2-metre blob covers a tiny fraction of a 2048 shadow map, and
 * the fullscreen version paid for a raymarch at every one of those four
 * million pixels only to discard almost all of them. Same image, because
 * every pixel outside these bounds missed anyway. */
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> VsOut {\n"
"  var corner = array<vec2<f32>, 6>(\n"
"    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),\n"
"    vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));\n"
"  let c = corner[vi];\n"
"  let p = mix(U0.bounds.xy, U0.bounds.zw, c);\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(p, 0.0, 1.0);\n"
"  o.ndc = p;\n"
"  return o;\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @builtin(frag_depth) f32 {\n"
/* The cascade is NOT reverse-Z (see the file header), so z=0 is the near
 * plane. Unproject this shadow-map pixel there to get the ray origin. */
"  let nearH = U0.invLightViewProj * vec4<f32>(in.ndc, 0.0, 1.0);\n"
"  let origin = nearH.xyz / nearH.w;\n"
"  let dir = normalize(U0.lightDir.xyz);\n"
"  var travel = 0.0;\n"
"  var hit = false;\n"
"  var step = 0u;\n"
"  let span = U0.params.z;\n"
"  loop {\n"
"    if (step >= 96u || travel > span) { break; }\n"
"    let d = mapScene(origin + dir * travel);\n"
"    if (d < 0.004) { hit = true; break; }\n"
"    travel = travel + max(d * 0.8, 0.005);\n"
"    step = step + 1u;\n"
"  }\n"
/* A miss must leave the cascade's cleared 1.0 in place, or the whole
 * shadow map would read as "occluded at the near plane" and black the
 * scene out. */
"  if (!hit) { discard; }\n"
"  let world = origin + dir * travel;\n"
"  let clip = U0.lightViewProj * vec4<f32>(world, 1.0);\n"
"  return clamp(clip.z / clip.w, 0.0, 1.0);\n"
"}\n";

/* The shader sources the Rae ShadowCache builds its depth-only pipelines from. */
const char* rae_sm_wgsl_static(void)  { return G3D_SHADOW_WGSL; }
const char* rae_sm_wgsl_skinned(void) { return G3D_SHADOW_SKIN_WGSL; }
const char* rae_sm_wgsl_sdf(void)     { return G3D_SM_SDF_WGSL; }
