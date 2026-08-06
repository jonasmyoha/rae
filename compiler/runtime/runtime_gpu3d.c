/* gpu3d — minimal 3D renderer natives (Assembly 2026 demo arc, M1).
 *
 * Included by rae_runtime.c into one translation unit, AFTER the gpu2d
 * modules: it reuses the SDL3 window + wgpu device (runtime_webgpu.c),
 * the surface + persistent offscreen target and present/screenshot
 * machinery (runtime_gpu2d_platform.c / runtime_gpu2d_frame.c).
 *
 * Frame model: gpu3d can own a standalone frame through begin/end, or submit
 * without presenting so gpu2d can append one load-preserving UI pass. The 3D
 * pass is SINGLE-SAMPLED (#333: MSAA dropped so depth is sampleable — WebGPU
 * has no depth resolve) and renders MRT directly into the same persistent
 * offscreen texture the 2D path uses (g_g2d_off_view) plus a prepass set:
 *   @location(0) color     -> g_g2d_off_view (present/screenshot unchanged)
 *   @location(1) normal    -> rgba16f world-space normals (real, not
 *                             depth-reconstructed — reconstruction is wrong
 *                             exactly at the edges where AO/GI matter)
 *   @location(2) velocity  -> rg16f UV-space motion vectors
 * Depth32Float is STORED and texture-bindable. These four targets are the
 * inputs SSAO (#337), TAA (#335) and GI gathering (#338+) sample; the SDF
 * metaball pass writes all of them too, so implicit and triangle geometry
 * are indistinguishable to any downstream technique.
 * Velocity is full per-object motion since #335: each draw carries its
 * PREVIOUS model matrix, so moving geometry reports its own motion and
 * not merely the camera's.
 * gpu2d's LoadOp_Load overlay path then screenshots and presents the
 * composed frame exactly once.
 *
 * Draw model: meshes are immutable vertex/index buffers (interleaved
 * pos3/nrm3/uv2 float32). Per-draw uniforms (model matrix + material)
 * live in one fixed-capacity storage buffer indexed by instance_index —
 * drawIndexed(.., firstInstance=drawIdx) — the same trick the 2D box
 * batcher relies on, avoiding dynamic-offset bind group layouts.
 * Uniform data accumulates CPU-side and uploads once at end().
 *
 * Lighting: single WGSL uber-shader — Cook-Torrance GGX + Smith +
 * Schlick Fresnel, one directional sun + hemisphere ambient, emissive,
 * exposure + ACES tonemap + gamma live in the tonemap pass (#334), not
 * in the material shaders.
 *
 * Ambient occlusion (#337) is a horizon search with a visibility bitmask,
 * generated at half resolution between the scene pass and TAA. It applies
 * to the INDIRECT term only, which is why the scene pass writes ambient to
 * its own attachment: direct light must not be occluded, and keeping the
 * split is also what stops AO double-darkening once GI lands.
 * `RAE_SSAO_QUALITY=mobile|low|desktop|high`, `RAE_SSAO_RADIUS=<world>`,
 * `RAE_NO_SSAO=1`, `RAE_SSAO_DEBUG=1` (view the raw AO buffer).
 *
 * Antialiasing is TAA (#335), not MSAA: the scene is jittered by a
 * Halton(2,3) sub-pixel offset each frame and resolved against a
 * reprojected history buffer. Frame order is
 *   scene -> taa -> tonemap -> (ui) -> present
 * with TAA operating on LINEAR HDR, before the tone curve, so
 * accumulation stays radiometrically correct — the same reason GI will
 * live in that slot. `RAE_NO_TAA=1` disables it for A/B comparison.
 */

#define G3D_MAX_MESHES 256
#define G3D_MAX_DRAWS  4096
#define G3D_DRAW_FLOATS 40  /* mat4 model + mat4 prevModel + baseColor+metallic + emissive+roughness */

static WGPUBuffer   g3d_mesh_vbuf[G3D_MAX_MESHES];
static WGPUBuffer   g3d_mesh_ibuf[G3D_MAX_MESHES];
static uint32_t     g3d_mesh_icount[G3D_MAX_MESHES];
static int          g3d_mesh_n = 0;

static WGPUTexture     g3d_hdr_tex = NULL;       /* rgba16f linear scene colour (#334) */
static WGPUTextureView g3d_hdr_view = NULL;
static WGPUTexture     g3d_depth_tex = NULL;
static WGPUTextureView g3d_depth_view = NULL;
static WGPUTexture     g3d_normal_tex = NULL;    /* rgba16f world normals */
static WGPUTextureView g3d_normal_view = NULL;
static WGPUTexture     g3d_velocity_tex = NULL;  /* rg16f UV-space motion */
static WGPUTextureView g3d_velocity_view = NULL;
static WGPUTexture     g3d_ambient_tex = NULL;   /* rgba16f indirect term (#337) */
static WGPUTextureView g3d_ambient_view = NULL;
static int             g3d_target_w = 0, g3d_target_h = 0;

/* Previous frame's viewProj for motion vectors. First frame reuses the
 * current matrix so velocity starts at zero instead of garbage. */
