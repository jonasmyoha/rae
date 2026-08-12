/* gpu3d SDF pass — depth-integrated smooth-union sphere raymarching.
 *
 * This is renderer policy and therefore a future Rae-migration candidate.
 * The C implementation currently owns only the WebGPU pipeline and packed ABI
 * needed to render Scene3d SdfPrimitive data beside raster meshes. The pass
 * writes fragment depth into gpu3d's shared single-sampled depth attachment,
 * so implicit and triangle geometry occlude each other normally — and since
 * #333 it also writes the normal + velocity prepass targets, so downstream
 * techniques (SSAO/TAA/GI) see SDF hits exactly like raster geometry.
 */

/* 64, matching GB_SDF_MAX_BALLS in the deferred path. It was 16, which
 * TRUNCATED SILENTLY: example 111's 40-ball cluster drew its first 16 and
 * dropped the rest, and because that scene's magenta balls are indices 30-39
 * the whole cluster rendered in one colour. Two renderers meant to be compared
 * side by side must not disagree about how much of the scene they draw. The
 * clamp below stays as the backstop; only the ceiling moved. */
#define G3D_MAX_SDF_BALLS 64
#define G3D_MAX_SDF_GROUPS 8

/* One buffer set PER GROUP per frame, not one shared set.
 * wgpuQueueWriteBuffer is applied when the command buffer is submitted, not
 * where it appears between encoded draws — so writing one shared buffer
 * once per cluster leaves every draw reading the LAST cluster's data and
 * only that blob renders (three times over). Each group therefore gets its
 * own buffers + bind group, selected by a per-frame slot counter. */
static WGPURenderPipeline g3d_sdf_pipeline = NULL;
static WGPUBuffer g3d_sdf_ball_sbuf[G3D_MAX_SDF_GROUPS];
static WGPUBuffer g3d_sdf_color_sbuf[G3D_MAX_SDF_GROUPS];   /* per-ball albedo */
static WGPUBuffer g3d_sdf_param_ubuf[G3D_MAX_SDF_GROUPS];
static WGPUBindGroup g3d_sdf_bind[G3D_MAX_SDF_GROUPS];
static int g3d_sdf_group = 0;          /* next free slot this frame */
static bool g3d_sdf_group_overflow = false;

