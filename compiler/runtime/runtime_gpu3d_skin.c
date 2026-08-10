/* gpu3d skinning — skinned mesh upload, joint palette, skinned draws.
 *
 * #374: the vertex format and pipeline that skeletal animation needs.
 *
 * WHY A SEPARATE PIPELINE rather than widening the existing one. The
 * static vertex layout is 32 bytes (pos3/nrm3/uv2) and is assumed by every
 * mesh generator in lib/mesh3d.rae and by the forward and G-buffer
 * shaders. Widening it would make a ground plane carry 32 bytes per vertex
 * of joint data it can never read, and would touch every 3D example.
 * Skinned geometry gets its own buffers, its own 64-byte layout and its
 * own pipeline; the two coexist inside one render pass by switching
 * pipeline between draws.
 *
 * THE PALETTE is one affine transform per joint:
 *
 *     palette[j] = jointWorldMatrix[j] * inverseBindMatrix[j]
 *
 * The inverse bind takes a vertex from model space into the joint's local
 * space; the joint's world matrix puts it back wherever that joint has
 * moved to. Composing them is what turns "where this vertex sits in the
 * bind pose" into "where it sits now".
 *
 * Stored as THREE vec4 rows per joint, not a mat4x4. A joint transform is
 * affine, so its fourth row is always (0,0,0,1) — uploading it is 25% of
 * the palette bandwidth spent on a constant. This is the shape lib's
 * Mat3x4 exists for (#354). Three vec4s rather than a WGSL mat3x4 because
 * matrix types in storage buffers carry alignment rules that differ across
 * backends, and rows sidestep the question entirely.
 *
 * SKINNING IS LINEAR BLEND SKINNING: each vertex is transformed by up to
 * four joints and the results are blended by weight. It collapses volume
 * at strongly twisted joints — the classic "candy wrapper" — which is a
 * known limit, not a bug, and the thing dual-quaternion skinning fixes if
 * the models ever need it.
 */

#define G3D_SKIN_MAX_MESHES 128
#define G3D_SKIN_MAX_DRAWS  256
#define G3D_SKIN_DRAW_FLOATS 24   /* mat4 model + vec4 albedo/metallic + vec4 rough + palette base */
#define G3D_SKIN_MAX_JOINTS 256
#define G3D_SKIN_VERT_FLOATS 20   /* pos3 nrm3 uv2 joints4 weights4 color4 */

static WGPUBuffer g3d_skin_vbuf[G3D_SKIN_MAX_MESHES];
static WGPUBuffer g3d_skin_ibuf[G3D_SKIN_MAX_MESHES];
static uint32_t   g3d_skin_icount[G3D_SKIN_MAX_MESHES];
static int        g3d_skin_mesh_n = 0;

static WGPURenderPipeline g3d_skin_pipeline = NULL;
static WGPUBuffer    g3d_skin_draw_sbuf = NULL;
static WGPUBuffer    g3d_skin_palette_sbuf = NULL;
static WGPUBindGroup g3d_skin_bind = NULL;
static float g3d_skin_draw_cpu[G3D_SKIN_MAX_DRAWS * G3D_SKIN_DRAW_FLOATS];
static int   g3d_skin_draw_count = 0;
/* 3 vec4 rows per joint. */
static float g3d_skin_palette_cpu[G3D_SKIN_MAX_JOINTS * 12];
static int   g3d_skin_joint_count = 0;
static bool  g3d_skin_palette_dirty = false;

static const char* G3D_SKIN_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"
"  sunDir: vec4<f32>,\n"
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"  invViewProj: mat4x4<f32>,\n"
"  prevViewProj: mat4x4<f32>,\n"
"  jitter: vec4<f32>,\n"
"};\n"
"struct DrawU {\n"
"  model: mat4x4<f32>,\n"
"  baseColorMetallic: vec4<f32>,\n"
"  emissiveRoughness: vec4<f32>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> draws: array<DrawU>;\n"
/* Three rows per joint; see the note on Mat3x4 above. */
"@group(0) @binding(2) var<storage, read> palette: array<vec4<f32>>;\n"
/* Shadow cascades (#382), same layout and bindings as the static pass.
 * A skinned surface and a static one under the same sun must agree about
 * shadowing as much as they agree about the BRDF. */