static float g3d_prev_viewproj[16];
static bool  g3d_have_prev = false;
/* Frame values the post passes reuse instead of recomputing (#337). */
static float g3d_frame_inv_viewproj[16];
static float g3d_frame_campos[3];

/* TAA (#335). Sub-pixel jitter walks a Halton(2,3) sequence: low
 * discrepancy, so N frames of history land on a well-spread set of
 * sample positions instead of clumping the way rand() would. */
static int   g3d_taa_frame = 0;
static bool  g3d_taa_enabled = true;
static float g3d_jitter_x = 0.0f, g3d_jitter_y = 0.0f;

static float g3d_halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= (float)base; r += f * (float)(index % base); index /= base; }
    return r;
}

static WGPURenderPipeline g3d_pipeline = NULL;
static WGPUBuffer   g3d_frame_ubuf = NULL;   /* frame uniforms (camera/sun/ambient) */
static WGPUBuffer   g3d_draw_sbuf = NULL;    /* per-draw storage array, fixed cap */
static WGPUBindGroup g3d_bind = NULL;
static float        g3d_draw_cpu[G3D_MAX_DRAWS * G3D_DRAW_FLOATS];
static int          g3d_draw_count = 0;
static int          g3d_draw_limit = G3D_MAX_DRAWS;
static bool         g3d_draw_overflow_reported = false;

static WGPUCommandEncoder    g3d_enc = NULL;
static WGPURenderPassEncoder g3d_pass = NULL;

/* Tonemap pass (#334): scene shaders output LINEAR HDR into g3d_hdr_view;
 * this fullscreen pass applies exposure + ACES + gamma and writes the LDR
 * result into gpu2d's offscreen. Accumulating light on tonemapped values
 * is wrong (light does not add after a nonlinear curve), so every future
 * indirect-lighting pass reads/writes the HDR target and tonemap stays
 * the LAST radiometric operation of the frame. */
static WGPURenderPipeline g3d_tonemap_pipeline = NULL;
static WGPUBindGroup      g3d_tonemap_bind[2] = {NULL, NULL};
static bool               g3d_tonemap_pending = false;

/* TAA resolve (#335). Two rgba16f history buffers ping-pong: this frame
 * reads hist[1-cur] and writes hist[cur], and tonemap then reads hist[cur].
 * Ping-ponging avoids a full-screen copy per frame and, more importantly,
 * avoids reading and writing one texture in the same pass.
 * History is PERSISTENT (cross-frame) — the lifetime class the render
 * graph was given in #331 precisely so temporal techniques could be
 * expressed honestly rather than bolted on. */
static WGPUTexture     g3d_taa_tex[2] = {NULL, NULL};
static WGPUTextureView g3d_taa_view[2] = {NULL, NULL};
static WGPURenderPipeline g3d_taa_pipeline = NULL;
static WGPUBindGroup   g3d_taa_bind[2] = {NULL, NULL};
static WGPUSampler     g3d_taa_sampler = NULL;
static int             g3d_taa_cur = 0;
static bool            g3d_taa_history_valid = false;
static bool            g3d_taa_pending = false;

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
"  let direct = (kd * albedo / PI + spec) * F.sunColor.rgb * NoL;\n"
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

#include "runtime_gpu3d_sdf.c"
#include "runtime_gpu3d_ssao.c"

static int g3d_invert_mat4(const float* m, float* out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
    return 1;
}

#ifndef __EMSCRIPTEN__
static void g3d_log_cb(WGPULogLevel level, WGPUStringView message, void* userdata) {
    (void)level; (void)userdata;
    fprintf(stderr, "[wgpu] %.*s\n", (int)message.length, message.data ? message.data : "");
}
#endif

static void g3d_configure_taa(void) {
    static bool done = false;
    if (done) return;
    done = true;
    /* Deterministic captures stay valid: TAA is a pure function of the
     * frame sequence, so a fixed frame count still converges identically.
     * The switch exists for A/B comparison and for isolating TAA when a
     * later temporal technique misbehaves. */
    if (getenv("RAE_NO_TAA")) g3d_taa_enabled = false;
}

static void g3d_configure_draw_limit(void) {
    g3d_draw_limit = G3D_MAX_DRAWS;
    const char* text = getenv("RAE_GPU3D_DRAW_LIMIT");
    if (!text || !text[0]) return;
    char* end = NULL;
    long requested = strtol(text, &end, 10);
    if (end != text && *end == '\0' && requested > 0 && requested <= G3D_MAX_DRAWS) {
        g3d_draw_limit = (int)requested;
        return;
    }
    fprintf(stderr,
            "[gpu3d] WARNING: ignoring invalid RAE_GPU3D_DRAW_LIMIT=%s; "
            "expected 1..%d\n",
            text, G3D_MAX_DRAWS);
}

