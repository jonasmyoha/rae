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
 * offscreen texture the 2D canvas owns (#920) plus a prepass set:
 *   @location(0) color     -> the canvas offscreen view (present/screenshot unchanged)
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

/* The shadow cascades are the renderer's Rae ShadowCache (#925). The forward
 * scene + skin binds sample them, so Rae hands the three handles (the 320-byte
 * shadow frame uniform, the cascade array view, the comparison sampler) to C
 * BORROWED — the ShadowCache owns them and pushes them again whenever its
 * targets are recreated; a bind built over the old ones is rebuilt (gen). */
static WGPUBuffer      g3d_shadow_in_ubuf = NULL;
static WGPUTextureView g3d_shadow_in_view = NULL;
static WGPUSampler     g3d_shadow_in_sampler = NULL;
static int             g3d_shadow_in_gen = 0;
static void g3d_skin_drop_bind(void);
void rae_g3d_set_shadow_inputs(void* frame_ubuf, void* array_view, void* sampler) {
    if ((WGPUBuffer)frame_ubuf == g3d_shadow_in_ubuf && (WGPUTextureView)array_view == g3d_shadow_in_view
        && (WGPUSampler)sampler == g3d_shadow_in_sampler) return;
    g3d_shadow_in_ubuf = (WGPUBuffer)frame_ubuf;
    g3d_shadow_in_view = (WGPUTextureView)array_view;
    g3d_shadow_in_sampler = (WGPUSampler)sampler;
    g3d_shadow_in_gen++;
    g3d_skin_drop_bind();
}

#define G3D_MAX_DRAWS  4096
#define G3D_DRAW_FLOATS 40  /* mat4 model + mat4 prevModel + baseColor+metallic + emissive+roughness */

/* #905: the mesh store (vertex/index buffers per mesh id) is the Rae
 * MeshStore owned by the renderer; C no longer holds meshes. */

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

/* TAA (#335). The sub-pixel jitter (a Halton(2,3) walk), the on/off switch,
 * the frame index and the draw limit come from Rae in the frame block
 * (lib/Gpu3dTemporal.rae, docs/runtime-c-audit.md row 11); these hold this
 * frame's values for the passes below. */
static int   g3d_taa_frame = 0;
static bool  g3d_taa_enabled = true;
static float g3d_jitter_x = 0.0f, g3d_jitter_y = 0.0f;

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

#include "runtime_gpu3d_shaders.c"
#include "runtime_gpu3d_sdf.c"
/* The background, drawn inside the same scene pass; shares its encoder,
 * its pipeline restore convention and the frame's camera. */
#include "runtime_gpu3d_sky.c"
#include "runtime_gpu3d_ssao.c"

#ifndef __EMSCRIPTEN__
static void g3d_log_cb(WGPULogLevel level, WGPUStringView message, void* userdata) {
    (void)level; (void)userdata;
    fprintf(stderr, "[wgpu] %.*s\n", (int)message.length, message.data ? message.data : "");
}
#endif

static void g3d_init_pipeline(void) {
    if (g3d_pipeline) return;
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

}

/* The scene bind over the borrowed shadow inputs (#925): built once they are
 * set, rebuilt when the ShadowCache hands over new ones (its targets were
 * recreated), absent — and the frame not ready — until then. */
static int g3d_bind_gen = -1;
static void g3d_ensure_bind(void) {
    if (!g3d_pipeline || !g3d_frame_ubuf || !g3d_draw_sbuf) return;
    if (g3d_bind && g3d_bind_gen == g3d_shadow_in_gen) return;
    if (g3d_bind) { wgpuBindGroupRelease(g3d_bind); g3d_bind = NULL; }
    if (!g3d_shadow_in_ubuf || !g3d_shadow_in_view || !g3d_shadow_in_sampler) return;
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_pipeline, 0);
    WGPUBindGroupEntry e[5]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g3d_frame_ubuf; e[0].size = 288;
    e[1].binding = 1; e[1].buffer = g3d_draw_sbuf;
    e[1].size = (uint64_t)G3D_MAX_DRAWS * G3D_DRAW_FLOATS * sizeof(float);
    e[2].binding = 2; e[2].buffer = g3d_shadow_in_ubuf; e[2].size = 320;
    e[3].binding = 3; e[3].textureView = g3d_shadow_in_view;
    e[4].binding = 4; e[4].sampler = g3d_shadow_in_sampler;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 5; bgd.entries = e;
    g3d_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
    g3d_bind_gen = g3d_shadow_in_gen;
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
 * sampled by later passes (SSAO/TAA/GI); color goes directly to the
 * canvas's offscreen target (surface-sized, #920) and needs nothing here. */
static void g3d_ensure_targets(void) {
    int w = g_sdl_w, h = g_sdl_h;
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

/* Begin the 3D frame. `frame` packs the camera/light state as 36 Floats:
 *   [0..15] viewProj (column-major)
 *   [16..18] camPos            [19] time
 *   [20..22] sunDir            [23] exposure
 *   [24..26] sunColor (rgb * intensity)
 *   [27..29] ambient sky rgb
 *   [30..32] ambient ground rgb
 *   [33..35] clear color rgb
 */
/* Frame prepare (#514): pack the per-frame uniform, advance the TAA
 * jitter/history, and upload it. The render pass itself is now built in Rae
 * (gpu3d.beginForward) over the WebGPU bindings; this returns 1 when the frame
 * is ready to encode, 0 if the device/targets are not up yet. */
int rae_g3d_frame_prepare(const float* frame, int64_t count){
    if (!g_wgpu_dev || !g_g2d_surface || !frame || count < 57) return 0;
    g3d_taa_enabled = frame[54] != 0.0f;
    g3d_taa_frame = (int)frame[55];
    g3d_draw_limit = (int)frame[56];
    if (g3d_draw_limit < 1 || g3d_draw_limit > G3D_MAX_DRAWS) g3d_draw_limit = G3D_MAX_DRAWS;
    g3d_init_pipeline();
    g3d_ensure_bind();
    if (!g3d_bind) return 0;   /* the ShadowCache has not handed over its inputs yet */
    g3d_ensure_targets();
    if (!g3d_hdr_view || !g3d_depth_view || !g3d_normal_view || !g3d_velocity_view || !g3d_ambient_view) return 0;
    /* The jitter arrives in pixels; the shader adds it in clip space, so
     * convert to NDC (2/size) for this render target. */
    if (g3d_taa_enabled && g3d_target_w > 0 && g3d_target_h > 0) {
        g3d_jitter_x = frame[52] * 2.0f / (float)g3d_target_w;
        g3d_jitter_y = frame[53] * 2.0f / (float)g3d_target_h;
    } else {
        g3d_jitter_x = 0.0f; g3d_jitter_y = 0.0f;
    }
    g3d_draw_count = 0;
    g3d_sdf_group = 0;   /* metaball cluster slots are per-frame */
    g3d_tonemap_pending = true;
    g3d_ssao_pending = true;
    g3d_taa_pending = true;
    g3d_taa_cur = 1 - g3d_taa_cur;   /* resolve into the slot not read this frame */

    float u[72]; memset(u, 0, sizeof(u));
    for (int i = 0; i < 16; i++) u[i] = (float)frame[i];       /* viewProj */
    u[16] = (float)frame[16]; u[17] = (float)frame[17]; u[18] = (float)frame[18]; u[19] = (float)frame[19];  /* camPos+time */
    u[20] = (float)frame[20]; u[21] = (float)frame[21]; u[22] = (float)frame[22]; u[23] = (float)frame[23];  /* sunDir+exposure */
    u[24] = (float)frame[24]; u[25] = (float)frame[25]; u[26] = (float)frame[26]; u[27] = 0.0f;              /* sunColor */
    u[28] = (float)frame[27]; u[29] = (float)frame[28]; u[30] = (float)frame[29]; u[31] = 0.0f;              /* ambSky */
    u[32] = (float)frame[30]; u[33] = (float)frame[31]; u[34] = (float)frame[32]; u[35] = 0.0f;              /* ambGround */
    for (int i = 0; i < 16; i++) u[36 + i] = frame[36 + i];  /* inverse viewProj (Rae) */
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

    return 1;
}

/* Handle accessors + frame handoff for the Rae-side forward pass (#514).
 * rae_g3d_* are platform-ABI accessors (ungated), mirroring rae_gb_* on the
 * deferred path; the gated rae_ext_Gpu3d_* set shrinks as passes move to Rae.
 * The color-target views and the geometry pipeline/bind group are constructed
 * in C (g3d_ensure_targets / g3d_init_pipeline); Rae reads them here to build
 * the render pass over the WebGPU bindings, then hands the encoder + pass back
 * with rae_g3d_set_frame so the still-C draw/end path encodes into them. */
void* rae_g3d_hdr_view(void)      { return (void*)g3d_hdr_view; }
void* rae_g3d_normal_view(void)   { return (void*)g3d_normal_view; }
void* rae_g3d_velocity_view(void) { return (void*)g3d_velocity_view; }
void* rae_g3d_ambient_view(void)  { return (void*)g3d_ambient_view; }
void* rae_g3d_depth_view(void)    { return (void*)g3d_depth_view; }
void* rae_g3d_pipeline(void)      { return (void*)g3d_pipeline; }
void* rae_g3d_bind(void)          { return (void*)g3d_bind; }
void  rae_g3d_set_frame(void* enc, void* pass){
    g3d_enc  = (WGPUCommandEncoder)enc;
    g3d_pass = (WGPURenderPassEncoder)pass;
}
void* rae_g3d_pass(void) { return (void*)g3d_pass; }
/* Scene-pass finish primitives for the Rae-side finishForward (#514), mirroring
 * the deferred rae_gb_* set. Rae ends the pass + finishes the command buffer
 * over the bindings; rae_g3d_submit_cmd is the one thin C call (Rae cannot yet
 * take the address of a single handle for wgpuQueueSubmit). */
void* rae_g3d_encoder(void) { return (void*)g3d_enc; }
int64_t rae_g3d_frame_active(void) { return g3d_pass ? 1 : 0; }
void rae_g3d_clear_frame(void) { g3d_pass = NULL; g3d_enc = NULL; }
void rae_g3d_submit_cmd(void* cmd){
    WGPUCommandBuffer cb = (WGPUCommandBuffer)cmd;
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
}

/* Queue one mesh draw. model = 16 Floats column-major. Uniform data is
 * written CPU-side (uploaded once at end); the draw is encoded now with
 * firstInstance = draw index so the shader picks its DrawU slot. */
/* Mirror of the Rae-side `Mat4` layout, for the gpu3d extern boundary.
 *
 * `lib/math3d.rae` declares `type Mat4 { m: Array(Float, cap: 16) }`, and the
 * compiler emits exactly these two declarations into the generated
 * translation unit. Repeating them here is not duplication for its own sake:
 * C compatibility across translation units (C11 6.2.7) requires the same tag
 * and member names/types, so declaring the SAME shape makes the runtime's
 * Mat4-taking externs (rae_g3d_push_draw_record, the skinned/shadow draws)
 * compatible with the prototype the
 * generated code calls through. A `const float*` parameter would have the
 * same ABI but an incompatible type, which is undefined behaviour rather
 * than merely untidy.
 *
 * These live in the runtime .c files, never in rae_runtime.h — generated code
 * includes that header and emits its own copy of these types, so putting them
 * there would be a redefinition.
 *
 * The static assert is the guard: if Mat4's size ever diverges from 16 floats
 * the build fails here instead of silently reading the wrong bytes.
 */
#ifndef RAE_GPU3D_MAT4_FFI
#define RAE_GPU3D_MAT4_FFI
typedef struct { float v[16]; } rae_Array_float_16;
typedef struct rae_Mat4 { rae_Array_float_16 m; } rae_Mat4;
_Static_assert(sizeof(rae_Mat4) == 16 * sizeof(float),
               "Rae Mat4 must stay 16 contiguous floats for the gpu3d extern boundary");
#endif

/* CPU bookkeeping half of a forward mesh draw (#514): validate the mesh + the
 * draw limit and pack the 40-float per-draw record into the shared draw buffer
 * (uploaded once by end()). Returns the firstInstance slot for the Rae side to
 * encode the instanced DrawIndexed, or -1 to skip. The record buffer + count
 * are shared with drawMetaballs/drawSkinned, so this stays in C until those
 * move too. Layout: [0..15] model, [16..31] prevModel, [32..35] baseColor+
 * metallic, [36..39] emissive+roughness. */
int rae_g3d_push_draw_record(int64_t mesh, rae_Mat4* model, rae_Mat4* prevModel,
                             float r, float g, float b, float metallic,
                             float emR, float emG, float emB, float roughness){
    if (!model) return -1;
    if (mesh < 1) return -1;   /* the Rae MeshStore validated the buffers (#905) */
    if (g3d_draw_count >= g3d_draw_limit) {
        if (!g3d_draw_overflow_reported) {
            fprintf(stderr,
                    "[gpu3d] ERROR: draw limit exceeded: configured=%d hardMaximum=%d; "
                    "discarding additional draws\n",
                    g3d_draw_limit, G3D_MAX_DRAWS);
            g3d_draw_overflow_reported = true;
        }
        return -1;
    }
    float* d = g3d_draw_cpu + g3d_draw_count * G3D_DRAW_FLOATS;
    for (int i = 0; i < 16; i++) d[i] = model->m.v[i];
    /* No previous transform (first frame, or a caller that does not track
     * one) means "did not move": reusing the current model yields zero
     * velocity, which is right, where zeros would project to the origin
     * and smear a full-screen streak. */
    if (prevModel) { for (int i = 0; i < 16; i++) d[16 + i] = prevModel->m.v[i]; }
    else           { for (int i = 0; i < 16; i++) d[16 + i] = model->m.v[i]; }
    d[32] = r; d[33] = g; d[34] = b; d[35] = metallic;
    d[36] = emR; d[37] = emG; d[38] = emB; d[39] = roughness;
    /* Upload this record's slice immediately (#514), the same per-draw pattern
     * the skinned and metaball paths use — so the scene-pass finish (now in
     * Rae) has nothing left to bulk-upload. */
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_draw_sbuf,
                         (uint64_t)g3d_draw_count * G3D_DRAW_FLOATS * sizeof(float),
                         d, G3D_DRAW_FLOATS * sizeof(float));
    return g3d_draw_count++;
}

/* Forward mesh-buffer handle accessors (ungated), keyed by the 1-based mesh
 * handle. Rae reads these to bind the vertex/index buffers for a draw. */
static int rae_g3d_finish_pass(void) {
    if (getenv("RAE_GPU3D_DEBUG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[gpu3d] end: pass=%p draws=%d target=%dx%d\n",
                    (void*)g3d_pass, g3d_draw_count, g3d_target_w, g3d_target_h);
            if (g3d_draw_count > 0) {
                float* d = g3d_draw_cpu;
                fprintf(stderr, "[gpu3d] draw0 model col3=(%.2f,%.2f,%.2f,%.2f) color=(%.2f,%.2f,%.2f) m=%.2f r=%.2f\n",
                        d[12], d[13], d[14], d[15], d[32], d[33], d[34], d[35], d[39]);
            }
            logged = 1;
        }
    }
    if (!g3d_pass) return 0;
    /* Records now upload per-draw (#514, rae_g3d_push_draw_record), so there is
     * no bulk upload here. */
    wgpuRenderPassEncoderEnd(g3d_pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(g3d_enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(g3d_pass); g3d_pass = NULL;
    wgpuCommandEncoderRelease(g3d_enc); g3d_enc = NULL;
    return 1;
}

/* submit moved to Rae (#514): gpu3d.submit() = finishForward() + webgpuPoll,
 * over the WebGPU bindings. finishForward mirrors this rae_g3d_finish_pass
 * (still used by the C post-process passes until they move too). */

/* TAA resolve (#335). Ends the scene pass first so the HDR image and
 * velocity buffer are complete, then accumulates into history[cur].
 * Tonemap afterwards reads history[cur] rather than the raw HDR. */
/* TAA bookkeeping (#514): the scene-pass finish moves to the Rae wrapper
 * (taaPass -> finishForward). This ensures the pipeline/bind, uploads the blend
 * params (history-validity aware), flips history_valid, and returns the current
 * history slot for the Rae side to encode the fullscreen resolve into
 * taa_view[cur]; -1 to skip (TAA disabled/not pending, or targets not up). */
int rae_g3d_taa_prepare(void) {
    if (!g3d_taa_enabled || !g3d_taa_pending) return -1;
    g3d_init_taa_pipeline();
    g3d_ensure_taa_bind();
    if (!g3d_taa_pipeline || !g3d_taa_bind[g3d_taa_cur]) return -1;
    g3d_taa_pending = false;
    /* 0.9 keeps ~10 frames of history: enough to converge the jitter
     * pattern, short enough that clamp failures wash out quickly. */
    float params[4] = { 0.9f, g3d_taa_history_valid ? 1.0f : 0.0f, 0.0f, 0.0f };
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_taa_param_ubuf, 0, params, sizeof(params));
    g3d_taa_history_valid = true;
    return g3d_taa_cur;
}
void* rae_g3d_taa_pipeline(void) { return (void*)g3d_taa_pipeline; }
void* rae_g3d_taa_bind(int64_t slot){
    if (slot < 0 || slot > 1) return (void*)0;
    return (void*)g3d_taa_bind[slot];
}
void* rae_g3d_taa_view(int64_t slot){
    if (slot < 0 || slot > 1) return (void*)0;
    return (void*)g3d_taa_view[slot];
}

/* Tonemap pass (#334): resolve linear HDR into the LDR offscreen. Ends the
 * scene pass first if the caller has not, so the HDR image is complete.
 * Graph-driven frames dispatch this between the scene pass and whatever
 * composites in LDR (UI overlay, present). */
/* Tonemap bookkeeping (#514): the scene-pass finish and the safety-net dispatch
 * of ssao/taa move to the Rae wrapper (tonemapPass -> finishForward); the render
 * graph always runs ssao/taa/tonemap in order, so the safety nets were only for
 * legacy non-graph callers, of which the forward path has none. This ensures the
 * tonemap pipeline/bind and returns the source slot (the TAA slot when TAA is on)
 * for the Rae side to encode the fullscreen resolve, or -1 to skip. */
int rae_g3d_tonemap_prepare(void) {
    if (!g3d_tonemap_pending || !g3d_hdr_view || !g_g2d_surface) return -1;
    g3d_init_tonemap_pipeline();
    g3d_ensure_tonemap_bind();
    const int tmSlot = g3d_taa_enabled ? g3d_taa_cur : 0;
    if (!g3d_tonemap_pipeline || !g3d_tonemap_bind[tmSlot]) return -1;
    g3d_tonemap_pending = false;
    return tmSlot;
}
void* rae_g3d_tonemap_pipeline(void) { return (void*)g3d_tonemap_pipeline; }
void* rae_g3d_tonemap_bind(int64_t slot){
    if (slot < 0 || slot > 1) return (void*)0;
    return (void*)g3d_tonemap_bind[slot];
}
int64_t rae_g3d_tonemap_pending(void) { return g3d_tonemap_pending ? 1 : 0; }

/* End the standalone 3D frame and reuse the 2D path's screenshot +
 * present-from-offscreen behavior. UI composition uses endPass instead. */
/* Screenshot + present the LDR offscreen.
 *
 * This is PLATFORM work, not frame-graph work: it copies whatever the
 * offscreen currently holds into the surface drawable. Both the forward
 * frame and the deferred frame (#356) end this way — they disagree about
 * how the offscreen got its pixels, not about how a composed image
 * reaches the window. Sharing it here is why the deferred present pass
 * does not have to call into renderer3d's frame logic to show anything.
 */
static void rae_g3d_present_offscreen(WGPUTexture tex, int width, int height) {
    if (!tex) { rae_wgpu_poll(0); return; }
    rae_wgpu_report_periodic();
    /* A frame's pixels exist by this point whether or not there is a surface to
     * show them on, so mark it here rather than at wgpuSurfacePresent -- headless
     * returns before ever presenting. */
    g_rae_presented_any = 1;
    if (g_sdl_headless_ms > 0 || g_sdl_headless_frames > 0) {
        const char* shot = getenv("RAE_GPU2D_SCREENSHOT");
        if (shot) rae_g2d_save_screenshot(shot, tex, width, height);
    }

    /* NO WINDOW MEANS NOTHING TO PRESENT TO. An offscreen-only frame (the
     * headless zero-alloc tests, and any future render-to-texture caller)
     * never creates a surface, and wgpuSurfaceGetCurrentTexture aborts the
     * process on a null one rather than reporting a status we could skip
     * on. The screenshot above has already captured the frame, which is the
     * whole point of running headless, so returning here loses nothing. */
    if (!g_g2d_surface) { rae_wgpu_poll(0); return; }
    if (!rae_g2d_window_visible()) {
        rae_present_note_skip("gpu3d", "window-not-visible");
        rae_wgpu_poll(0);
        return;
    }

    /* Present best-effort: copy the resolved offscreen image into the
     * surface drawable (same policy as the 2D endFrame). */
    WGPUSurfaceTexture st; memset(&st, 0, sizeof(st));
    wgpuSurfaceGetCurrentTexture(g_g2d_surface, &st);
    int presented = 0;
    if (st.texture &&
        (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
        WGPUCommandEncoder penc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
        WGPUTexelCopyTextureInfo cs; memset(&cs, 0, sizeof(cs)); cs.texture = tex; cs.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyTextureInfo cd; memset(&cd, 0, sizeof(cd)); cd.texture = st.texture; cd.aspect = WGPUTextureAspect_All;
        uint32_t cw = (uint32_t)(width < g_sdl_w ? width : g_sdl_w);
        uint32_t ch = (uint32_t)(height < g_sdl_h ? height : g_sdl_h);
        WGPUExtent3D ext; ext.width = cw; ext.height = ch; ext.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToTexture(penc, &cs, &cd, &ext);
        WGPUCommandBuffer pcb = wgpuCommandEncoderFinish(penc, NULL);
        wgpuQueueSubmit(g_wgpu_queue, 1, &pcb);
        wgpuCommandBufferRelease(pcb); wgpuCommandEncoderRelease(penc);
#ifndef __EMSCRIPTEN__
        wgpuSurfacePresent(g_g2d_surface);
#endif
        g_g2d_last_present_ok = 1;
        presented = 1;
        rae_present_note_ok();
    }
    /* No usable drawable — the black-screen case. Release the drawable first,
     * then apply status-specific recovery (occlusion does not reconfigure). */
    if (st.texture) wgpuTextureRelease(st.texture);
    if (!presented) rae_present_recover("gpu3d", st.status);
    /* Blocking poll on a presented frame retires the surface-present
     * submission's resources, which a non-blocking poll leaves queued until
     * they complete at the next vsync — a ~7 KB/frame RSS climb while busy-
     * rendering. Same fix and rationale as the 2D endFrame; Fifo already
     * paces to vsync so it costs no frame rate. Occluded/headless frames
     * (nothing presented) keep the cheap non-blocking poll. */
    if (presented) rae_wgpu_poll_frame_end(); else rae_wgpu_poll(0);
}

/* Present the tonemapped offscreen (#514): advance the virtual clock and copy
 * to the drawable. The scene-pass finish and the tonemap-if-pending fallback
 * move to the Rae end() wrapper; rae_g3d_present_offscreen is genuine platform
 * copy-to-drawable, so it stays C (same class as rae_ext_Gbuffer_present). */
void rae_g3d_present_frame(void* texture, int64_t width, int64_t height) {
    rae_g2d_tick_virtual_clock();
    rae_g3d_present_offscreen((WGPUTexture)texture, (int)width, (int)height);
}

void rae_ext_Gpu3d_shutdown(void) {
    g3d_sdf_shutdown();
    g3d_sky_shutdown();
    g3d_ssao_shutdown();
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