"struct ShadowU {\n"
"  lightViewProj: array<mat4x4<f32>, 4>,\n"
"  splitFar: vec4<f32>,\n"
"  texelWorld: vec4<f32>,\n"
"  depthRange: vec4<f32>,\n"
"  shadowCfg: vec4<f32>,\n"
"};\n"
"@group(0) @binding(3) var<uniform> SH: ShadowU;\n"
"@group(0) @binding(4) var shadowTex: texture_depth_2d_array;\n"
"@group(0) @binding(5) var shadowSamp: sampler_comparison;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) wpos: vec3<f32>,\n"
"  @location(1) nrm: vec3<f32>,\n"
"  @location(2) uv: vec2<f32>,\n"
"  @location(3) @interpolate(flat) inst: u32,\n"
"  @location(4) clipNow: vec4<f32>,\n"
"  @location(5) clipPrev: vec4<f32>,\n"
"  @location(6) vcol: vec3<f32>,\n"
"};\n"
/* Rebuild a joint's affine transform from its three stored rows. */
"fn jointMat(j: u32) -> mat4x4<f32> {\n"
"  let r0 = palette[j * 3u + 0u];\n"
"  let r1 = palette[j * 3u + 1u];\n"
"  let r2 = palette[j * 3u + 2u];\n"
/* Rows in, columns out: WGSL matrices are column-major, so the stored
 * rows become the matrix's columns transposed back here. Getting this
 * backwards produces a pose that is subtly sheared rather than obviously
 * broken, which is the hardest kind to spot. */
"  return mat4x4<f32>(\n"
"    vec4<f32>(r0.x, r1.x, r2.x, 0.0),\n"
"    vec4<f32>(r0.y, r1.y, r2.y, 0.0),\n"
"    vec4<f32>(r0.z, r1.z, r2.z, 0.0),\n"
"    vec4<f32>(r0.w, r1.w, r2.w, 1.0));\n"
"}\n"
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32,\n"
"      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>,\n"
"      @location(3) jf: vec4<f32>, @location(4) w: vec4<f32>,\n"
"      @location(5) vc: vec4<f32>) -> VsOut {\n"
"  let d = draws[ii];\n"
/* Joint indices travelled as floats through the vertex buffer; see the
 * note in lib/gltf.rae. */
"  let j = vec4<u32>(u32(jf.x), u32(jf.y), u32(jf.z), u32(jf.w));\n"
/* Linear blend skinning. Summing the weighted MATRICES and applying once
 * is equivalent to applying each and blending the results, and costs one
 * transform instead of four. */
"  var skin = jointMat(j.x) * w.x;\n"
"  skin = skin + jointMat(j.y) * w.y;\n"
"  skin = skin + jointMat(j.z) * w.z;\n"
"  skin = skin + jointMat(j.w) * w.w;\n"
"  let sp = skin * vec4<f32>(p, 1.0);\n"
"  let wp = d.model * vec4<f32>(sp.xyz, 1.0);\n"
"  var o: VsOut;\n"
"  o.pos = F.viewProj * wp;\n"
"  o.wpos = wp.xyz;\n"
/* The normal is skinned by the same blend with w=0, so translation drops
 * out and only rotation applies. Non-uniform joint scale would need the
 * inverse transpose, the same documented limit the static path has. */
"  let sn = skin * vec4<f32>(n, 0.0);\n"
"  o.nrm = normalize((d.model * vec4<f32>(sn.xyz, 0.0)).xyz);\n"
"  o.uv = uv;\n"
"  o.vcol = vc.rgb;\n"
"  o.inst = ii;\n"
"  o.clipNow = o.pos;\n"
/* No previous palette is kept yet, so a skinned vertex reports only its
 * OBJECT motion, not its limb motion. Velocity is therefore right for a
 * character sliding across the screen and wrong for a swinging arm; TAA
 * will smear the latter until a previous-frame palette exists. Stated
 * rather than left to be discovered. */