static void g3d_init_pipeline(void) {
    if (g3d_pipeline) return;
    g3d_configure_taa();
    g3d_configure_draw_limit();
#ifndef __EMSCRIPTEN__
    if (getenv("RAE_GPU3D_DEBUG")) {
        wgpuSetLogCallback(g3d_log_cb, NULL);
        wgpuSetLogLevel(WGPULogLevel_Warn);
    }
#endif
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G3D_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUVertexAttribute attrs[3];
    memset(attrs, 0, sizeof(attrs));
    attrs[0].format = WGPUVertexFormat_Float32x3; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Float32x3; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
    attrs[2].format = WGPUVertexFormat_Float32x2; attrs[2].offset = 24; attrs[2].shaderLocation = 2;
    WGPUVertexBufferLayout vbl; memset(&vbl, 0, sizeof(vbl));
    vbl.arrayStride = 32; /* 8 float32 */
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.attributeCount = 3; vbl.attributes = attrs;

    WGPUColorTargetState cts[4]; memset(cts, 0, sizeof(cts));
    cts[0].format = WGPUTextureFormat_RGBA16Float;  cts[0].writeMask = WGPUColorWriteMask_All; /* direct+emissive HDR */
    cts[1].format = WGPUTextureFormat_RGBA16Float;  cts[1].writeMask = WGPUColorWriteMask_All; /* world normals */
    cts[2].format = WGPUTextureFormat_RG16Float;    cts[2].writeMask = WGPUColorWriteMask_All; /* motion vectors */
    cts[3].format = WGPUTextureFormat_RGBA16Float;  cts[3].writeMask = WGPUColorWriteMask_All; /* indirect/ambient */
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 4; fs.targets = cts;

    WGPUDepthStencilState ds; memset(&ds, 0, sizeof(ds));
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.depthCompare = WGPUCompareFunction_Less;
    ds.stencilFront.compare = WGPUCompareFunction_Always;
    ds.stencilFront.failOp = WGPUStencilOperation_Keep;
    ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    ds.stencilFront.passOp = WGPUStencilOperation_Keep;
    ds.stencilBack = ds.stencilFront;
    ds.stencilReadMask = 0xFFFFFFFFu; ds.stencilWriteMask = 0xFFFFFFFFu;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL; /* auto layout from shader bindings */
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_Back;
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_pipeline) { fprintf(stderr, "[gpu3d] render pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
    ud.size = 288; /* Frame: 3 mat4 (viewProj/inv/prev) + 6 vec4 (incl. jitter) */
    ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g3d_frame_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);

    WGPUBufferDescriptor sd; memset(&sd, 0, sizeof(sd));
    sd.size = (uint64_t)G3D_MAX_DRAWS * G3D_DRAW_FLOATS * sizeof(float);
    sd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g3d_draw_sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &sd);

    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_pipeline, 0);
    WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g3d_frame_ubuf; e[0].size = 288;
    e[1].binding = 1; e[1].buffer = g3d_draw_sbuf;  e[1].size = sd.size;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
    g3d_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

static void g3d_init_tonemap_pipeline(void) {
    if (g3d_tonemap_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G3D_TONEMAP_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;
    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_tonemap_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_tonemap_pipeline) fprintf(stderr, "[gpu3d] tonemap pipeline creation FAILED\n");
}

/* Bind group references the HDR view, so it must be rebuilt whenever
 * g3d_ensure_targets recreates the textures (it nulls this then). */
static WGPUBuffer g3d_taa_param_ubuf = NULL;

static void g3d_init_taa_pipeline(void) {
    if (g3d_taa_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G3D_TAA_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);
    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = WGPUTextureFormat_RGBA16Float; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;
    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_taa_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_taa_pipeline) { fprintf(stderr, "[gpu3d] TAA pipeline creation FAILED\n"); return; }
    WGPUSamplerDescriptor sd; memset(&sd, 0, sizeof(sd));
    sd.addressModeU = WGPUAddressMode_ClampToEdge;
    sd.addressModeV = WGPUAddressMode_ClampToEdge;
    sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter = WGPUFilterMode_Linear;   /* reprojection lands off-grid */
    sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sd.maxAnisotropy = 1;
    g3d_taa_sampler = wgpuDeviceCreateSampler(g_wgpu_dev, &sd);
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = 16; bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g3d_taa_param_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
}

/* Bind group t resolves INTO history[t], reading history[1-t]. */
static void g3d_ensure_taa_bind(void) {
    if (!g3d_taa_pipeline || !g3d_hdr_view || !g3d_velocity_view) return;
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_taa_pipeline, 0);
    for (int t = 0; t < 2; t++) {
        if (g3d_taa_bind[t] || !g3d_taa_view[t] || !g3d_taa_view[1 - t]) continue;
        WGPUBindGroupEntry e[5]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].textureView = g3d_hdr_view;
        e[1].binding = 1; e[1].textureView = g3d_taa_view[1 - t];
        e[2].binding = 2; e[2].textureView = g3d_velocity_view;
        e[3].binding = 3; e[3].sampler = g3d_taa_sampler;
        e[4].binding = 4; e[4].buffer = g3d_taa_param_ubuf; e[4].size = 16;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 5; bgd.entries = e;
        g3d_taa_bind[t] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    }
    wgpuBindGroupLayoutRelease(bgl);
}

/* Tonemap reads whichever image is the frame's final HDR: the TAA output
 * when TAA ran, the raw scene target when it did not. Slot 0 = raw HDR,
 * slot 1+t = TAA history buffer t. */