static const char* G3D_SDF_WGSL =
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
"struct Params {\n"
"  info: vec4<u32>,\n"
"  baseColorMetallic: vec4<f32>,\n"   /* rgb unused (per-ball albedo); a = metallic */
"  emissiveRoughness: vec4<f32>,\n"
"  blend: vec4<f32>,\n"               /* x = smoothing k ("stickiness") */
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> balls: array<vec4<f32>>;\n"
"@group(0) @binding(2) var<uniform> P: Params;\n"
"@group(0) @binding(3) var<storage, read> ballColors: array<vec4<f32>>;\n"
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
"fn smoothMin(a: f32, b: f32, k: f32) -> f32 {\n"
"  let h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);\n"
"  return mix(b, a, h) - k * h * (1.0 - h);\n"
"}\n"
/* Distance only — used for marching and normals, where colour is dead
 * weight. k comes from the cluster now instead of being hardcoded: it is
 * the "stickiness", the distance over which two balls fuse. */
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
/* Albedo at a surface point: the SAME blend weight h that fuses the
 * distances also mixes the colours, so a ball's colour bleeds across
 * exactly the region where its surface merges. Evaluated once at the hit
 * point, not per march step. */
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
"const PI: f32 = 3.14159265;\n"
"fn dGGX(NoH: f32, rough: f32) -> f32 {\n"
"  let a = rough * rough; let a2 = a * a;\n"
"  let d = NoH * NoH * (a2 - 1.0) + 1.0;\n"
"  return a2 / (PI * d * d + 1e-5);\n"
"}\n"
"fn gSmith(NoV: f32, NoL: f32, rough: f32) -> f32 {\n"
"  let k = (rough + 1.0) * (rough + 1.0) / 8.0;\n"
"  return NoV / (NoV * (1.0 - k) + k) * NoL / (NoL * (1.0 - k) + k);\n"
"}\n"
"fn fresnel(VoH: f32, f0: vec3<f32>) -> vec3<f32> {\n"
"  return f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - VoH, 5.0);\n"
"}\n"
"struct FsOut {\n"
"  @location(0) color: vec4<f32>,\n"
"  @location(1) normal: vec4<f32>,\n"
"  @location(2) velocity: vec2<f32>,\n"
"  @location(3) ambient: vec4<f32>,\n"
"  @builtin(frag_depth) depth: f32,\n"
"};\n"
"@fragment\n"
"fn fs(in: VsOut) -> FsOut {\n"
"  let farH = F.invViewProj * vec4<f32>(in.ndc, 1.0, 1.0);\n"
"  let rayOrigin = F.camPos.xyz;\n"
"  let rayDir = normalize(farH.xyz / farH.w - rayOrigin);\n"
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
"  if (!hit) { discard; }\n"
"  let worldPos = rayOrigin + rayDir * travel;\n"
"  let N = sceneNormal(worldPos);\n"
"  let V = normalize(rayOrigin - worldPos);\n"
"  let L = normalize(-F.sunDir.xyz);\n"
"  let H = normalize(V + L);\n"
"  let NoV = max(dot(N, V), 1e-4); let NoL = max(dot(N, L), 0.0);\n"
"  let NoH = max(dot(N, H), 0.0); let VoH = max(dot(V, H), 0.0);\n"
"  let albedo = sceneAlbedo(worldPos);\n"
"  let metallic = clamp(P.baseColorMetallic.a, 0.0, 1.0);\n"
"  let rough = clamp(P.emissiveRoughness.a, 0.045, 1.0);\n"
"  let f0 = mix(vec3<f32>(0.04), albedo, metallic);\n"
"  let Fs = fresnel(VoH, f0);\n"
"  let spec = dGGX(NoH, rough) * gSmith(NoV, NoL, rough) * Fs / max(4.0 * NoV * NoL, 1e-4);\n"
"  let kd = (vec3<f32>(1.0) - Fs) * (1.0 - metallic);\n"
"  let direct = (kd * albedo / PI + spec) * F.sunColor.rgb * NoL;\n"
"  let hemi = mix(F.ambGround.rgb, F.ambSky.rgb, N.z * 0.5 + 0.5);\n"
"  let ambient = hemi * albedo * (1.0 - metallic) + hemi * Fs * (1.0 - rough * 0.7);\n"
/* Indirect kept separate so SSAO can attenuate it alone (#337) — the SDF
 * surface must split the same way as raster, or AO would apply to one and
 * not the other. */
"  let c = direct + P.emissiveRoughness.rgb;\n"
"  let clip = F.viewProj * vec4<f32>(worldPos, 1.0);\n"
/* Prepass outputs (#333): the raymarch hit contributes normal + velocity
 * exactly like raster geometry, so AO/GI/TAA never special-case SDFs.
 * Velocity is camera-only reprojection until #336 (same as raster). */
"  let clipPrev = F.prevViewProj * vec4<f32>(worldPos, 1.0);\n"
"  var out: FsOut;\n"
"  out.color = vec4<f32>(c, 1.0);\n"
"  out.normal = vec4<f32>(N, 1.0);\n"
"  out.velocity = (clip.xy / clip.w - clipPrev.xy / clipPrev.w) * vec2<f32>(0.5, -0.5);\n"
"  out.ambient = vec4<f32>(ambient, 1.0);\n"
"  out.depth = clamp(clip.z / clip.w, 0.0, 1.0);\n"
"  return out;\n"
"}\n";

static void g3d_sdf_init_pipeline(void) {
    if (g3d_sdf_pipeline || !g3d_frame_ubuf) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G3D_SDF_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUColorTargetState cts[4]; memset(cts, 0, sizeof(cts));
    cts[0].format = WGPUTextureFormat_RGBA16Float;  cts[0].writeMask = WGPUColorWriteMask_All;
    cts[1].format = WGPUTextureFormat_RGBA16Float;  cts[1].writeMask = WGPUColorWriteMask_All;
    cts[2].format = WGPUTextureFormat_RG16Float;    cts[2].writeMask = WGPUColorWriteMask_All;
    cts[3].format = WGPUTextureFormat_RGBA16Float;  cts[3].writeMask = WGPUColorWriteMask_All;
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
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_sdf_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_sdf_pipeline) { fprintf(stderr, "[gpu3d] SDF pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_sdf_pipeline, 0);
    const uint64_t ballBytes = G3D_MAX_SDF_BALLS * 4u * sizeof(float);
    for (int g = 0; g < G3D_MAX_SDF_GROUPS; g++) {
        bd.size = ballBytes;
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        g3d_sdf_ball_sbuf[g] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        g3d_sdf_color_sbuf[g] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        bd.size = 64;
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        g3d_sdf_param_ubuf[g] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);

        WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = g3d_frame_ubuf; e[0].size = 272;
        e[1].binding = 1; e[1].buffer = g3d_sdf_ball_sbuf[g]; e[1].size = ballBytes;
        e[2].binding = 2; e[2].buffer = g3d_sdf_param_ubuf[g]; e[2].size = 64;
        e[3].binding = 3; e[3].buffer = g3d_sdf_color_sbuf[g]; e[3].size = ballBytes;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
        g3d_sdf_bind[g] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    }
    wgpuBindGroupLayoutRelease(bgl);
}

