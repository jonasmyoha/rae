/* Metaball clusters into the G-buffer (#392).
 *
 * The deferred counterpart of runtime_gpu3d_sdf.c. That one raymarches
 * and SHADES in one fragment shader, writing lit colour into the forward
 * frame. This one raymarches and writes packed surface attributes, so a
 * metaball surface is shaded by the same lighting pass as every triangle
 * — which is the point of deferred, and the reason this file is small:
 * everything after the hit is the G-buffer encode, not a second BRDF.
 *
 * SEPARATE FILE because runtime_gpu3d_gbuffer.c is already near the
 * project's 1000-line limit, and the SDF field evaluation is a coherent
 * unit on its own.
 *
 * THE DISTANCE FIELD IS TRANSCRIBED, NOT SHARED. WGSL has no include, and
 * the forward version lives inside a different shader's string. The
 * functions below are a literal copy of mapScene/sceneAlbedo/sceneNormal
 * — if one changes, both must, and the smooth-union weight `h` that fuses
 * distances is the same weight that mixes colour in both. A divergence
 * here shows up as metaballs that fuse differently in the two frames,
 * which is exactly the kind of thing screenshots do not catch.
 *
 * REVERSE-Z. The deferred depth buffer clears to 0 and tests Greater
 * (#367), the opposite of the forward frame. The emitted frag_depth must
 * follow the frame it is writing into, not the shader it was copied from.
 */

/* #922: the pipeline, the frame uniform, the per-cluster ball / colour /
 * param buffers and the bind groups are Rae manager objects on the
 * GbufferCache (lib/GbufferSdf.rae). C keeps ONLY this WGSL source. */

static const char* GB_SDF_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  invViewProj: mat4x4<f32>,\n"
"  prevViewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"
"};\n"
"struct Params {\n"
"  info: vec4<u32>,\n"
"  baseColorMetallic: vec4<f32>,\n"
"  emissiveRoughness: vec4<f32>,\n"
"  blend: vec4<f32>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> balls: array<vec4<f32>>;\n"
"@group(0) @binding(2) var<uniform> P: Params;\n"
"@group(0) @binding(3) var<storage, read> ballColors: array<vec4<f32>>;\n"
GB_OCT_WGSL
"fn smoothMin(a: f32, b: f32, k: f32) -> f32 {\n"
"  let h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);\n"
"  return mix(b, a, h) - k * h * (1.0 - h);\n"
"}\n"
"fn mapScene(p: vec3<f32>) -> f32 {\n"
"  var d = 10000.0;\n"
"  var i = 0u;\n"
"  loop {\n"
"    if (i >= P.info.x) { break; }\n"
"    let b = balls[i];\n"
"    d = smoothMin(d, length(p - b.xyz) - b.w, P.blend.x);\n"
"    i = i + 1u;\n"
"  }\n"
"  return d;\n"
"}\n"
"fn sceneAlbedo(p: vec3<f32>) -> vec3<f32> {\n"
"  var d = 10000.0;\n"
"  var col = vec3<f32>(0.0);\n"
"  var i = 0u;\n"
"  loop {\n"
"    if (i >= P.info.x) { break; }\n"
"    let b = balls[i];\n"
"    let di = length(p - b.xyz) - b.w;\n"
"    let h = clamp(0.5 + 0.5 * (di - d) / P.blend.x, 0.0, 1.0);\n"
"    col = mix(ballColors[i].rgb, col, h);\n"
"    d = mix(di, d, h) - P.blend.x * h * (1.0 - h);\n"
"    i = i + 1u;\n"
"  }\n"
"  return col;\n"
"}\n"
"fn sceneNormal(p: vec3<f32>) -> vec3<f32> {\n"
"  let e = 0.003;\n"
"  return normalize(vec3<f32>(\n"
"    mapScene(p + vec3<f32>(e, 0.0, 0.0)) - mapScene(p - vec3<f32>(e, 0.0, 0.0)),\n"
"    mapScene(p + vec3<f32>(0.0, e, 0.0)) - mapScene(p - vec3<f32>(0.0, e, 0.0)),\n"
"    mapScene(p + vec3<f32>(0.0, 0.0, e)) - mapScene(p - vec3<f32>(0.0, 0.0, e))));\n"
"}\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) ndc: vec2<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> VsOut {\n"
"  var points = array<vec2<f32>, 3>(\n"
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(points[vi], 0.0, 1.0);\n"
"  o.ndc = points[vi];\n"
"  return o;\n"
"}\n"
"struct FsOut {\n"
"  @location(0) gba: vec4<f32>,\n"
"  @location(1) gbb: vec4<f32>,\n"
"  @location(2) gbc: vec4<f32>,\n"
"  @builtin(frag_depth) depth: f32,\n"
"};\n"
"@fragment\n"
"fn fs(in: VsOut) -> FsOut {\n"
/* The ray is built from the NEAR plane under reverse-Z: z=1 is near, not
 * far. Using 1.0 as "far" here — which is what the forward version does,
 * correctly, for its own convention — would aim every ray backwards. */