static void g3d_ensure_tonemap_bind(void) {
    if (!g3d_tonemap_pipeline || !g3d_hdr_view) return;
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_tonemap_pipeline, 0);
    for (int t = 0; t < 2; t++) {
        if (g3d_tonemap_bind[t]) continue;
        WGPUTextureView src = g3d_taa_enabled ? g3d_taa_view[t] : g3d_hdr_view;
        if (!src) continue;
        WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = g3d_frame_ubuf; e[0].size = 288;
        e[1].binding = 1; e[1].textureView = src;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
        g3d_tonemap_bind[t] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    }
    wgpuBindGroupLayoutRelease(bgl);
}

/* (Re)create the prepass targets (depth/normal/velocity) to match the
 * offscreen color target's size. Called from begin(); cheap when unchanged.
 * All three carry TextureBinding: their whole reason to exist is being
 * sampled by later passes (SSAO/TAA/GI); color goes directly to
 * g_g2d_off_view and needs nothing here. */
static void g3d_ensure_targets(void) {
    int w = g_g2d_off_w, h = g_g2d_off_h;
    if (w <= 0 || h <= 0) return;
    if (g3d_depth_view && w == g3d_target_w && h == g3d_target_h) return;
    for (int t = 0; t < 2; t++) {
        if (g3d_tonemap_bind[t]) { wgpuBindGroupRelease(g3d_tonemap_bind[t]); g3d_tonemap_bind[t] = NULL; }
        if (g3d_taa_bind[t]) { wgpuBindGroupRelease(g3d_taa_bind[t]); g3d_taa_bind[t] = NULL; }
        if (g3d_taa_view[t]) { wgpuTextureViewRelease(g3d_taa_view[t]); g3d_taa_view[t] = NULL; }
        if (g3d_taa_tex[t]) { wgpuTextureRelease(g3d_taa_tex[t]); g3d_taa_tex[t] = NULL; }
    }
    g3d_taa_history_valid = false;   /* a resize invalidates reprojection */
    if (g3d_hdr_view) { wgpuTextureViewRelease(g3d_hdr_view); g3d_hdr_view = NULL; }
    if (g3d_hdr_tex)  { wgpuTextureRelease(g3d_hdr_tex); g3d_hdr_tex = NULL; }
    if (g3d_depth_view) { wgpuTextureViewRelease(g3d_depth_view); g3d_depth_view = NULL; }
    if (g3d_depth_tex)  { wgpuTextureRelease(g3d_depth_tex); g3d_depth_tex = NULL; }
    if (g3d_normal_view) { wgpuTextureViewRelease(g3d_normal_view); g3d_normal_view = NULL; }
    if (g3d_normal_tex)  { wgpuTextureRelease(g3d_normal_tex); g3d_normal_tex = NULL; }
    if (g3d_velocity_view) { wgpuTextureViewRelease(g3d_velocity_view); g3d_velocity_view = NULL; }
    if (g3d_velocity_tex)  { wgpuTextureRelease(g3d_velocity_tex); g3d_velocity_tex = NULL; }
    if (g3d_ambient_view) { wgpuTextureViewRelease(g3d_ambient_view); g3d_ambient_view = NULL; }
    if (g3d_ambient_tex)  { wgpuTextureRelease(g3d_ambient_tex); g3d_ambient_tex = NULL; }
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h; td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = 1; td.sampleCount = 1;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    td.format = WGPUTextureFormat_RGBA16Float;   /* linear HDR scene colour */
    g3d_hdr_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_hdr_view = wgpuTextureCreateView(g3d_hdr_tex, NULL);
    td.format = WGPUTextureFormat_Depth32Float;
    g3d_depth_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_depth_view = wgpuTextureCreateView(g3d_depth_tex, NULL);
    td.format = WGPUTextureFormat_RGBA16Float;
    g3d_normal_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_normal_view = wgpuTextureCreateView(g3d_normal_tex, NULL);
    td.format = WGPUTextureFormat_RG16Float;
    g3d_velocity_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_velocity_view = wgpuTextureCreateView(g3d_velocity_tex, NULL);
    td.format = WGPUTextureFormat_RGBA16Float;
    g3d_ambient_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_ambient_view = wgpuTextureCreateView(g3d_ambient_tex, NULL);
    td.format = WGPUTextureFormat_RGBA16Float;   /* TAA history ping-pong */
    for (int t = 0; t < 2; t++) {
        g3d_taa_tex[t] = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
        g3d_taa_view[t] = wgpuTextureCreateView(g3d_taa_tex[t], NULL);
    }
    g3d_target_w = w; g3d_target_h = h;
}

/* Create an immutable mesh. verts = interleaved pos3/nrm3/uv2 as Rae
 * Floats (doubles), 8 per vertex; indices as Rae Ints. Converted to
 * float32 / uint32 on upload. Returns handle > 0, or 0 on failure. */
