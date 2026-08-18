/* gpu3d shadows — cascaded shadow maps for the directional sun (#382).
 *
 * The GPU half of docs/shadow-system-design.md §3 Layer A. The cascade
 * FITTING is not here: it is pure arithmetic and lives in Rae
 * (lib/shadow3d.rae) where test 579 can check the anti-shimmer property
 * without a GPU. This file only rasterises depth into the cascades and
 * exposes them for sampling.
 *
 * WHY A SEPARATE PASS BEFORE THE SCENE PASS. The scene pass encodes draws
 * as they arrive, so by the time it is open the draw list is already
 * being consumed — there is no point at which "all draws are known but
 * nothing is encoded". Shadows therefore need their own explicit pass
 * with its own walk of the scene, which is also what the render graph
 * wants: shadowMap is written by one pass and read by another, and the
 * ordering falls out of that declaration rather than being asserted.
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

#define G3D_SHADOW_MAX_CASCADES 4
#define G3D_SHADOW_MAX_DRAWS 4096

static WGPUTexture     g3d_sm_tex = NULL;
static WGPUTextureView g3d_sm_array_view = NULL;                      /* sampled by the lighting pass */
static WGPUTextureView g3d_sm_layer_view[G3D_SHADOW_MAX_CASCADES];    /* render targets */
static WGPUSampler     g3d_sm_sampler = NULL;                         /* comparison sampler */
static int g3d_sm_res = 0;
static int g3d_sm_layers = 0;
/* Bumped whenever the texture is recreated, so bind groups built against
 * the old view can be invalidated rather than silently sampling a freed
 * texture. Same pattern as the G-buffer's gb_targets_gen. */
static int g3d_sm_gen = 0;

static WGPURenderPipeline g3d_sm_pipeline = NULL;        /* static vertex format */
static WGPURenderPipeline g3d_sm_pipeline_skin = NULL;   /* skinned vertex format */
static WGPUBuffer    g3d_sm_cascade_ubuf[G3D_SHADOW_MAX_CASCADES];
static WGPUBindGroup g3d_sm_bind[G3D_SHADOW_MAX_CASCADES];
static WGPUBindGroup g3d_sm_bind_skin[G3D_SHADOW_MAX_CASCADES];
static WGPUBuffer    g3d_sm_model_sbuf = NULL;

/* The shadow pass keeps its OWN draw list rather than sharing the scene
 * pass's: it runs first, so the scene list does not exist yet. */
static float g3d_sm_model_cpu[G3D_SHADOW_MAX_DRAWS * 16];
static int   g3d_sm_draw_mesh[G3D_SHADOW_MAX_DRAWS];
static int   g3d_sm_draw_skinned[G3D_SHADOW_MAX_DRAWS];
static int   g3d_sm_draw_count = 0;
static int   g3d_sm_cascade_count = 0;
static float g3d_sm_cascade_vp[G3D_SHADOW_MAX_CASCADES * 16];
static float g3d_sm_split_far[G3D_SHADOW_MAX_CASCADES];
static float g3d_sm_texel_world[G3D_SHADOW_MAX_CASCADES];
static float g3d_sm_depth_range[G3D_SHADOW_MAX_CASCADES];
static bool  g3d_sm_enabled = false;

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
"fn jointMat(j: u32) -> mat4x4<f32> {\n"
"  let r0 = palette[j * 3u + 0u];\n"
"  let r1 = palette[j * 3u + 1u];\n"
"  let r2 = palette[j * 3u + 2u];\n"
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
"  var skin = jointMat(j.x) * w.x;\n"
"  skin = skin + jointMat(j.y) * w.y;\n"
"  skin = skin + jointMat(j.z) * w.z;\n"
"  skin = skin + jointMat(j.w) * w.w;\n"
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
 * than no shadow at all.
 */
#define G3D_SM_SDF_MAX_CLUSTERS 8
#define G3D_SM_SDF_MAX_BALLS 64
#define G3D_SM_SDF_UBYTES 176   /* 2 mat4 + lightDir + params + ndc bounds (#398) */