void rae_ext_gpu3d_drawMetaballs(const float* packedBalls, int64_t count,
                                 const float* packedColors, float smoothing,
                                 float metallic, float roughness,
                                 float emR, float emG, float emB){
    if (!g3d_pass || !packedBalls || !packedColors || count <= 0) return;
    g3d_sdf_init_pipeline();
    if (!g3d_sdf_pipeline) return;
    if (g3d_sdf_group >= G3D_MAX_SDF_GROUPS) {
        if (!g3d_sdf_group_overflow) {
            fprintf(stderr, "[gpu3d] ERROR: more than %d metaball clusters in one frame; "
                            "discarding the rest\n", G3D_MAX_SDF_GROUPS);
            g3d_sdf_group_overflow = true;
        }
        return;
    }
    const int slot = g3d_sdf_group++;
    if (!g3d_sdf_bind[slot]) return;
    if (count > G3D_MAX_SDF_BALLS) count = G3D_MAX_SDF_BALLS;
    if (getenv("RAE_GPU3D_SDF_TEST_LOG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[gpu3d] SDF metaballs: count=%lld\n", (long long)count);
            logged = 1;
        }
    }
    float balls[G3D_MAX_SDF_BALLS * 4];
    float colors[G3D_MAX_SDF_BALLS * 4];
    for (int64_t i = 0; i < count * 4; i++) balls[i] = (float)packedBalls[i];
    for (int64_t i = 0; i < count * 4; i++) colors[i] = (float)packedColors[i];
    struct {
        uint32_t info[4];
        float baseColorMetallic[4];
        float emissiveRoughness[4];
        float blend[4];
    } params;
    memset(&params, 0, sizeof(params));
    params.info[0] = (uint32_t)count;
    /* rgb unused: albedo is per-ball now. `a` still carries metallic. */
    params.baseColorMetallic[3] = (float)metallic;
    /* k must stay positive — smoothMin divides by it. */
    params.blend[0] = smoothing > 0.001f ? (float)smoothing : 0.001f;
    params.emissiveRoughness[0] = (float)emR;
    params.emissiveRoughness[1] = (float)emG;
    params.emissiveRoughness[2] = (float)emB;
    params.emissiveRoughness[3] = (float)roughness;
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sdf_ball_sbuf[slot], 0, balls,
                         (size_t)count * 4u * sizeof(float));
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sdf_color_sbuf[slot], 0, colors,
                         (size_t)count * 4u * sizeof(float));
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sdf_param_ubuf[slot], 0, &params, sizeof(params));
    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_sdf_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_sdf_bind[slot], 0, NULL);
    wgpuRenderPassEncoderDraw(g3d_pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_bind, 0, NULL);
}

static void g3d_sdf_shutdown(void) {
    for (int g = 0; g < G3D_MAX_SDF_GROUPS; g++) {
        if (g3d_sdf_bind[g]) { wgpuBindGroupRelease(g3d_sdf_bind[g]); g3d_sdf_bind[g] = NULL; }
        if (g3d_sdf_param_ubuf[g]) { wgpuBufferRelease(g3d_sdf_param_ubuf[g]); g3d_sdf_param_ubuf[g] = NULL; }
        if (g3d_sdf_color_sbuf[g]) { wgpuBufferRelease(g3d_sdf_color_sbuf[g]); g3d_sdf_color_sbuf[g] = NULL; }
        if (g3d_sdf_ball_sbuf[g]) { wgpuBufferRelease(g3d_sdf_ball_sbuf[g]); g3d_sdf_ball_sbuf[g] = NULL; }
    }
    g3d_sdf_group = 0;
    g3d_sdf_group_overflow = false;
    if (g3d_sdf_pipeline) { wgpuRenderPipelineRelease(g3d_sdf_pipeline); g3d_sdf_pipeline = NULL; }
}