"  o.clipPrev = F.prevViewProj * (d.model * vec4<f32>(sp.xyz, 1.0));\n"
"  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);\n"
"  return o;\n"
"}\n"
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
/* Identical BRDF to the static forward pass on purpose: a skinned surface
 * and a static one under the same light must match, or the seam between a
 * character and the world it stands in becomes visible. */
"@fragment\n"
"fn fs(in: VsOut) -> FsOut {\n"
"  let d = draws[in.inst];\n"
"  let albedo = d.baseColorMetallic.rgb * in.vcol;\n"
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
"  let viewDepth = length(F.camPos.xyz - in.wpos);\n"
"  let sunVis = sunVisibility(in.wpos, N, viewDepth, in.pos.xy);\n"
"  let direct = (kd * albedo / PI + spec) * F.sunColor.rgb * NoL * sunVis;\n"
"  let hemi = mix(F.ambGround.rgb, F.ambSky.rgb, N.y * 0.5 + 0.5);\n"
"  let ambF = fresnel(NoV, f0);\n"
"  let ambient = hemi * albedo * (1.0 - metallic) + hemi * ambF * (1.0 - rough * 0.7);\n"
"  var o: FsOut;\n"
"  o.color = vec4<f32>(direct + d.emissiveRoughness.rgb, 1.0);\n"
"  o.ambient = vec4<f32>(ambient, 1.0);\n"
"  o.normal = vec4<f32>(N, 1.0);\n"
"  o.velocity = motionVec(in.clipNow, in.clipPrev);\n"
"  return o;\n"
"}\n";

static void g3d_skin_init_pipeline(void) {
    if (g3d_skin_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G3D_SKIN_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUVertexAttribute attrs[6]; memset(attrs, 0, sizeof(attrs));
    attrs[0].format = WGPUVertexFormat_Float32x3; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Float32x3; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
    attrs[2].format = WGPUVertexFormat_Float32x2; attrs[2].offset = 24; attrs[2].shaderLocation = 2;
    attrs[3].format = WGPUVertexFormat_Float32x4; attrs[3].offset = 32; attrs[3].shaderLocation = 3;
    attrs[4].format = WGPUVertexFormat_Float32x4; attrs[4].offset = 48; attrs[4].shaderLocation = 4;
    /* Per-vertex albedo tint (#378). A model with no colour data bakes
     * white here, which multiplies to a no-op, so the channel costs
     * correctness nothing when it is unused. */
    attrs[5].format = WGPUVertexFormat_Float32x4; attrs[5].offset = 64; attrs[5].shaderLocation = 5;
    WGPUVertexBufferLayout vbl; memset(&vbl, 0, sizeof(vbl));
    vbl.arrayStride = G3D_SKIN_VERT_FLOATS * sizeof(float);
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.attributeCount = 6; vbl.attributes = attrs;

    /* Same four targets and depth state as the static forward pass, so
     * both pipelines are valid inside the same render pass. */
    WGPUColorTargetState cts[4]; memset(cts, 0, sizeof(cts));
    cts[0].format = WGPUTextureFormat_RGBA16Float; cts[0].writeMask = WGPUColorWriteMask_All;
    cts[1].format = WGPUTextureFormat_RGBA16Float; cts[1].writeMask = WGPUColorWriteMask_All;
    cts[2].format = WGPUTextureFormat_RG16Float;   cts[2].writeMask = WGPUColorWriteMask_All;
    cts[3].format = WGPUTextureFormat_RGBA16Float; cts[3].writeMask = WGPUColorWriteMask_All;
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
    pd.layout = NULL;
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_Back;
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_skin_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_skin_pipeline) { fprintf(stderr, "[skin] pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor sd; memset(&sd, 0, sizeof(sd));
    sd.size = (uint64_t)G3D_SKIN_MAX_DRAWS * G3D_SKIN_DRAW_FLOATS * sizeof(float);
    sd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g3d_skin_draw_sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &sd);

    WGPUBufferDescriptor pl; memset(&pl, 0, sizeof(pl));
    pl.size = (uint64_t)G3D_SKIN_MAX_JOINTS * 12 * sizeof(float);
    pl.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g3d_skin_palette_sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &pl);

    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_skin_pipeline, 0);
    WGPUBindGroupEntry e[6]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g3d_frame_ubuf; e[0].size = 288;
    e[1].binding = 1; e[1].buffer = g3d_skin_draw_sbuf; e[1].size = sd.size;
    e[2].binding = 2; e[2].buffer = g3d_skin_palette_sbuf; e[2].size = pl.size;
    e[3].binding = 3; e[3].buffer = g3d_sm_frame_ubuf; e[3].size = 320;
    e[4].binding = 4; e[4].textureView = g3d_sm_array_view;
    e[5].binding = 5; e[5].sampler = g3d_sm_sampler;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 6; bgd.entries = e;
    g3d_skin_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