"  let nearH = F.invViewProj * vec4<f32>(in.ndc, 1.0, 1.0);\n"
"  let farH  = F.invViewProj * vec4<f32>(in.ndc, 0.0, 1.0);\n"
"  let rayOrigin = F.camPos.xyz;\n"
"  let rayDir = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);\n"
"  var travel = 0.05;\n"
"  var hit = false;\n"
"  var step = 0u;\n"
"  loop {\n"
"    if (step >= 112u || travel > 80.0) { break; }\n"
"    let d = mapScene(rayOrigin + rayDir * travel);\n"
"    if (d < max(0.0015, travel * 0.00035)) { hit = true; break; }\n"
"    travel = travel + max(d * 0.72, 0.003);\n"
"    step = step + 1u;\n"
"  }\n"
/* A miss must not write ANY attachment, depth included — discard is the
 * only way to leave the G-buffer's cleared background intact. */
"  if (!hit) { discard; }\n"
"  let worldPos = rayOrigin + rayDir * travel;\n"
"  let N = sceneNormal(worldPos);\n"
"  let oct = octEncode(N);\n"
"  let albedo = sceneAlbedo(worldPos);\n"
"  let rough = clamp(P.emissiveRoughness.a, 0.045, 1.0);\n"
"  let clip = F.viewProj * vec4<f32>(worldPos, 1.0);\n"
"  let clipPrev = F.prevViewProj * vec4<f32>(worldPos, 1.0);\n"
/* Camera-only reprojection: the field itself is re-evaluated each frame
 * with no notion of where a blob WAS, so a moving metaball reports the
 * motion of the camera and not its own. The forward path has the same
 * limitation; both should gain per-cluster motion together. */
"  let motion = (clip.xy / clip.w - clipPrev.xy / clipPrev.w) * vec2<f32>(0.5, -0.5);\n"
"  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(" GB_MOTION_ZERO_WGSL ");\n"
"  var emissive = 1.0;\n"
"  var mode = " GB_MODE_LIT_WGSL ";\n"
"  let emitPeak = max(P.emissiveRoughness.r, max(P.emissiveRoughness.g, P.emissiveRoughness.b));\n"
"  if (emitPeak > 0.0) {\n"
"    emissive = clamp(log(1.0 + emitPeak) / " GB_EMISSIVE_LOG_K_WGSL ", 0.0, 1.0);\n"
"    mode = " GB_MODE_EMISSIVE_WGSL ";\n"
"  }\n"
"  var o: FsOut;\n"
"  o.gba = vec4<f32>(oct.x, oct.y, 0.5, mode);\n"
"  o.gbb = vec4<f32>(albedo, rough);\n"
"  o.gbc = vec4<f32>(mEnc.x, mEnc.y, clamp(P.baseColorMetallic.a, 0.0, 1.0), emissive);\n"
"  o.depth = clamp(clip.z / clip.w, 0.0, 1.0);\n"
"  return o;\n"
"}\n";

const char* rae_gb_sdf_wgsl(void) { return GB_SDF_WGSL; }