static WGPURenderPipeline g3d_sm_sdf_pipeline = NULL;
static WGPUBuffer    g3d_sm_sdf_ball_sbuf[G3D_SM_SDF_MAX_CLUSTERS];
static WGPUBuffer    g3d_sm_sdf_ubuf[G3D_SHADOW_MAX_CASCADES * G3D_SM_SDF_MAX_CLUSTERS];
static WGPUBindGroup g3d_sm_sdf_bind[G3D_SHADOW_MAX_CASCADES * G3D_SM_SDF_MAX_CLUSTERS];
static float g3d_sm_sdf_balls_cpu[G3D_SM_SDF_MAX_CLUSTERS * G3D_SM_SDF_MAX_BALLS * 4];
static int   g3d_sm_sdf_count[G3D_SM_SDF_MAX_CLUSTERS];
static float g3d_sm_sdf_smoothing[G3D_SM_SDF_MAX_CLUSTERS];
static int   g3d_sm_sdf_clusters = 0;

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

static void g3d_sm_sdf_init(void) {
    if (g3d_sm_sdf_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G3D_SM_SDF_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

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

    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 0;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.vertex.bufferCount = 0;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_sm_sdf_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_sm_sdf_pipeline) { fprintf(stderr, "[shadow] SDF caster pipeline FAILED\n"); return; }

    for (int i = 0; i < G3D_SM_SDF_MAX_CLUSTERS; i++) {
        WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
        bd.size = (uint64_t)G3D_SM_SDF_MAX_BALLS * 4 * sizeof(float);
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        g3d_sm_sdf_ball_sbuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    }
    for (int i = 0; i < G3D_SHADOW_MAX_CASCADES * G3D_SM_SDF_MAX_CLUSTERS; i++) {
        WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
        ud.size = G3D_SM_SDF_UBYTES;
        ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        g3d_sm_sdf_ubuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
    }
}

/* Queue a metaball cluster as a caster. Called between shadowBegin and
 * shadowEnd, like the mesh casters. */
void rae_ext_gpu3d_shadowMetaballs(const float* packedBalls, int64_t count, float smoothing) {
    if (!g3d_sm_enabled || !packedBalls || count < 1) return;
    if (g3d_sm_sdf_clusters >= G3D_SM_SDF_MAX_CLUSTERS) return;
    if (count > G3D_SM_SDF_MAX_BALLS) count = G3D_SM_SDF_MAX_BALLS;
    g3d_sm_sdf_init();
    if (!g3d_sm_sdf_pipeline) return;
    int ci = g3d_sm_sdf_clusters++;
    memcpy(g3d_sm_sdf_balls_cpu + ci * G3D_SM_SDF_MAX_BALLS * 4, packedBalls,
           (size_t)count * 4 * sizeof(float));
    g3d_sm_sdf_count[ci] = (int)count;
    g3d_sm_sdf_smoothing[ci] = smoothing > 0.0001f ? smoothing : 0.0001f;
}

static void g3d_shadow_release_targets(void) {
    for (int i = 0; i < G3D_SHADOW_MAX_CASCADES; i++) {
        if (g3d_sm_layer_view[i]) { wgpuTextureViewRelease(g3d_sm_layer_view[i]); g3d_sm_layer_view[i] = NULL; }
        if (g3d_sm_bind[i]) { wgpuBindGroupRelease(g3d_sm_bind[i]); g3d_sm_bind[i] = NULL; }
        if (g3d_sm_bind_skin[i]) { wgpuBindGroupRelease(g3d_sm_bind_skin[i]); g3d_sm_bind_skin[i] = NULL; }
    }
    if (g3d_sm_array_view) { wgpuTextureViewRelease(g3d_sm_array_view); g3d_sm_array_view = NULL; }
    if (g3d_sm_tex) { wgpuTextureRelease(g3d_sm_tex); g3d_sm_tex = NULL; }
}