/* Upload a skinned mesh: 20 Floats per vertex, indices as Rae Ints. */
int64_t rae_ext_gpu3d_skinnedMeshCreate(const float* verts, int64_t vertCount,
                                        const int64_t* indices, int64_t indexCount) {
    if (!g_wgpu_dev || !verts || !indices) return 0;
    if (vertCount <= 0 || indexCount <= 0 || g3d_skin_mesh_n >= G3D_SKIN_MAX_MESHES) return 0;
    uint32_t* ix = (uint32_t*)malloc((size_t)indexCount * sizeof(uint32_t));
    if (!ix) return 0;
    for (int64_t i = 0; i < indexCount; i++) ix[i] = (uint32_t)indices[i];

    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = (uint64_t)vertCount * G3D_SKIN_VERT_FLOATS * sizeof(float);
    bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    WGPUBuffer vb = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    wgpuQueueWriteBuffer(g_wgpu_queue, vb, 0, verts, bd.size);
    bd.size = (uint64_t)indexCount * sizeof(uint32_t);
    bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
    WGPUBuffer ib = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    wgpuQueueWriteBuffer(g_wgpu_queue, ib, 0, ix, bd.size);
    free(ix);
    if (!vb || !ib) return 0;
    int slot = g3d_skin_mesh_n++;
    g3d_skin_vbuf[slot] = vb;
    g3d_skin_ibuf[slot] = ib;
    g3d_skin_icount[slot] = (uint32_t)indexCount;
    return (int64_t)(slot + 1);
}

/* Replace the joint palette. `rows` is 12 Floats per joint: three vec4
 * rows of the affine transform. */
void rae_ext_gpu3d_setPalette(const float* rows, int64_t jointCount) {
    if (!rows || jointCount <= 0) { g3d_skin_joint_count = 0; return; }
    if (jointCount > G3D_SKIN_MAX_JOINTS) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[skin] palette has %lld joints, max is %d; extra joints ignored\n",
                    (long long)jointCount, G3D_SKIN_MAX_JOINTS);
            warned = true;
        }
        jointCount = G3D_SKIN_MAX_JOINTS;
    }
    memcpy(g3d_skin_palette_cpu, rows, (size_t)jointCount * 12 * sizeof(float));
    g3d_skin_joint_count = (int)jointCount;
    g3d_skin_palette_dirty = true;
    /* Upload EAGERLY as well as lazily. The lazy path uploads on the
     * first skinned draw, which is inside the scene pass — but the shadow
     * pass (#382) runs BEFORE that and reads the same buffer, so a
     * lazy-only upload would shadow the character in last frame's pose,
     * or in no pose at all on frame one. Cheap: one write per frame. */
    if (g3d_skin_palette_sbuf && g3d_skin_joint_count > 0) {
        wgpuQueueWriteBuffer(g_wgpu_queue, g3d_skin_palette_sbuf, 0,
                             g3d_skin_palette_cpu,
                             (size_t)g3d_skin_joint_count * 12 * sizeof(float));
        g3d_skin_palette_dirty = false;
    }
}

