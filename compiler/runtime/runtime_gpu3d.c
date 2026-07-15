/* gpu3d — minimal 3D renderer natives (Assembly 2026 demo arc, M1).
 *
 * Included by rae_runtime.c into one translation unit, AFTER the gpu2d
 * modules: it reuses the SDL3 window + wgpu device (runtime_webgpu.c),
 * the surface + persistent offscreen target and present/screenshot
 * machinery (runtime_gpu2d_platform.c / runtime_gpu2d_frame.c).
 *
 * Frame model: gpu3d owns the whole frame (an app uses EITHER the 2D
 * beginFrame/endFrame OR gpu3d begin/end per frame). The 3D pass renders
 * into a 4x MSAA color target with a Depth32Float attachment and
 * RESOLVES into the same persistent offscreen texture the 2D path uses
 * (g_g2d_off_view), so present-to-surface and the RAE_GPU2D_SCREENSHOT
 * headless readback keep working unchanged. A later milestone draws the
 * 2D pass on top (LoadOp_Load) for HUD/text overlays.
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
 * exposure + ACES tonemap + gamma. Antialiasing = MSAA 4x.
 */

#define G3D_MAX_MESHES 256
#define G3D_MAX_DRAWS  4096
#define G3D_DRAW_FLOATS 24  /* mat4 model (16) + baseColor+metallic (4) + emissive+roughness (4) */

static WGPUBuffer   g3d_mesh_vbuf[G3D_MAX_MESHES];
static WGPUBuffer   g3d_mesh_ibuf[G3D_MAX_MESHES];
static uint32_t     g3d_mesh_icount[G3D_MAX_MESHES];
static int          g3d_mesh_n = 0;

static WGPUTexture     g3d_msaa_tex = NULL;
static WGPUTextureView g3d_msaa_view = NULL;
static WGPUTexture     g3d_depth_tex = NULL;
static WGPUTextureView g3d_depth_view = NULL;
static int             g3d_target_w = 0, g3d_target_h = 0;

static WGPURenderPipeline g3d_pipeline = NULL;
static WGPUBuffer   g3d_frame_ubuf = NULL;   /* frame uniforms (camera/sun/ambient) */
static WGPUBuffer   g3d_draw_sbuf = NULL;    /* per-draw storage array, fixed cap */
static WGPUBindGroup g3d_bind = NULL;
static float        g3d_draw_cpu[G3D_MAX_DRAWS * G3D_DRAW_FLOATS];
static int          g3d_draw_count = 0;

static WGPUCommandEncoder    g3d_enc = NULL;
static WGPURenderPassEncoder g3d_pass = NULL;

static const char* G3D_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"     /* xyz cam, w time */
"  sunDir: vec4<f32>,\n"     /* xyz dir (toward scene), w exposure */
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"};\n"
"struct DrawU {\n"
"  model: mat4x4<f32>,\n"
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
"  return o;\n"
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
"fn aces(x: vec3<f32>) -> vec3<f32> {\n"
"  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),\n"
"               vec3<f32>(0.0), vec3<f32>(1.0));\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @location(0) vec4<f32> {\n"
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
"  var c = direct + ambient + d.emissiveRoughness.rgb;\n"
"  c = aces(c * F.sunDir.w);\n"
"  c = pow(c, vec3<f32>(1.0 / 2.2));\n"
"  return vec4<f32>(c, 1.0);\n"
"}\n";

static void g3d_log_cb(WGPULogLevel level, WGPUStringView message, void* userdata) {
    (void)level; (void)userdata;
    fprintf(stderr, "[wgpu] %.*s\n", (int)message.length, message.data ? message.data : "");
}

static void g3d_init_pipeline(void) {
    if (g3d_pipeline) return;
    if (getenv("RAE_GPU3D_DEBUG")) {
        wgpuSetLogCallback(g3d_log_cb, NULL);
        wgpuSetLogLevel(WGPULogLevel_Warn);
    }
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

    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.writeMask = WGPUColorWriteMask_All; /* opaque, no blend */
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;

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
    pd.multisample.count = 4; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_pipeline) { fprintf(stderr, "[gpu3d] render pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
    ud.size = 160; /* Frame struct */
    ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g3d_frame_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);

    WGPUBufferDescriptor sd; memset(&sd, 0, sizeof(sd));
    sd.size = (uint64_t)G3D_MAX_DRAWS * G3D_DRAW_FLOATS * sizeof(float);
    sd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g3d_draw_sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &sd);

    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_pipeline, 0);
    WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g3d_frame_ubuf; e[0].size = 160;
    e[1].binding = 1; e[1].buffer = g3d_draw_sbuf;  e[1].size = sd.size;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
    g3d_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

/* (Re)create the MSAA color + depth targets to match the offscreen
 * resolve target's size. Called from begin(); cheap when unchanged. */
