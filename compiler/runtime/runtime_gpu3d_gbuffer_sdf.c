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

#define GB_SDF_MAX_GROUPS 8
#define GB_SDF_MAX_BALLS 64

static WGPURenderPipeline gb_sdf_pipeline = NULL;
static WGPUBuffer    gb_sdf_ball_sbuf[GB_SDF_MAX_GROUPS];
static WGPUBuffer    gb_sdf_color_sbuf[GB_SDF_MAX_GROUPS];
static WGPUBuffer    gb_sdf_param_ubuf[GB_SDF_MAX_GROUPS];
static WGPUBuffer    gb_sdf_frame_ubuf = NULL;
static WGPUBindGroup gb_sdf_bind[GB_SDF_MAX_GROUPS];

/* Own frame uniform: the raymarch needs the camera position and the
 * inverse view-projection to build a ray, neither of which the geometry
 * pass's uniform carries. */
#define GB_SDF_FRAME_BYTES 208   /* 3 mat4 + camPos vec4 */

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

static void gb_sdf_init(void) {
    if (gb_sdf_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(GB_SDF_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUColorTargetState cts[3]; memset(cts, 0, sizeof(cts));
    cts[0].format = WGPUTextureFormat_RGB10A2Unorm; cts[0].writeMask = WGPUColorWriteMask_All;
    cts[1].format = WGPUTextureFormat_RGBA8Unorm;   cts[1].writeMask = WGPUColorWriteMask_All;
    cts[2].format = WGPUTextureFormat_RGBA8Unorm;   cts[2].writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 3; fs.targets = cts;

    WGPUDepthStencilState ds; memset(&ds, 0, sizeof(ds));
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.depthCompare = WGPUCompareFunction_Greater;   /* reverse-Z, like the raster pass */
    ds.stencilFront.compare = WGPUCompareFunction_Always;
    ds.stencilFront.failOp = WGPUStencilOperation_Keep;
    ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    ds.stencilFront.passOp = WGPUStencilOperation_Keep;
    ds.stencilBack = ds.stencilFront;
    ds.stencilReadMask = 0xFFFFFFFFu; ds.stencilWriteMask = 0xFFFFFFFFu;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.vertex.bufferCount = 0;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;   /* fullscreen triangle */
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    gb_sdf_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!gb_sdf_pipeline) { fprintf(stderr, "[gbuffer] SDF pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor fd; memset(&fd, 0, sizeof(fd));
    fd.size = GB_SDF_FRAME_BYTES;
    fd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    gb_sdf_frame_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &fd);

    for (int i = 0; i < GB_SDF_MAX_GROUPS; i++) {
        WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
        bd.size = (uint64_t)GB_SDF_MAX_BALLS * 4 * sizeof(float);
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        gb_sdf_ball_sbuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        gb_sdf_color_sbuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
        ud.size = 64;
        ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        gb_sdf_param_ubuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
    }
}

/* One smooth-union cluster. Called between gbufferBegin and gbufferEnd,
 * so it lands in the same render pass as the triangles and depth-tests
 * against them. */
/* The metaball G-buffer draw runs in Rae now (lib/gbuffer.rae:drawMetaballs,
 * #504): Rae issues the draw into the open geometry pass. C keeps the dense
 * per-cluster prep — the inverse-viewProj/motion frame uniform, the ball/colour
 * storage uploads, the material params and the bind group — done here and
 * returning the cluster's group index (or -1 when it cannot draw). */
int64_t rae_gb_sdf_prepare(void* packedBalls_, int64_t count, void* packedColors_,
                           float smoothing, float camX, float camY, float camZ,
                           float metallic, float roughness,
                           float emR, float emG, float emB) {
    const float* packedBalls = (const float*)packedBalls_;
    const float* packedColors = (const float*)packedColors_;
    if (!gb_pass || !packedBalls || count < 1) return -1;
    if (count > GB_SDF_MAX_BALLS) count = GB_SDF_MAX_BALLS;
    gb_sdf_init();
    if (!gb_sdf_pipeline) return -1;
    if (gb_sdf_group >= GB_SDF_MAX_GROUPS) return -1;
    int gi = gb_sdf_group++;

    float fu[GB_SDF_FRAME_BYTES / 4];
    memset(fu, 0, sizeof(fu));
    memcpy(fu, gb_viewproj, 16 * sizeof(float));
    /* Invert the JITTERED matrix: the ray must match the pixel grid the
     * rest of the G-buffer was rasterised on (#397). */
    if (!g3d_invert_mat4(gb_viewproj_jittered, fu + 16)) {
        memset(fu + 16, 0, 16 * sizeof(float));
        fu[16] = 1.0f; fu[21] = 1.0f; fu[26] = 1.0f; fu[31] = 1.0f;
    }
    memcpy(fu + 32, gb_prev_viewproj, 16 * sizeof(float));
    fu[48] = camX; fu[49] = camY; fu[50] = camZ; fu[51] = 0.0f;
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_sdf_frame_ubuf, 0, fu, GB_SDF_FRAME_BYTES);

    wgpuQueueWriteBuffer(g_wgpu_queue, gb_sdf_ball_sbuf[gi], 0, packedBalls,
                         (size_t)count * 4 * sizeof(float));
    if (packedColors) {
        wgpuQueueWriteBuffer(g_wgpu_queue, gb_sdf_color_sbuf[gi], 0, packedColors,
                             (size_t)count * 4 * sizeof(float));
    }

    float pu[16]; memset(pu, 0, sizeof(pu));
    ((uint32_t*)pu)[0] = (uint32_t)count;
    pu[4] = 0.0f; pu[5] = 0.0f; pu[6] = 0.0f; pu[7] = metallic;
    pu[8] = emR; pu[9] = emG; pu[10] = emB; pu[11] = roughness;
    pu[12] = smoothing > 0.0001f ? smoothing : 0.0001f;
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_sdf_param_ubuf[gi], 0, pu, sizeof(pu));

    if (!gb_sdf_bind[gi]) {
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_sdf_pipeline, 0);
        WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = gb_sdf_frame_ubuf; e[0].size = GB_SDF_FRAME_BYTES;
        e[1].binding = 1; e[1].buffer = gb_sdf_ball_sbuf[gi];
        e[1].size = (uint64_t)GB_SDF_MAX_BALLS * 4 * sizeof(float);
        e[2].binding = 2; e[2].buffer = gb_sdf_param_ubuf[gi]; e[2].size = 64;
        e[3].binding = 3; e[3].buffer = gb_sdf_color_sbuf[gi];
        e[3].size = (uint64_t)GB_SDF_MAX_BALLS * 4 * sizeof(float);
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
        gb_sdf_bind[gi] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        wgpuBindGroupLayoutRelease(bgl);
    }
    if (!gb_sdf_bind[gi]) return -1;

    /* Same diagnostic line and same env gate as the forward path, so
     * 110's gate keeps asserting that metaballs actually reached the
     * renderer after the migration rather than losing that coverage. */
    if (getenv("RAE_GPU3D_SDF_TEST_LOG")) {
        static int gb_sdf_logged = 0;
        if (!gb_sdf_logged) {
            fprintf(stderr, "[gpu3d] SDF metaballs: count=%lld\n", (long long)count);
            gb_sdf_logged = 1;
        }
    }
    return gi;
}
void* rae_gb_sdf_pipeline(void)      { return (void*)gb_sdf_pipeline; }
void* rae_gb_sdf_bind(int64_t gi)    { return (gi >= 0 && gi < GB_SDF_MAX_GROUPS) ? (void*)gb_sdf_bind[(int)gi] : NULL; }

void rae_ext_gbuffer_sdfShutdown(void) {
    if (gb_sdf_pipeline) { wgpuRenderPipelineRelease(gb_sdf_pipeline); gb_sdf_pipeline = NULL; }
    if (gb_sdf_frame_ubuf) { wgpuBufferRelease(gb_sdf_frame_ubuf); gb_sdf_frame_ubuf = NULL; }
    for (int i = 0; i < GB_SDF_MAX_GROUPS; i++) {
        if (gb_sdf_ball_sbuf[i])  { wgpuBufferRelease(gb_sdf_ball_sbuf[i]); gb_sdf_ball_sbuf[i] = NULL; }
        if (gb_sdf_color_sbuf[i]) { wgpuBufferRelease(gb_sdf_color_sbuf[i]); gb_sdf_color_sbuf[i] = NULL; }
        if (gb_sdf_param_ubuf[i]) { wgpuBufferRelease(gb_sdf_param_ubuf[i]); gb_sdf_param_ubuf[i] = NULL; }
        if (gb_sdf_bind[i])       { wgpuBindGroupRelease(gb_sdf_bind[i]); gb_sdf_bind[i] = NULL; }
    }
    gb_sdf_group = 0;
}