int64_t rae_ext_gpu3d_meshCreate(const float* verts, int64_t vertCount,
                                 const int64_t* indices, int64_t indexCount){
    if (!g_wgpu_dev || !verts || !indices) return 0;
    if (vertCount <= 0 || indexCount <= 0 || g3d_mesh_n >= G3D_MAX_MESHES) return 0;
    size_t vfloats = (size_t)vertCount * 8;
    /* Rae `Float` is f32, so `verts` is ALREADY the f32 layout the GPU wants:
     * upload straight from it. The temporary buffer that used to live here
     * existed only to narrow the old f64 default down to float. Indices still
     * need converting — Rae `Int` is i64 and the index buffer is u32. */
    uint32_t* ix = (uint32_t*)malloc((size_t)indexCount * sizeof(uint32_t));
    if (!ix) return 0;
    for (int64_t i = 0; i < indexCount; i++) ix[i] = (uint32_t)indices[i];

    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = vfloats * sizeof(float);
    bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    WGPUBuffer vb = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    wgpuQueueWriteBuffer(g_wgpu_queue, vb, 0, verts, bd.size);
    /* Index buffer sizes must be 4-byte multiples (uint32 already is). */
    bd.size = (uint64_t)indexCount * sizeof(uint32_t);
    bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
    WGPUBuffer ib = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    wgpuQueueWriteBuffer(g_wgpu_queue, ib, 0, ix, bd.size);
    free(ix);
    if (!vb || !ib) return 0;
    int slot = g3d_mesh_n++;
    g3d_mesh_vbuf[slot] = vb;
    g3d_mesh_ibuf[slot] = ib;
    g3d_mesh_icount[slot] = (uint32_t)indexCount;
    return (int64_t)(slot + 1);
}

/* Begin the 3D frame. `frame` packs the camera/light state as 36 Floats:
 *   [0..15] viewProj (column-major)
 *   [16..18] camPos            [19] time
 *   [20..22] sunDir            [23] exposure
 *   [24..26] sunColor (rgb * intensity)
 *   [27..29] ambient sky rgb
 *   [30..32] ambient ground rgb
 *   [33..35] clear color rgb
 */
void rae_ext_gpu3d_begin(const float* frame, int64_t count){
    if (!g_wgpu_dev || !g_g2d_off_view || !frame || count < 36) return;
    g3d_init_pipeline();
    g3d_ensure_targets();
    if (!g3d_hdr_view || !g3d_depth_view || !g3d_normal_view || !g3d_velocity_view || !g3d_ambient_view) return;
    /* Halton offsets are in [0,1); recentre to [-0.5,0.5) pixels, then
     * convert to NDC (2/size) since the shader adds them in clip space. */
    if (g3d_taa_enabled && g3d_target_w > 0 && g3d_target_h > 0) {
        const int hi = (g3d_taa_frame % 16) + 1;   /* skip index 0 => no zero-offset frame */
        g3d_jitter_x = (g3d_halton(hi, 2) - 0.5f) * 2.0f / (float)g3d_target_w;
        g3d_jitter_y = (g3d_halton(hi, 3) - 0.5f) * 2.0f / (float)g3d_target_h;
    } else {
        g3d_jitter_x = 0.0f; g3d_jitter_y = 0.0f;
    }
    g3d_draw_count = 0;
    g3d_sdf_group = 0;   /* metaball cluster slots are per-frame */
    g3d_tonemap_pending = true;
    g3d_ssao_pending = true;
    g3d_taa_pending = true;
    g3d_taa_cur = 1 - g3d_taa_cur;   /* resolve into the slot not read this frame */
    g3d_taa_frame++;

    float u[72]; memset(u, 0, sizeof(u));
    for (int i = 0; i < 16; i++) u[i] = (float)frame[i];       /* viewProj */
    u[16] = (float)frame[16]; u[17] = (float)frame[17]; u[18] = (float)frame[18]; u[19] = (float)frame[19];  /* camPos+time */
    u[20] = (float)frame[20]; u[21] = (float)frame[21]; u[22] = (float)frame[22]; u[23] = (float)frame[23];  /* sunDir+exposure */
    u[24] = (float)frame[24]; u[25] = (float)frame[25]; u[26] = (float)frame[26]; u[27] = 0.0f;              /* sunColor */
    u[28] = (float)frame[27]; u[29] = (float)frame[28]; u[30] = (float)frame[29]; u[31] = 0.0f;              /* ambSky */
    u[32] = (float)frame[30]; u[33] = (float)frame[31]; u[34] = (float)frame[32]; u[35] = 0.0f;              /* ambGround */
    g3d_invert_mat4(u, u + 36);
    memcpy(g3d_frame_inv_viewproj, u + 36, 16 * sizeof(float));
    g3d_frame_campos[0] = u[16]; g3d_frame_campos[1] = u[17]; g3d_frame_campos[2] = u[18];
    if (!g3d_have_prev) { memcpy(g3d_prev_viewproj, u, 16 * sizeof(float)); g3d_have_prev = true; }
    memcpy(u + 52, g3d_prev_viewproj, 16 * sizeof(float));   /* prevViewProj */
    memcpy(g3d_prev_viewproj, u, 16 * sizeof(float));        /* becomes prev next frame */
    u[68] = g3d_jitter_x; u[69] = g3d_jitter_y;              /* jitter (#335) */
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_frame_ubuf, 0, u, sizeof(u));
    if (getenv("RAE_GPU3D_DEBUG")) {
        static int logged2 = 0;
        if (!logged2) {
            fprintf(stderr, "[gpu3d] viewProj cols:\n");
            for (int c = 0; c < 4; c++)
                fprintf(stderr, "  [%.3f %.3f %.3f %.3f]\n", u[c*4+0], u[c*4+1], u[c*4+2], u[c*4+3]);
            fprintf(stderr, "[gpu3d] cam=(%.2f,%.2f,%.2f) sun=(%.2f,%.2f,%.2f) exp=%.2f\n",
                    u[16], u[17], u[18], u[20], u[21], u[22], u[23]);
            logged2 = 1;
        }
    }

    g3d_enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca[4]; memset(ca, 0, sizeof(ca));
    ca[0].view = g3d_hdr_view;     /* linear HDR; tonemap writes the offscreen (#334) */
    ca[0].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca[0].loadOp = WGPULoadOp_Clear;
    ca[0].storeOp = WGPUStoreOp_Store;
    ca[0].clearValue.r = frame[33]; ca[0].clearValue.g = frame[34]; ca[0].clearValue.b = frame[35]; ca[0].clearValue.a = 1.0;
    ca[1].view = g3d_normal_view;
    ca[1].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca[1].loadOp = WGPULoadOp_Clear;
    ca[1].storeOp = WGPUStoreOp_Store;   /* sampled by SSAO/GI */
    ca[2].view = g3d_velocity_view;
    ca[2].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca[2].loadOp = WGPULoadOp_Clear;     /* zero motion where nothing renders */
    ca[2].storeOp = WGPUStoreOp_Store;   /* sampled by TAA */
    ca[3].view = g3d_ambient_view;
    ca[3].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca[3].loadOp = WGPULoadOp_Clear;
    ca[3].storeOp = WGPUStoreOp_Store;   /* sampled by the SSAO composite */
    WGPURenderPassDepthStencilAttachment da; memset(&da, 0, sizeof(da));
    da.view = g3d_depth_view;
    da.depthLoadOp = WGPULoadOp_Clear;
    da.depthStoreOp = WGPUStoreOp_Store; /* the point of #333: depth is sampleable */
    da.depthClearValue = 1.0f;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 4;
    rp.colorAttachments = ca;
    rp.depthStencilAttachment = &da;
    g3d_pass = wgpuCommandEncoderBeginRenderPass(g3d_enc, &rp);
    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_bind, 0, NULL);
}