static void g3d_ensure_targets(void) {
    int w = g_g2d_off_w, h = g_g2d_off_h;
    if (w <= 0 || h <= 0) return;
    if (g3d_msaa_view && w == g3d_target_w && h == g3d_target_h) return;
    if (g3d_msaa_view) { wgpuTextureViewRelease(g3d_msaa_view); g3d_msaa_view = NULL; }
    if (g3d_msaa_tex)  { wgpuTextureRelease(g3d_msaa_tex); g3d_msaa_tex = NULL; }
    if (g3d_depth_view) { wgpuTextureViewRelease(g3d_depth_view); g3d_depth_view = NULL; }
    if (g3d_depth_tex)  { wgpuTextureRelease(g3d_depth_tex); g3d_depth_tex = NULL; }
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h; td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = 1; td.sampleCount = 4;
    td.format = g_g2d_fmt;
    td.usage = WGPUTextureUsage_RenderAttachment;
    g3d_msaa_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_msaa_view = wgpuTextureCreateView(g3d_msaa_tex, NULL);
    td.format = WGPUTextureFormat_Depth32Float;
    g3d_depth_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_depth_view = wgpuTextureCreateView(g3d_depth_tex, NULL);
    g3d_target_w = w; g3d_target_h = h;
}

/* Create an immutable mesh. verts = interleaved pos3/nrm3/uv2 as Rae
 * Floats (doubles), 8 per vertex; indices as Rae Ints. Converted to
 * float32 / uint32 on upload. Returns handle > 0, or 0 on failure. */
int64_t rae_ext_gpu3d_meshCreate(const double* verts, int64_t vertCount,
                                 const int64_t* indices, int64_t indexCount) {
    if (!g_wgpu_dev || !verts || !indices) return 0;
    if (vertCount <= 0 || indexCount <= 0 || g3d_mesh_n >= G3D_MAX_MESHES) return 0;
    size_t vfloats = (size_t)vertCount * 8;
    float* vf = (float*)malloc(vfloats * sizeof(float));
    uint32_t* ix = (uint32_t*)malloc((size_t)indexCount * sizeof(uint32_t));
    if (!vf || !ix) { free(vf); free(ix); return 0; }
    for (size_t i = 0; i < vfloats; i++) vf[i] = (float)verts[i];
    for (int64_t i = 0; i < indexCount; i++) ix[i] = (uint32_t)indices[i];

    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = vfloats * sizeof(float);
    bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    WGPUBuffer vb = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    wgpuQueueWriteBuffer(g_wgpu_queue, vb, 0, vf, bd.size);
    /* Index buffer sizes must be 4-byte multiples (uint32 already is). */
    bd.size = (uint64_t)indexCount * sizeof(uint32_t);
    bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
    WGPUBuffer ib = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    wgpuQueueWriteBuffer(g_wgpu_queue, ib, 0, ix, bd.size);
    free(vf); free(ix);
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
void rae_ext_gpu3d_begin(const double* frame, int64_t count) {
    if (!g_wgpu_dev || !g_g2d_off_view || !frame || count < 36) return;
    g3d_init_pipeline();
    g3d_ensure_targets();
    if (!g3d_msaa_view || !g3d_depth_view) return;
    g3d_draw_count = 0;

    float u[40];
    for (int i = 0; i < 16; i++) u[i] = (float)frame[i];       /* viewProj */
    u[16] = (float)frame[16]; u[17] = (float)frame[17]; u[18] = (float)frame[18]; u[19] = (float)frame[19];  /* camPos+time */
    u[20] = (float)frame[20]; u[21] = (float)frame[21]; u[22] = (float)frame[22]; u[23] = (float)frame[23];  /* sunDir+exposure */
    u[24] = (float)frame[24]; u[25] = (float)frame[25]; u[26] = (float)frame[26]; u[27] = 0.0f;              /* sunColor */
    u[28] = (float)frame[27]; u[29] = (float)frame[28]; u[30] = (float)frame[29]; u[31] = 0.0f;              /* ambSky */
    u[32] = (float)frame[30]; u[33] = (float)frame[31]; u[34] = (float)frame[32]; u[35] = 0.0f;              /* ambGround */
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_frame_ubuf, 0, u, 160);
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
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = g3d_msaa_view;
    ca.resolveTarget = g_g2d_off_view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Discard; /* only the resolve matters */
    ca.clearValue.r = frame[33]; ca.clearValue.g = frame[34]; ca.clearValue.b = frame[35]; ca.clearValue.a = 1.0;
    WGPURenderPassDepthStencilAttachment da; memset(&da, 0, sizeof(da));
    da.view = g3d_depth_view;
    da.depthLoadOp = WGPULoadOp_Clear;
    da.depthStoreOp = WGPUStoreOp_Discard;
    da.depthClearValue = 1.0f;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    rp.depthStencilAttachment = &da;
    g3d_pass = wgpuCommandEncoderBeginRenderPass(g3d_enc, &rp);
    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_bind, 0, NULL);
}