static void g3d_shadow_ensure_targets(int res, int layers) {
    if (g3d_sm_tex && g3d_sm_res == res && g3d_sm_layers == layers) return;
    g3d_shadow_release_targets();

    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)res;
    td.size.height = (uint32_t)res;
    td.size.depthOrArrayLayers = (uint32_t)layers;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.format = WGPUTextureFormat_Depth32Float;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    g3d_sm_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    if (!g3d_sm_tex) { fprintf(stderr, "[shadow] cascade texture creation FAILED\n"); return; }

    WGPUTextureViewDescriptor vd; memset(&vd, 0, sizeof(vd));
    vd.format = WGPUTextureFormat_Depth32Float;
    vd.dimension = WGPUTextureViewDimension_2DArray;
    vd.mipLevelCount = 1;
    vd.arrayLayerCount = (uint32_t)layers;
    vd.aspect = WGPUTextureAspect_DepthOnly;
    g3d_sm_array_view = wgpuTextureCreateView(g3d_sm_tex, &vd);

    for (int i = 0; i < layers; i++) {
        WGPUTextureViewDescriptor lv; memset(&lv, 0, sizeof(lv));
        lv.format = WGPUTextureFormat_Depth32Float;
        lv.dimension = WGPUTextureViewDimension_2D;
        lv.baseArrayLayer = (uint32_t)i;
        lv.arrayLayerCount = 1;
        lv.mipLevelCount = 1;
        lv.aspect = WGPUTextureAspect_DepthOnly;
        g3d_sm_layer_view[i] = wgpuTextureCreateView(g3d_sm_tex, &lv);
    }

    if (!g3d_sm_sampler) {
        /* A COMPARISON sampler: the hardware does the depth test and the
         * bilinear filter together, so one textureSampleCompare gives
         * 2x2 PCF for the price of one tap. That is the cheapest real
         * softening available and the reason M1 does not need a manual
         * kernel. */
        WGPUSamplerDescriptor sd; memset(&sd, 0, sizeof(sd));
        sd.addressModeU = WGPUAddressMode_ClampToEdge;
        sd.addressModeV = WGPUAddressMode_ClampToEdge;
        sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = WGPUFilterMode_Linear;
        sd.minFilter = WGPUFilterMode_Linear;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.lodMaxClamp = 1.0f;
        sd.compare = WGPUCompareFunction_Less;
        sd.maxAnisotropy = 1;
        g3d_sm_sampler = wgpuDeviceCreateSampler(g_wgpu_dev, &sd);
    }

    g3d_sm_res = res;
    g3d_sm_layers = layers;
    g3d_sm_gen++;
}

static WGPURenderPipeline g3d_shadow_make_pipeline(const char* wgsl, bool skinned) {
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(wgsl);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUVertexAttribute attrs[3]; memset(attrs, 0, sizeof(attrs));
    attrs[0].format = WGPUVertexFormat_Float32x3; attrs[0].offset = 0; attrs[0].shaderLocation = 0;
    int attrCount = 1;
    if (skinned) {
        attrs[1].format = WGPUVertexFormat_Float32x4; attrs[1].offset = 32; attrs[1].shaderLocation = 3;
        attrs[2].format = WGPUVertexFormat_Float32x4; attrs[2].offset = 48; attrs[2].shaderLocation = 4;
        attrCount = 3;
    }
    WGPUVertexBufferLayout vbl; memset(&vbl, 0, sizeof(vbl));
    vbl.arrayStride = skinned ? (20 * sizeof(float)) : (8 * sizeof(float));
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.attributeCount = (size_t)attrCount;
    vbl.attributes = attrs;

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
    /* A constant depth bias in the RASTERISER, not in the shader. It is
     * applied in depth units after projection, which is exactly where
     * acne originates, and it costs nothing. Slope-scaled is the part
     * that matters at grazing angles. */
    ds.depthBias = 2;
    ds.depthBiasSlopeScale = 2.5f;
    ds.depthBiasClamp = 0.0f;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    /* FRONT-face culling in the shadow pass. Rendering back faces puts
     * the stored depth on the far side of the occluder, which moves the
     * acne into the shadow interior where nothing samples it. Costs
     * nothing and removes most of the bias tuning. */
    pd.primitive.cullMode = WGPUCullMode_Front;
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = NULL;   /* depth only */
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!p) fprintf(stderr, "[shadow] %s pipeline creation FAILED\n", skinned ? "skinned" : "static");
    return p;
}

static void g3d_shadow_init(void) {
    if (g3d_sm_pipeline) return;
    g3d_sm_pipeline = g3d_shadow_make_pipeline(G3D_SHADOW_WGSL, false);
    g3d_sm_pipeline_skin = g3d_shadow_make_pipeline(G3D_SHADOW_SKIN_WGSL, true);
    if (!g3d_sm_pipeline) return;

    WGPUBufferDescriptor md; memset(&md, 0, sizeof(md));
    md.size = (uint64_t)G3D_SHADOW_MAX_DRAWS * 16 * sizeof(float);
    md.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g3d_sm_model_sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &md);

    for (int i = 0; i < G3D_SHADOW_MAX_CASCADES; i++) {
        WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
        ud.size = 64;   /* one mat4 */
        ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        g3d_sm_cascade_ubuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
    }

    /* The uniform the LIGHTING passes read. Created here, where the
     * shadow module owns it, rather than alongside the forward pipeline:
     * the deferred frame binds it too and never builds that pipeline, so
     * leaving it there made a deferred-only app abort inside
     * wgpuDeviceCreateBindGroup on a null buffer. */
    if (!g3d_sm_frame_ubuf) {
        WGPUBufferDescriptor sd; memset(&sd, 0, sizeof(sd));
        sd.size = 320;
        sd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        g3d_sm_frame_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &sd);
    }
}