/* Queue one mesh draw. model = 16 Floats column-major. Uniform data is
 * written CPU-side (uploaded once at end); the draw is encoded now with
 * firstInstance = draw index so the shader picks its DrawU slot. */
void rae_ext_gpu3d_draw(int64_t mesh, const float* model, const float* prevModel,
                        float r, float g, float b,
                        float metallic, float roughness,
                        float emR, float emG, float emB){
    if (!g3d_pass || !model) return;
    int slot = (int)mesh - 1;
    if (slot < 0 || slot >= g3d_mesh_n) return;
    if (g3d_draw_count >= g3d_draw_limit) {
        if (!g3d_draw_overflow_reported) {
            fprintf(stderr,
                    "[gpu3d] ERROR: draw limit exceeded: configured=%d hardMaximum=%d; "
                    "discarding additional draws\n",
                    g3d_draw_limit, G3D_MAX_DRAWS);
            g3d_draw_overflow_reported = true;
        }
        return;
    }
    float* d = g3d_draw_cpu + g3d_draw_count * G3D_DRAW_FLOATS;
    for (int i = 0; i < 16; i++) d[i] = (float)model[i];
    /* No previous transform (first frame, or a caller that does not track
     * one) means "did not move": reusing the current model yields zero
     * velocity, which is right, where zeros would project to the origin
     * and smear a full-screen streak. */
    if (prevModel) { for (int i = 0; i < 16; i++) d[16 + i] = (float)prevModel[i]; }
    else           { for (int i = 0; i < 16; i++) d[16 + i] = (float)model[i]; }
    d[32] = (float)r; d[33] = (float)g; d[34] = (float)b; d[35] = (float)metallic;
    d[36] = (float)emR; d[37] = (float)emG; d[38] = (float)emB; d[39] = (float)roughness;
    wgpuRenderPassEncoderSetVertexBuffer(g3d_pass, 0, g3d_mesh_vbuf[slot], 0,
                                         wgpuBufferGetSize(g3d_mesh_vbuf[slot]));
    wgpuRenderPassEncoderSetIndexBuffer(g3d_pass, g3d_mesh_ibuf[slot],
                                        WGPUIndexFormat_Uint32, 0,
                                        wgpuBufferGetSize(g3d_mesh_ibuf[slot]));
    wgpuRenderPassEncoderDrawIndexed(g3d_pass, g3d_mesh_icount[slot], 1, 0, 0,
                                     (uint32_t)g3d_draw_count);
    g3d_draw_count++;
}

/* Finish and submit the 3D pass. The caller decides whether to present now or
 * append a load-preserving gpu2d/UI pass first. */
