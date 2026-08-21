/* gpu3d forward sky — the background, drawn inside the scene pass.
 *
 * The deferred path (#399-#404) computes its sky in the LIGHTING pass, for
 * every pixel the G-buffer left empty. The forward path has no G-buffer to ask,
 * so the same shader runs here instead: one fullscreen triangle at the very
 * start of the scene pass, before any geometry.
 *
 * The shader is not a second copy. runtime_sky_wgsl.h holds the one sky, and
 * both passes include it; only the uniform feeding it differs, because the
 * lighting pass already had sun and clear colour to hand and this pass does
 * not. Two renderers meant to be compared side by side (examples 111 and 112
 * are the same scene twice) must not disagree about what the sky looks like.
 *
 * DEPTH: the pipeline writes no depth and compares Always, so it paints every
 * pixel and then loses every one of them to geometry drawn afterwards. That is
 * cheaper than it sounds for the raster path and it is the only ordering that
 * works for the SDF path, whose raymarcher discards on a miss rather than
 * writing a background.
 *
 * VELOCITY: sky pixels report zero motion, exactly as the cleared attachment
 * did before this existed, so TAA smears the sky slightly when the camera
 * turns. The deferred background has the same property. Fixing it means
 * reprojecting the ray direction, and doing that in one renderer and not the
 * other would make the comparison lie -- so it stays a shared limitation.
 *
 * OPT IN. Nothing here runs unless the app calls gpu3d.drawSky between
 * beginScene and its geometry; an app that does not ask keeps the flat clear
 * colour it had before.
 */

/* Both guarded, and included here rather than relied on from elsewhere in the
 * translation unit: this file is #included into runtime_gpu3d.c, which comes
 * BEFORE the deferred path that also pulls them in. */
#include "runtime_sky_wgsl.h"
#include "runtime_sky_state.h"

static WGPURenderPipeline g3d_sky_pipeline = NULL;
static WGPUBuffer g3d_sky_ubuf = NULL;
static WGPUBindGroup g3d_sky_bind = NULL;

/* 80 floats: mat4 + 7 vec4 + 9 vec4 of cooked Hosek. Every member is vec4 or
 * mat4, so std140 padding never enters into it and the CPU-side struct below
 * can be a flat array with named offsets. */
#define G3D_SKY_U_FLOATS 80

static const char* G3D_SKY_WGSL =
"struct SkyU {\n"
"  invViewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"
"  sunDir: vec4<f32>,\n"          /* xyz toward the scene, matching Light3d */
"  sunColor: vec4<f32>,\n"
"  clearColor: vec4<f32>,\n"      /* what kind 0 (no sky) shows */
"  skyParams: vec4<f32>,\n"       /* x kind, y turbidity, z exposure, w sun angular size */
"  skyZenith: vec4<f32>,\n"       /* stylised zenith colour, w = band count */
"  skyHorizon: vec4<f32>,\n"      /* stylised horizon colour, w = sun disc intensity */
"  hosek: array<vec4<f32>, 9>,\n"
"};\n"
/* Named `L` because that is the contract runtime_sky_wgsl.h documents: the
 * shared shader reads L.sunDir, L.skyParams and the rest, and the deferred
 * lighting pass calls its own uniform the same thing. */
"@group(0) @binding(0) var<uniform> L: SkyU;\n"
"const PI: f32 = 3.14159265;\n"
RAE_SKY_WGSL
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
/* All four scene attachments, because the pipeline has to match the pass. The
 * last three are written with the values they were CLEARED to, so the sky
 * contributes no normal, no motion and no indirect light -- it is the
 * background, not a surface. */
"struct FsOut {\n"
"  @location(0) color: vec4<f32>,\n"
"  @location(1) normal: vec4<f32>,\n"
"  @location(2) velocity: vec2<f32>,\n"
"  @location(3) ambient: vec4<f32>,\n"
"};\n"
"@fragment\n"
"fn fs(in: VsOut) -> FsOut {\n"
/* The far plane point through this pixel, unprojected: subtracting the eye
 * gives the view ray. Reconstructed rather than interpolated so the direction
 * is exact per pixel — a sun disc a third of a degree wide is small enough
 * that a cheaper approximation shows as a wobble as the camera turns. */