static void g3d_shadow_ensure_binds(void) {
    if (!g3d_sm_pipeline || !g3d_sm_model_sbuf) return;
    for (int i = 0; i < g3d_sm_layers && i < G3D_SHADOW_MAX_CASCADES; i++) {
        if (!g3d_sm_bind[i]) {
            WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_sm_pipeline, 0);
            WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
            e[0].binding = 0; e[0].buffer = g3d_sm_cascade_ubuf[i]; e[0].size = 64;
            e[1].binding = 1; e[1].buffer = g3d_sm_model_sbuf;
            e[1].size = (uint64_t)G3D_SHADOW_MAX_DRAWS * 16 * sizeof(float);
            WGPUBindGroupDescriptor bd; memset(&bd, 0, sizeof(bd));
            bd.layout = bgl; bd.entryCount = 2; bd.entries = e;
            g3d_sm_bind[i] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bd);
            wgpuBindGroupLayoutRelease(bgl);
        }
        if (!g3d_sm_bind_skin[i] && g3d_sm_pipeline_skin && g3d_skin_palette_sbuf) {
            WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_sm_pipeline_skin, 0);
            WGPUBindGroupEntry e[3]; memset(e, 0, sizeof(e));
            e[0].binding = 0; e[0].buffer = g3d_sm_cascade_ubuf[i]; e[0].size = 64;
            e[1].binding = 1; e[1].buffer = g3d_sm_model_sbuf;
            e[1].size = (uint64_t)G3D_SHADOW_MAX_DRAWS * 16 * sizeof(float);
            e[2].binding = 2; e[2].buffer = g3d_skin_palette_sbuf;
            e[2].size = (uint64_t)G3D_SKIN_MAX_JOINTS * 12 * sizeof(float);
            WGPUBindGroupDescriptor bd; memset(&bd, 0, sizeof(bd));
            bd.layout = bgl; bd.entryCount = 3; bd.entries = e;
            g3d_sm_bind_skin[i] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bd);
            wgpuBindGroupLayoutRelease(bgl);
        }
    }
}

/* ----- Rae-facing API ------------------------------------------------ */

/* Start collecting casters. `cascades` is count*16 floats of light
 * view-projection (fitted in Rae by lib/shadow3d.rae), followed by
 * `count` split distances and `count` texel world sizes. */
void rae_ext_gpu3d_shadowBegin(const float* cascades, int64_t count,
                               int64_t resolution, const float* splits,
                               const float* texelWorld, const float* depthRange) {
    g3d_sm_enabled = false;
    g3d_sm_draw_count = 0;
    g3d_sm_sdf_clusters = 0;
    if (!g_wgpu_dev || !cascades || count < 1) return;
    if (count > G3D_SHADOW_MAX_CASCADES) count = G3D_SHADOW_MAX_CASCADES;
    if (resolution < 256) resolution = 256;
    if (resolution > 4096) resolution = 4096;

    /* The scene pipeline owns the uniform this pass fills and the bind
     * group that samples the result, and shadows run BEFORE it on the
     * first frame — so make sure it exists. Idempotent. */
    g3d_init_pipeline();
    g3d_shadow_init();
    if (!g3d_sm_pipeline) return;
    g3d_shadow_ensure_targets((int)resolution, (int)count);
    if (!g3d_sm_tex) return;

    g3d_sm_cascade_count = (int)count;
    memcpy(g3d_sm_cascade_vp, cascades, (size_t)count * 16 * sizeof(float));
    for (int i = 0; i < count; i++) {
        g3d_sm_split_far[i] = splits ? splits[i] : 0.0f;
        g3d_sm_texel_world[i] = texelWorld ? texelWorld[i] : 0.0f;
        g3d_sm_depth_range[i] = depthRange ? depthRange[i] : 1.0f;
        wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sm_cascade_ubuf[i], 0,
                             cascades + i * 16, 16 * sizeof(float));
    }

    /* Publish to the lighting pass. Unused cascade slots stay zeroed and
     * `cfg.x` carries the live count, so the shader needs no separate
     * enable flag and an app that never calls this simply gets 1.0. */
    if (g3d_sm_frame_ubuf) {
        float u[80]; memset(u, 0, sizeof(u));
        memcpy(u, cascades, (size_t)count * 16 * sizeof(float));
        for (int i = 0; i < count; i++) {
            u[64 + i] = g3d_sm_split_far[i];
            u[68 + i] = g3d_sm_texel_world[i];
            u[72 + i] = g3d_sm_depth_range[i];
        }
        u[76] = (float)count;
        /* Resolution rides along so the shader can turn a UV radius into
         * texels without a second uniform. */
        u[77] = (float)resolution;
        wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sm_frame_ubuf, 0, u, sizeof(u));
    }
    g3d_sm_enabled = true;
}