static int rae_g3d_finish_pass(void) {
    if (getenv("RAE_GPU3D_DEBUG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[gpu3d] end: pass=%p draws=%d meshes=%d target=%dx%d\n",
                    (void*)g3d_pass, g3d_draw_count, g3d_mesh_n, g3d_target_w, g3d_target_h);
            if (g3d_draw_count > 0) {
                float* d = g3d_draw_cpu;
                fprintf(stderr, "[gpu3d] draw0 model col3=(%.2f,%.2f,%.2f,%.2f) color=(%.2f,%.2f,%.2f) m=%.2f r=%.2f\n",
                        d[12], d[13], d[14], d[15], d[32], d[33], d[34], d[35], d[39]);
            }
            logged = 1;
        }
    }
    if (!g3d_pass) return 0;
    if (g3d_draw_count > 0) {
        wgpuQueueWriteBuffer(g_wgpu_queue, g3d_draw_sbuf, 0, g3d_draw_cpu,
                             (size_t)g3d_draw_count * G3D_DRAW_FLOATS * sizeof(float));
    }
    wgpuRenderPassEncoderEnd(g3d_pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(g3d_enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(g3d_pass); g3d_pass = NULL;
    wgpuCommandEncoderRelease(g3d_enc); g3d_enc = NULL;
    return 1;
}

void rae_ext_gpu3d_submit(void) {
    if (rae_g3d_finish_pass()) rae_wgpu_poll(0);
}

/* TAA resolve (#335). Ends the scene pass first so the HDR image and
 * velocity buffer are complete, then accumulates into history[cur].
 * Tonemap afterwards reads history[cur] rather than the raw HDR. */
void rae_ext_gpu3d_taa(void) {
    if (!g3d_taa_enabled || !g3d_taa_pending) return;
    rae_g3d_finish_pass();
    g3d_init_taa_pipeline();
    g3d_ensure_taa_bind();
    if (!g3d_taa_pipeline || !g3d_taa_bind[g3d_taa_cur]) return;
    g3d_taa_pending = false;

    /* 0.9 keeps ~10 frames of history: enough to converge the jitter
     * pattern, short enough that clamp failures wash out quickly. */
    float params[4] = { 0.9f, g3d_taa_history_valid ? 1.0f : 0.0f, 0.0f, 0.0f };
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_taa_param_ubuf, 0, params, sizeof(params));

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = g3d_taa_view[g3d_taa_cur];
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, g3d_taa_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g3d_taa_bind[g3d_taa_cur], 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
    g3d_taa_history_valid = true;
}

/* Tonemap pass (#334): resolve linear HDR into the LDR offscreen. Ends the
 * scene pass first if the caller has not, so the HDR image is complete.
 * Graph-driven frames dispatch this between the scene pass and whatever
 * composites in LDR (UI overlay, present). */
void rae_ext_gpu3d_tonemap(void) {
    rae_g3d_finish_pass();
    /* Same safety net as tonemap-in-end: a caller whose frame does not
     * dispatch TAA explicitly still gets it, so history keeps advancing
     * and tonemap never samples a stale slot. */
    if (g3d_ssao_enabled && g3d_ssao_pending) rae_ext_gpu3d_ssao();
    if (g3d_taa_enabled && g3d_taa_pending) rae_ext_gpu3d_taa();
    if (!g3d_tonemap_pending || !g3d_hdr_view || !g_g2d_off_view) return;
    g3d_init_tonemap_pipeline();
    g3d_ensure_tonemap_bind();
    const int tmSlot = g3d_taa_enabled ? g3d_taa_cur : 0;
    if (!g3d_tonemap_pipeline || !g3d_tonemap_bind[tmSlot]) return;
    g3d_tonemap_pending = false;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = g_g2d_off_view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;   /* fullscreen triangle overwrites every pixel */
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, g3d_tonemap_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g3d_tonemap_bind[tmSlot], 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
}

/* End the standalone 3D frame and reuse the 2D path's screenshot +
 * present-from-offscreen behavior. UI composition uses endPass instead. */