/* Queue one mesh draw. model = 16 Floats column-major. Uniform data is
 * written CPU-side (uploaded once at end); the draw is encoded now with
 * firstInstance = draw index so the shader picks its DrawU slot. */
void rae_ext_gpu3d_draw(int64_t mesh, const double* model,
                        double r, double g, double b,
                        double metallic, double roughness,
                        double emR, double emG, double emB) {
    if (!g3d_pass || !model) return;
    int slot = (int)mesh - 1;
    if (slot < 0 || slot >= g3d_mesh_n) return;
    if (g3d_draw_count >= G3D_MAX_DRAWS) return;
    float* d = g3d_draw_cpu + g3d_draw_count * G3D_DRAW_FLOATS;
    for (int i = 0; i < 16; i++) d[i] = (float)model[i];
    d[16] = (float)r; d[17] = (float)g; d[18] = (float)b; d[19] = (float)metallic;
    d[20] = (float)emR; d[21] = (float)emG; d[22] = (float)emB; d[23] = (float)roughness;
    wgpuRenderPassEncoderSetVertexBuffer(g3d_pass, 0, g3d_mesh_vbuf[slot], 0,
                                         wgpuBufferGetSize(g3d_mesh_vbuf[slot]));
    wgpuRenderPassEncoderSetIndexBuffer(g3d_pass, g3d_mesh_ibuf[slot],
                                        WGPUIndexFormat_Uint32, 0,
                                        wgpuBufferGetSize(g3d_mesh_ibuf[slot]));
    wgpuRenderPassEncoderDrawIndexed(g3d_pass, g3d_mesh_icount[slot], 1, 0, 0,
                                     (uint32_t)g3d_draw_count);
    g3d_draw_count++;
}

/* End the 3D frame: upload the accumulated per-draw uniforms (queue
 * writes execute before the submitted pass), submit, then reuse the 2D
 * path's screenshot + present-from-offscreen behavior. */
void rae_ext_gpu3d_end(void) {
    if (getenv("RAE_GPU3D_DEBUG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[gpu3d] end: pass=%p draws=%d meshes=%d target=%dx%d\n",
                    (void*)g3d_pass, g3d_draw_count, g3d_mesh_n, g3d_target_w, g3d_target_h);
            if (g3d_draw_count > 0) {
                float* d = g3d_draw_cpu;
                fprintf(stderr, "[gpu3d] draw0 model col3=(%.2f,%.2f,%.2f,%.2f) color=(%.2f,%.2f,%.2f) m=%.2f r=%.2f\n",
                        d[12], d[13], d[14], d[15], d[16], d[17], d[18], d[19], d[23]);
            }
            logged = 1;
        }
    }
    if (!g3d_pass) return;
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

    if (g_sdl_headless_ms > 0) {
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
        wgpuSurfacePresent(g_g2d_surface);
        g_g2d_last_present_ok = 1;
    }
    if (st.texture) wgpuTextureRelease(st.texture);
    if (g_wgpu_dev) wgpuDevicePoll(g_wgpu_dev, false, NULL);
}

void rae_ext_gpu3d_shutdown(void) {
    for (int i = 0; i < g3d_mesh_n; i++) {
        if (g3d_mesh_vbuf[i]) { wgpuBufferRelease(g3d_mesh_vbuf[i]); g3d_mesh_vbuf[i] = NULL; }
        if (g3d_mesh_ibuf[i]) { wgpuBufferRelease(g3d_mesh_ibuf[i]); g3d_mesh_ibuf[i] = NULL; }
    }
    g3d_mesh_n = 0;
    if (g3d_bind) { wgpuBindGroupRelease(g3d_bind); g3d_bind = NULL; }
    if (g3d_draw_sbuf) { wgpuBufferRelease(g3d_draw_sbuf); g3d_draw_sbuf = NULL; }
    if (g3d_frame_ubuf) { wgpuBufferRelease(g3d_frame_ubuf); g3d_frame_ubuf = NULL; }
    if (g3d_pipeline) { wgpuRenderPipelineRelease(g3d_pipeline); g3d_pipeline = NULL; }
    if (g3d_msaa_view) { wgpuTextureViewRelease(g3d_msaa_view); g3d_msaa_view = NULL; }
    if (g3d_msaa_tex)  { wgpuTextureRelease(g3d_msaa_tex); g3d_msaa_tex = NULL; }
    if (g3d_depth_view) { wgpuTextureViewRelease(g3d_depth_view); g3d_depth_view = NULL; }
    if (g3d_depth_tex)  { wgpuTextureRelease(g3d_depth_tex); g3d_depth_tex = NULL; }
    g3d_target_w = 0; g3d_target_h = 0;
}