static void g3d_shadow_queue(int64_t mesh, rae_Mat4* model, int skinned) {
    if (!g3d_sm_enabled || !model) return;
    int slot = (int)mesh - 1;
    if (slot < 0) return;
    if (g3d_sm_draw_count >= G3D_SHADOW_MAX_DRAWS) return;
    float* d = g3d_sm_model_cpu + g3d_sm_draw_count * 16;
    for (int i = 0; i < 16; i++) d[i] = model->m.v[i];
    g3d_sm_draw_mesh[g3d_sm_draw_count] = slot;
    g3d_sm_draw_skinned[g3d_sm_draw_count] = skinned;
    g3d_sm_draw_count++;
}

void rae_ext_gpu3d_shadowDraw(int64_t mesh, rae_Mat4* model) {
    g3d_shadow_queue(mesh, model, 0);
}

void rae_ext_gpu3d_shadowDrawSkinned(int64_t mesh, rae_Mat4* model) {
    g3d_shadow_queue(mesh, model, 1);
}

/* Rasterise every queued caster into every cascade.
 *
 * One render pass per cascade, because each writes a different array
 * layer. They share the model buffer and the pipelines, so the cost is
 * the geometry, which is why §6 of the design doc wants far cascades on
 * a reduced update cadence later. */
/* The shadow cascade RENDER now runs in Rae (lib/gbuffer_shadow.rae, #504):
 * Rae drives the encoder + per-cascade depth passes + the caster draw loop over
 * the bindings. C keeps the pipelines/targets/binds/uniforms and the metaball
 * raymarch (dense projection math), exposed via these accessors. */
int64_t rae_sm_ready(void) { return (g3d_sm_enabled && g3d_sm_tex) ? 1 : 0; }
void rae_sm_upload_models(void) {
    if (g3d_sm_draw_count > 0)
        wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sm_model_sbuf, 0, g3d_sm_model_cpu,
                             (size_t)g3d_sm_draw_count * 16 * sizeof(float));
    g3d_shadow_ensure_binds();
}
int64_t rae_sm_cascade_count(void) { return (int64_t)g3d_sm_cascade_count; }
int64_t rae_sm_draw_count(void)    { return (int64_t)g3d_sm_draw_count; }
void* rae_sm_layer_view(int64_t c) { return (c >= 0 && c < G3D_SHADOW_MAX_CASCADES) ? (void*)g3d_sm_layer_view[(int)c] : NULL; }
int64_t rae_sm_caster_ready(int64_t i, int64_t c) {
    if (i < 0 || i >= g3d_sm_draw_count || c < 0 || c >= g3d_sm_cascade_count) return 0;
    int slot = g3d_sm_draw_mesh[i];
    if (slot < 0) return 0;
    if (g3d_sm_draw_skinned[i]) {
        if (slot >= g3d_skin_mesh_n || !g3d_sm_pipeline_skin || !g3d_sm_bind_skin[c]) return 0;
        return (g3d_skin_vbuf[slot] && g3d_skin_ibuf[slot] && g3d_skin_icount[slot]) ? 1 : 0;
    }
    if (slot >= g3d_mesh_n || !g3d_sm_bind[c]) return 0;
    return (g3d_mesh_vbuf[slot] && g3d_mesh_ibuf[slot] && g3d_mesh_icount[slot]) ? 1 : 0;
}
void* rae_sm_caster_pipeline(int64_t i) {
    if (i < 0 || i >= g3d_sm_draw_count) return NULL;
    return g3d_sm_draw_skinned[i] ? (void*)g3d_sm_pipeline_skin : (void*)g3d_sm_pipeline;
}
void* rae_sm_caster_bind(int64_t i, int64_t c) {
    if (i < 0 || i >= g3d_sm_draw_count || c < 0 || c >= G3D_SHADOW_MAX_CASCADES) return NULL;
    return g3d_sm_draw_skinned[i] ? (void*)g3d_sm_bind_skin[(int)c] : (void*)g3d_sm_bind[(int)c];
}
void* rae_sm_caster_vbuf(int64_t i) {
    if (i < 0 || i >= g3d_sm_draw_count) return NULL;
    int slot = g3d_sm_draw_mesh[i];
    return g3d_sm_draw_skinned[i] ? (void*)g3d_skin_vbuf[slot] : (void*)g3d_mesh_vbuf[slot];
}
void* rae_sm_caster_ibuf(int64_t i) {
    if (i < 0 || i >= g3d_sm_draw_count) return NULL;
    int slot = g3d_sm_draw_mesh[i];
    return g3d_sm_draw_skinned[i] ? (void*)g3d_skin_ibuf[slot] : (void*)g3d_mesh_ibuf[slot];
}
int64_t rae_sm_caster_icount(int64_t i) {
    if (i < 0 || i >= g3d_sm_draw_count) return 0;
    int slot = g3d_sm_draw_mesh[i];
    return g3d_sm_draw_skinned[i] ? (int64_t)g3d_skin_icount[slot] : (int64_t)g3d_mesh_icount[slot];
}