void rae_ext_gpu3d_end(void) {
    rae_g2d_tick_virtual_clock();
    rae_g3d_finish_pass();
    /* Presenting reads the LDR offscreen, which only tonemap writes. A
     * graph-driven frame has already dispatched it (no-op here); a legacy
     * begin/draw/end caller has not, so run it now rather than presenting
     * whatever the offscreen held last. */
    if (g3d_tonemap_pending) rae_ext_gpu3d_tonemap();

    if (g_sdl_headless_ms > 0 || g_sdl_headless_frames > 0) {
        const char* shot = getenv("RAE_GPU2D_SCREENSHOT");
        if (shot) rae_g2d_save_screenshot(shot);
    }

    /* Present best-effort: copy the resolved offscreen image into the
     * surface drawable (same policy as the 2D endFrame). */
    WGPUSurfaceTexture st; memset(&st, 0, sizeof(st));
    wgpuSurfaceGetCurrentTexture(g_g2d_surface, &st);
    if (st.texture &&
        (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
        WGPUCommandEncoder penc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
        WGPUTexelCopyTextureInfo cs; memset(&cs, 0, sizeof(cs)); cs.texture = g_g2d_off_tex; cs.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyTextureInfo cd; memset(&cd, 0, sizeof(cd)); cd.texture = st.texture; cd.aspect = WGPUTextureAspect_All;
        WGPUExtent3D ext; ext.width = (uint32_t)g_sdl_w; ext.height = (uint32_t)g_sdl_h; ext.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToTexture(penc, &cs, &cd, &ext);
        WGPUCommandBuffer pcb = wgpuCommandEncoderFinish(penc, NULL);
        wgpuQueueSubmit(g_wgpu_queue, 1, &pcb);
        wgpuCommandBufferRelease(pcb); wgpuCommandEncoderRelease(penc);
#ifndef __EMSCRIPTEN__
        wgpuSurfacePresent(g_g2d_surface);
#endif
        g_g2d_last_present_ok = 1;
    }
    if (st.texture) wgpuTextureRelease(st.texture);
    rae_wgpu_poll(0);
}

void rae_ext_gpu3d_shutdown(void) {
    g3d_sdf_shutdown();
    g3d_ssao_shutdown();
    for (int i = 0; i < g3d_mesh_n; i++) {
        if (g3d_mesh_vbuf[i]) { wgpuBufferRelease(g3d_mesh_vbuf[i]); g3d_mesh_vbuf[i] = NULL; }
        if (g3d_mesh_ibuf[i]) { wgpuBufferRelease(g3d_mesh_ibuf[i]); g3d_mesh_ibuf[i] = NULL; }
    }
    g3d_mesh_n = 0;
    if (g3d_bind) { wgpuBindGroupRelease(g3d_bind); g3d_bind = NULL; }
    if (g3d_draw_sbuf) { wgpuBufferRelease(g3d_draw_sbuf); g3d_draw_sbuf = NULL; }
    if (g3d_frame_ubuf) { wgpuBufferRelease(g3d_frame_ubuf); g3d_frame_ubuf = NULL; }
    if (g3d_pipeline) { wgpuRenderPipelineRelease(g3d_pipeline); g3d_pipeline = NULL; }
    for (int t = 0; t < 2; t++) if (g3d_tonemap_bind[t]) { wgpuBindGroupRelease(g3d_tonemap_bind[t]); g3d_tonemap_bind[t] = NULL; }
    if (g3d_tonemap_pipeline) { wgpuRenderPipelineRelease(g3d_tonemap_pipeline); g3d_tonemap_pipeline = NULL; }
    for (int t = 0; t < 2; t++) {
        if (g3d_taa_bind[t]) { wgpuBindGroupRelease(g3d_taa_bind[t]); g3d_taa_bind[t] = NULL; }
        if (g3d_taa_view[t]) { wgpuTextureViewRelease(g3d_taa_view[t]); g3d_taa_view[t] = NULL; }
        if (g3d_taa_tex[t]) { wgpuTextureRelease(g3d_taa_tex[t]); g3d_taa_tex[t] = NULL; }
    }
    if (g3d_taa_sampler) { wgpuSamplerRelease(g3d_taa_sampler); g3d_taa_sampler = NULL; }
    if (g3d_taa_param_ubuf) { wgpuBufferRelease(g3d_taa_param_ubuf); g3d_taa_param_ubuf = NULL; }
    if (g3d_taa_pipeline) { wgpuRenderPipelineRelease(g3d_taa_pipeline); g3d_taa_pipeline = NULL; }
    g3d_taa_history_valid = false; g3d_taa_pending = false; g3d_taa_cur = 0; g3d_taa_frame = 0;
    if (g3d_hdr_view) { wgpuTextureViewRelease(g3d_hdr_view); g3d_hdr_view = NULL; }
    if (g3d_hdr_tex)  { wgpuTextureRelease(g3d_hdr_tex); g3d_hdr_tex = NULL; }
    if (g3d_depth_view) { wgpuTextureViewRelease(g3d_depth_view); g3d_depth_view = NULL; }
    if (g3d_depth_tex)  { wgpuTextureRelease(g3d_depth_tex); g3d_depth_tex = NULL; }
    if (g3d_normal_view) { wgpuTextureViewRelease(g3d_normal_view); g3d_normal_view = NULL; }
    if (g3d_normal_tex)  { wgpuTextureRelease(g3d_normal_tex); g3d_normal_tex = NULL; }
    if (g3d_velocity_view) { wgpuTextureViewRelease(g3d_velocity_view); g3d_velocity_view = NULL; }
    if (g3d_velocity_tex)  { wgpuTextureRelease(g3d_velocity_tex); g3d_velocity_tex = NULL; }
    if (g3d_ambient_view) { wgpuTextureViewRelease(g3d_ambient_view); g3d_ambient_view = NULL; }
    if (g3d_ambient_tex)  { wgpuTextureRelease(g3d_ambient_tex); g3d_ambient_tex = NULL; }
    g3d_target_w = 0; g3d_target_h = 0;
    g3d_have_prev = false;
    g3d_tonemap_pending = false;
    g3d_draw_limit = G3D_MAX_DRAWS;
    g3d_draw_overflow_reported = false;
}