/* Queue a skinned draw into the scene pass that is already open. */
void rae_ext_gpu3d_drawSkinned(int64_t mesh, rae_Mat4* model,
                               float r, float g, float b,
                               float metallic, float roughness) {
    if (!g3d_pass || !model) return;
    int slot = (int)mesh - 1;
    if (slot < 0 || slot >= g3d_skin_mesh_n) return;
    if (g3d_skin_draw_count >= G3D_SKIN_MAX_DRAWS) return;
    g3d_skin_init_pipeline();
    if (!g3d_skin_pipeline) return;

    /* The palette is per FRAME, not per draw, so it uploads once on the
     * first skinned draw rather than once per character part — twelve
     * primitives sharing one skeleton would otherwise upload it twelve
     * times. */
    if (g3d_skin_palette_dirty && g3d_skin_joint_count > 0) {
        wgpuQueueWriteBuffer(g_wgpu_queue, g3d_skin_palette_sbuf, 0, g3d_skin_palette_cpu,
                             (size_t)g3d_skin_joint_count * 12 * sizeof(float));
        g3d_skin_palette_dirty = false;
    }

    float* d = g3d_skin_draw_cpu + g3d_skin_draw_count * G3D_SKIN_DRAW_FLOATS;
    for (int i = 0; i < 16; i++) d[i] = model->m.v[i];
    d[16] = r; d[17] = g; d[18] = b; d[19] = metallic;
    d[20] = 0.0f; d[21] = 0.0f; d[22] = 0.0f; d[23] = roughness;
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_skin_draw_sbuf,
                         (uint64_t)g3d_skin_draw_count * G3D_SKIN_DRAW_FLOATS * sizeof(float),
                         d, G3D_SKIN_DRAW_FLOATS * sizeof(float));

    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_skin_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_skin_bind, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(g3d_pass, 0, g3d_skin_vbuf[slot], 0,
                                         wgpuBufferGetSize(g3d_skin_vbuf[slot]));
    wgpuRenderPassEncoderSetIndexBuffer(g3d_pass, g3d_skin_ibuf[slot],
                                        WGPUIndexFormat_Uint32, 0,
                                        wgpuBufferGetSize(g3d_skin_ibuf[slot]));
    wgpuRenderPassEncoderDrawIndexed(g3d_pass, g3d_skin_icount[slot], 1, 0, 0,
                                     (uint32_t)g3d_skin_draw_count);
    g3d_skin_draw_count++;
    /* Hand the pass back to the static pipeline: anything drawn after this
     * uses the plain layout, and leaving the skinned pipeline bound would
     * feed it vertices with no joint attributes. */
    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_bind, 0, NULL);
}

void rae_ext_gpu3d_skinFrameBegin(void) { g3d_skin_draw_count = 0; }

int64_t rae_ext_gpu3d_skinDrawCount(void) { return (int64_t)g3d_skin_draw_count; }

void rae_ext_gpu3d_skinShutdown(void) {
    for (int i = 0; i < g3d_skin_mesh_n; i++) {
        if (g3d_skin_vbuf[i]) { wgpuBufferRelease(g3d_skin_vbuf[i]); g3d_skin_vbuf[i] = NULL; }
        if (g3d_skin_ibuf[i]) { wgpuBufferRelease(g3d_skin_ibuf[i]); g3d_skin_ibuf[i] = NULL; }
    }
    g3d_skin_mesh_n = 0;
    if (g3d_skin_bind) { wgpuBindGroupRelease(g3d_skin_bind); g3d_skin_bind = NULL; }
    if (g3d_skin_draw_sbuf) { wgpuBufferRelease(g3d_skin_draw_sbuf); g3d_skin_draw_sbuf = NULL; }
    if (g3d_skin_palette_sbuf) { wgpuBufferRelease(g3d_skin_palette_sbuf); g3d_skin_palette_sbuf = NULL; }
    if (g3d_skin_pipeline) { wgpuRenderPipelineRelease(g3d_skin_pipeline); g3d_skin_pipeline = NULL; }
}