/* The metaball casters for one cascade — a per-cluster raymarch whose AABB
 * projection is dense CPU math, so it stays C. Drawn into the Rae-created pass. */
void rae_sm_draw_metaballs(int64_t c_arg, void* passptr) {
    int c = (int)c_arg;
    WGPURenderPassEncoder pass = (WGPURenderPassEncoder)passptr;
    if (!pass || !g3d_sm_sdf_pipeline) return;
        for (int ci = 0; ci < g3d_sm_sdf_clusters; ci++) {
            int ui = c * G3D_SM_SDF_MAX_CLUSTERS + ci;
            float u[G3D_SM_SDF_UBYTES / 4];
            memset(u, 0, sizeof(u));
            memcpy(u, g3d_sm_cascade_vp + c * 16, 16 * sizeof(float));
            if (!g3d_invert_mat4(g3d_sm_cascade_vp + c * 16, u + 16)) continue;
            /* Direction the light TRAVELS, recovered from the cascade
             * matrix rather than passed in again: the matrix is the only
             * definition of where this cascade looks from, so deriving it
             * cannot disagree with the fit. Row 2 of the light view is
             * -forward, and the ortho projection leaves it unrotated. */
            u[32] = 0.0f - g3d_sm_cascade_vp[c * 16 + 2];
            u[33] = 0.0f - g3d_sm_cascade_vp[c * 16 + 6];
            u[34] = 0.0f - g3d_sm_cascade_vp[c * 16 + 10];
            u[35] = 0.0f;
            u[36] = (float)g3d_sm_sdf_count[ci];
            u[37] = g3d_sm_sdf_smoothing[ci];
            /* March span: the cascade's whole depth range, so a blob
             * anywhere between the light and the far plane is found. */
            u[38] = g3d_sm_depth_range[c] > 0.0f ? g3d_sm_depth_range[c] : 100.0f;

            /* Project the cluster's world AABB into this cascade and keep
             * only that rectangle. The box is grown by the smoothing
             * distance because the smooth union BULGES beyond the union of
             * the spheres — clipping to the raw spheres would shave the
             * edge off every fused shadow. */
            const float* bp = g3d_sm_sdf_balls_cpu + ci * G3D_SM_SDF_MAX_BALLS * 4;
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            for (int b = 0; b < g3d_sm_sdf_count[ci]; b++) {
                float r = bp[b * 4 + 3] + g3d_sm_sdf_smoothing[ci];
                for (int a = 0; a < 3; a++) {
                    float v = bp[b * 4 + a];
                    if (v - r < lo[a]) lo[a] = v - r;
                    if (v + r > hi[a]) hi[a] = v + r;
                }
            }
            float nx0 = 1e30f, ny0 = 1e30f, nx1 = -1e30f, ny1 = -1e30f;
            const float* M = g3d_sm_cascade_vp + c * 16;
            for (int k = 0; k < 8; k++) {
                float wx = (k & 1) ? hi[0] : lo[0];
                float wy = (k & 2) ? hi[1] : lo[1];
                float wz = (k & 4) ? hi[2] : lo[2];
                float cx = M[0]*wx + M[4]*wy + M[8]*wz + M[12];
                float cy = M[1]*wx + M[5]*wy + M[9]*wz + M[13];
                float cw = M[3]*wx + M[7]*wy + M[11]*wz + M[15];
                if (cw <= 0.0f) cw = 1.0f;   /* ortho: w is 1 */
                float px = cx / cw, py = cy / cw;
                if (px < nx0) nx0 = px;
                if (px > nx1) nx1 = px;
                if (py < ny0) ny0 = py;
                if (py > ny1) ny1 = py;
            }
            /* Clip to the cascade and skip entirely when off it. */
            if (nx0 < -1.0f) nx0 = -1.0f;
            if (ny0 < -1.0f) ny0 = -1.0f;
            if (nx1 >  1.0f) nx1 =  1.0f;
            if (ny1 >  1.0f) ny1 =  1.0f;
            if (nx1 <= nx0 || ny1 <= ny0) continue;
            /* SKIP CASCADES WHERE THE BLOB IS SUB-TEXEL. In a far cascade a
             * small cluster covers less than a texel, so it can write
             * nothing a sample would ever read. */
            float wpx = (nx1 - nx0) * 0.5f * (float)g3d_sm_res;
            float hpx = (ny1 - ny0) * 0.5f * (float)g3d_sm_res;
            if (wpx < 2.0f || hpx < 2.0f) continue;
            u[40] = nx0; u[41] = ny0; u[42] = nx1; u[43] = ny1;
            wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sm_sdf_ubuf[ui], 0, u, G3D_SM_SDF_UBYTES);
            wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sm_sdf_ball_sbuf[ci], 0,
                                 g3d_sm_sdf_balls_cpu + ci * G3D_SM_SDF_MAX_BALLS * 4,
                                 (size_t)g3d_sm_sdf_count[ci] * 4 * sizeof(float));
            if (!g3d_sm_sdf_bind[ui]) {
                WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_sm_sdf_pipeline, 0);
                WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
                e[0].binding = 0; e[0].buffer = g3d_sm_sdf_ubuf[ui]; e[0].size = G3D_SM_SDF_UBYTES;
                e[1].binding = 1; e[1].buffer = g3d_sm_sdf_ball_sbuf[ci];
                e[1].size = (uint64_t)G3D_SM_SDF_MAX_BALLS * 4 * sizeof(float);
                WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
                bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
                g3d_sm_sdf_bind[ui] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
                wgpuBindGroupLayoutRelease(bgl);
            }
            if (!g3d_sm_sdf_bind[ui]) continue;
            wgpuRenderPassEncoderSetPipeline(pass, g3d_sm_sdf_pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, g3d_sm_sdf_bind[ui], 0, NULL);
            wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
        }
}

int64_t rae_ext_gpu3d_shadowDrawCount(void) { return (int64_t)g3d_sm_draw_count; }

void rae_ext_gpu3d_shadowShutdown(void) {
    g3d_shadow_release_targets();
    if (g3d_sm_sampler) { wgpuSamplerRelease(g3d_sm_sampler); g3d_sm_sampler = NULL; }
    if (g3d_sm_pipeline) { wgpuRenderPipelineRelease(g3d_sm_pipeline); g3d_sm_pipeline = NULL; }
    if (g3d_sm_pipeline_skin) { wgpuRenderPipelineRelease(g3d_sm_pipeline_skin); g3d_sm_pipeline_skin = NULL; }
    if (g3d_sm_model_sbuf) { wgpuBufferRelease(g3d_sm_model_sbuf); g3d_sm_model_sbuf = NULL; }
    for (int i = 0; i < G3D_SHADOW_MAX_CASCADES; i++) {
        if (g3d_sm_cascade_ubuf[i]) { wgpuBufferRelease(g3d_sm_cascade_ubuf[i]); g3d_sm_cascade_ubuf[i] = NULL; }
    }
    g3d_sm_res = 0; g3d_sm_layers = 0; g3d_sm_enabled = false;
}