"  let farH = L.invViewProj * vec4<f32>(in.ndc, 1.0, 1.0);\n"
"  let dir = normalize(farH.xyz / farH.w - L.camPos.xyz);\n"
"  var o: FsOut;\n"
"  o.color = vec4<f32>(skyColor(dir), 1.0);\n"
"  o.normal = vec4<f32>(0.0);\n"
"  o.velocity = vec2<f32>(0.0);\n"
"  o.ambient = vec4<f32>(0.0);\n"
"  return o;\n"
"}\n";

static void g3d_sky_init_pipeline(void) {
    if (g3d_sky_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G3D_SKY_WGSL);
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
    /* The two lines that make this a background rather than a wall at the far
     * plane: nothing it draws occludes anything drawn after it. */
    ds.depthWriteEnabled = WGPUOptionalBool_False;
    ds.depthCompare = WGPUCompareFunction_Always;
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
    g3d_sky_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!g3d_sky_pipeline) { fprintf(stderr, "[gpu3d] sky pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = G3D_SKY_U_FLOATS * sizeof(float);
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g3d_sky_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);

    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_sky_pipeline, 0);
    WGPUBindGroupEntry e; memset(&e, 0, sizeof(e));
    e.binding = 0; e.buffer = g3d_sky_ubuf; e.size = bd.size;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 1; bgd.entries = &e;
    g3d_sky_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

void rae_ext_gpu3d_skyHosekPush(int64_t index, float value) {
    rae_sky_hosek_push(index, value);
}

/* Draw the sky. Call after gpu3d.begin and before any geometry.
 *
 * The camera is NOT an argument: invViewProj and the eye position are the ones
 * gpu3d.begin already computed for this frame, so the sky cannot be drawn from
 * a different viewpoint than the scene in front of it. That was worth three
 * fewer arguments on a list this long. */
/* Sky bookkeeping (#514): ensure the sky pipeline, pack the sky uniform (frame
 * inv-viewproj + campos + sun/colors/params + the Hosek table Rae pushed) and
 * upload it. Returns 1 to proceed / 0 to skip; the Rae side (gpu3d.skyDraw)
 * encodes the fullscreen sky draw into the scene pass and restores the geometry
 * pipeline/bind, mirroring the metaball draw. */
int rae_g3d_sky_prepare(float skyKind, float turbidity, float skyExposure, float sunSizeRad,
                        float sunX, float sunY, float sunZ,
                        float sunR, float sunG, float sunB,
                        float zenR, float zenG, float zenB, float bands,
                        float horR, float horG, float horB, float discI,
                        float clearR, float clearG, float clearB) {
    g3d_sky_init_pipeline();
    if (!g3d_sky_pipeline || !g3d_sky_bind) return 0;

    float u[G3D_SKY_U_FLOATS]; memset(u, 0, sizeof(u));
    memcpy(u, g3d_frame_inv_viewproj, 16 * sizeof(float));
    u[16] = g3d_frame_campos[0]; u[17] = g3d_frame_campos[1]; u[18] = g3d_frame_campos[2];
    u[20] = sunX; u[21] = sunY; u[22] = sunZ;
    u[24] = sunR; u[25] = sunG; u[26] = sunB;
    u[28] = clearR; u[29] = clearG; u[30] = clearB; u[31] = 1.0f;
    u[32] = skyKind; u[33] = turbidity; u[34] = skyExposure; u[35] = sunSizeRad;
    u[36] = zenR; u[37] = zenG; u[38] = zenB; u[39] = bands;
    u[40] = horR; u[41] = horG; u[42] = horB; u[43] = discI;
    /* Pushed by Rae before this call, same as the deferred path; this only
     * copies. See runtime_sky_state.h. */
    memcpy(u + 44, rae_sky_hosek, 36 * sizeof(float));
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_sky_ubuf, 0, u, sizeof(u));
    return 1;
}
void* rae_g3d_sky_pipeline(void) { return (void*)g3d_sky_pipeline; }
void* rae_g3d_sky_bind(void)     { return (void*)g3d_sky_bind; }

static void g3d_sky_shutdown(void) {
    if (g3d_sky_bind) { wgpuBindGroupRelease(g3d_sky_bind); g3d_sky_bind = NULL; }
    if (g3d_sky_ubuf) { wgpuBufferRelease(g3d_sky_ubuf); g3d_sky_ubuf = NULL; }
    if (g3d_sky_pipeline) { wgpuRenderPipelineRelease(g3d_sky_pipeline); g3d_sky_pipeline = NULL; }
}
