/* gpu3d SDF pass — depth-integrated smooth-union sphere raymarching.
 *
 * This is renderer policy and therefore a future Rae-migration candidate.
 * The C implementation currently owns only the WebGPU pipeline and packed ABI
 * needed to render Scene3d SdfPrimitive data beside raster meshes. The pass
 * writes fragment depth into gpu3d's shared MSAA depth attachment, so implicit
 * and triangle geometry occlude each other normally.
 */

#define G3D_MAX_SDF_BALLS 16

static WGPURenderPipeline g3d_sdf_pipeline = NULL;
static WGPUBuffer g3d_sdf_ball_sbuf = NULL;
static WGPUBuffer g3d_sdf_param_ubuf = NULL;
static WGPUBindGroup g3d_sdf_bind = NULL;

static const char* G3D_SDF_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"
"  sunDir: vec4<f32>,\n"
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"  invViewProj: mat4x4<f32>,\n"
"};\n"
"struct Params {\n"
"  info: vec4<u32>,\n"
"  baseColorMetallic: vec4<f32>,\n"
"  emissiveRoughness: vec4<f32>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> balls: array<vec4<f32>>;\n"
"@group(0) @binding(2) var<uniform> P: Params;\n"
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
"fn mapScene(p: vec3<f32>) -> f32 {\n"
"  var d = 10000.0;\n"
"  var i = 0u;\n"
"  loop {\n"
"    if (i >= P.info.x) { break; }\n"
"    let b = balls[i];\n"
"    d = smoothMin(d, length(p - b.xyz) - b.w, 0.62);\n"
"    i = i + 1u;\n"
"  }\n"
"  return d;\n"
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
"fn aces(x: vec3<f32>) -> vec3<f32> {\n"
"  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3<f32>(0.0), vec3<f32>(1.0));\n"
"}\n"
"struct FsOut {\n"
"  @location(0) color: vec4<f32>,\n"
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
"  let albedo = P.baseColorMetallic.rgb;\n"
"  let metallic = clamp(P.baseColorMetallic.a, 0.0, 1.0);\n"
"  let rough = clamp(P.emissiveRoughness.a, 0.045, 1.0);\n"
"  let f0 = mix(vec3<f32>(0.04), albedo, metallic);\n"
"  let Fs = fresnel(VoH, f0);\n"
"  let spec = dGGX(NoH, rough) * gSmith(NoV, NoL, rough) * Fs / max(4.0 * NoV * NoL, 1e-4);\n"
"  let kd = (vec3<f32>(1.0) - Fs) * (1.0 - metallic);\n"
"  let direct = (kd * albedo / PI + spec) * F.sunColor.rgb * NoL;\n"
"  let hemi = mix(F.ambGround.rgb, F.ambSky.rgb, N.z * 0.5 + 0.5);\n"
"  let ambient = hemi * albedo * (1.0 - metallic) + hemi * Fs * (1.0 - rough * 0.7);\n"
"  var c = aces((direct + ambient + P.emissiveRoughness.rgb) * F.sunDir.w);\n"
"  c = pow(c, vec3<f32>(1.0 / 2.2));\n"
"  let clip = F.viewProj * vec4<f32>(worldPos, 1.0);\n"
"  var out: FsOut; out.color = vec4<f32>(c, 1.0); out.depth = clamp(clip.z / clip.w, 0.0, 1.0);\n"
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

    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.writeMask = WGPUColorWriteMask_All;
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
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.depthStencil = &ds;
    pd.multisample.count = 4; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g3d_sdf_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_sdf_pipeline) { fprintf(stderr, "[gpu3d] SDF pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = G3D_MAX_SDF_BALLS * 4u * sizeof(float);
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g3d_sdf_ball_sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    bd.size = 48;
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g3d_sdf_param_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);

    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_sdf_pipeline, 0);
    WGPUBindGroupEntry e[3]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g3d_frame_ubuf; e[0].size = 208;
    e[1].binding = 1; e[1].buffer = g3d_sdf_ball_sbuf; e[1].size = G3D_MAX_SDF_BALLS * 4u * sizeof(float);
    e[2].binding = 2; e[2].buffer = g3d_sdf_param_ubuf; e[2].size = 48;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 3; bgd.entries = e;
    g3d_sdf_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

void rae_ext_gpu3d_drawMetaballs(const double* packedBalls, int64_t count,
                                 double r, double g, double b,
                                 double metallic, double roughness,
                                 double emR, double emG, double emB) {
    if (!g3d_pass || !packedBalls || count <= 0) return;
    g3d_sdf_init_pipeline();
    if (!g3d_sdf_pipeline || !g3d_sdf_bind) return;
    if (count > G3D_MAX_SDF_BALLS) count = G3D_MAX_SDF_BALLS;
    if (getenv("RAE_GPU3D_SDF_TEST_LOG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[gpu3d] SDF metaballs: count=%lld\n", (long long)count);
            logged = 1;
        }
    }
    float balls[G3D_MAX_SDF_BALLS * 4];
    for (int64_t i = 0; i < count * 4; i++) balls[i] = (float)packedBalls[i];
    struct {
        uint32_t info[4];
        float baseColorMetallic[4];
        float emissiveRoughness[4];
    } params;
    memset(&params, 0, sizeof(params));
    params.info[0] = (uint32_t)count;
    params.baseColorMetallic[0] = (float)r;
    params.baseColorMetallic[1] = (float)g;
    params.baseColorMetallic[2] = (float)b;
    params.baseColorMetallic[3] = (float)metallic;
    params.emissiveRoughness[0] = (float)emR;
    params.emissiveRoughness[1] = (float)emG;
    params.emissiveRoughness[2] = (float)emB;
    params.emissiveRoughness[3] = (float)roughness;
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sdf_ball_sbuf, 0, balls,
                         (size_t)count * 4u * sizeof(float));
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sdf_param_ubuf, 0, &params, sizeof(params));
    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_sdf_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_sdf_bind, 0, NULL);
    wgpuRenderPassEncoderDraw(g3d_pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderSetPipeline(g3d_pass, g3d_pipeline);
    wgpuRenderPassEncoderSetBindGroup(g3d_pass, 0, g3d_bind, 0, NULL);
}

static void g3d_sdf_shutdown(void) {
    if (g3d_sdf_bind) { wgpuBindGroupRelease(g3d_sdf_bind); g3d_sdf_bind = NULL; }
    if (g3d_sdf_param_ubuf) { wgpuBufferRelease(g3d_sdf_param_ubuf); g3d_sdf_param_ubuf = NULL; }
    if (g3d_sdf_ball_sbuf) { wgpuBufferRelease(g3d_sdf_ball_sbuf); g3d_sdf_ball_sbuf = NULL; }
    if (g3d_sdf_pipeline) { wgpuRenderPipelineRelease(g3d_sdf_pipeline); g3d_sdf_pipeline = NULL; }
}
