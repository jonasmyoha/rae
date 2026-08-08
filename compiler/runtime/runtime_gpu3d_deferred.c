/* Deferred frame — depth pyramid, lighting, composite (#356, Track B).
 *
 * The passes that consume what runtime_gpu3d_gbuffer.c produces. Split
 * into its own file because the geometry pass and the passes that read it
 * are separate concerns that change for different reasons, and because
 * runtime_gpu3d.c is already the cautionary tale for letting a renderer
 * family accumulate in one file (queue #364).
 *
 * DEPTH PYRAMID. A mip chain of depth reduced to the NEAREST value, not an
 * average. The consumer is occlusion: "is anything in this screen region
 * closer than my bounding box" is answered by the nearest depth, and an
 * averaged pyramid would report a plausible-looking value that is
 * conservative in neither direction — it would cull visible geometry.
 * Under reverse-Z (#367) nearest is the MAXIMUM, so the reduction is max;
 * it was min before the depth convention flipped, and a pyramid reducing
 * the wrong way is invisible until something culls with it.
 * Built with fragment passes rather than compute for the same reason the
 * SSAO pass is: they run everywhere WebGPU runs, with no workgroup-size
 * guess. Hi-Z culling and a deferred SSAO are the intended readers; the
 * chain exists now so those arrive as consumers of a real resource rather
 * than as a reshaping of the frame.
 *
 * LIGHTING. One fullscreen pass, Cook-Torrance GGX + Smith + Schlick over
 * the G-buffer, writing LINEAR HDR radiance into litColor. This is the
 * payoff of the whole split: its cost is one evaluation per pixel and does
 * not scale with how many objects wrote that pixel. World position is
 * reconstructed from depth and the inverse view-projection rather than
 * read from a stored position buffer — the reason the G-buffer has no
 * position channel.
 *
 * COMPOSITE. Exposure + ACES + gamma, litColor -> the presentable
 * offscreen. Identical tone curve to the forward frame's tonemap on
 * purpose: the two frames should differ in how radiance is computed, not
 * in how it is displayed, or comparing them proves nothing.
 */

#define GB_PYRAMID_MAX_MIPS 16
#define GB_LIGHT_BYTES 160   /* mat4 invViewProj + 6 vec4 */

static WGPUTexture     gb_pyramid_tex = NULL;    /* r32float, half res, mip chain */
static WGPUTextureView gb_pyramid_rt[GB_PYRAMID_MAX_MIPS];   /* one per mip, as target */
static WGPUTextureView gb_pyramid_src[GB_PYRAMID_MAX_MIPS];  /* one per mip, as source */
static int             gb_pyramid_mips = 0;
static int             gb_pyramid_w = 0, gb_pyramid_h = 0;

static WGPUTexture     gb_lit_tex = NULL;        /* rgba16float, linear HDR radiance */
static WGPUTextureView gb_lit_view = NULL;

static WGPURenderPipeline gb_pyr_from_depth_pipeline = NULL;  /* depth32f -> r32f */
static WGPURenderPipeline gb_pyr_reduce_pipeline = NULL;      /* r32f -> r32f */
static WGPUBindGroup      gb_pyr_bind[GB_PYRAMID_MAX_MIPS];

static WGPURenderPipeline gb_light_pipeline = NULL;
static WGPUBindGroup      gb_light_bind = NULL;
static WGPUBuffer         gb_light_ubuf = NULL;

static WGPURenderPipeline gb_composite_pipeline = NULL;
static WGPUBindGroup      gb_composite_bind = NULL;
static WGPUBuffer         gb_composite_ubuf = NULL;

static int gb_deferred_gen = -1;   /* gb_targets_gen this file's binds were built for */

/* Fullscreen triangle, shared by every pass here. Three vertices covering
 * the viewport beats two triangles: no shared edge, so no risk of a seam
 * from inconsistent interpolation along the diagonal. */
#define GB_FULLSCREEN_VS \
"@vertex\n" \
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n" \
"  var points = array<vec2<f32>, 3>(\n" \
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n" \
"  return vec4<f32>(points[vi], 0.0, 1.0);\n" \
"}\n"

/* Mip 0 of the pyramid: reduce full-res depth into the half-res chain. */
static const char* GB_PYR_FROM_DEPTH_WGSL =
"@group(0) @binding(0) var srcTex: texture_depth_2d;\n"
GB_FULLSCREEN_VS
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) f32 {\n"
"  let px = vec2<i32>(pos.xy) * 2;\n"
"  let dim = vec2<i32>(textureDimensions(srcTex, 0)) - vec2<i32>(1);\n"
/* Clamp rather than skip: on an odd-sized level the 2x2 footprint runs
 * off the edge, and sampling out of bounds returns zero — which under
 * reverse-Z is the FAR plane, so an unclamped border would report nothing
 * occluding rather than something wrongly close. Clamping keeps the border
 * honest either way. */
"  let a = textureLoad(srcTex, min(px,                  dim), 0);\n"
"  let b = textureLoad(srcTex, min(px + vec2<i32>(1,0), dim), 0);\n"
"  let c = textureLoad(srcTex, min(px + vec2<i32>(0,1), dim), 0);\n"
"  let d = textureLoad(srcTex, min(px + vec2<i32>(1,1), dim), 0);\n"
"  return max(max(a, b), max(c, d));\n"
"}\n";

/* Mip n from mip n-1. Same reduction, non-depth source format. */
static const char* GB_PYR_REDUCE_WGSL =
"@group(0) @binding(0) var srcTex: texture_2d<f32>;\n"
GB_FULLSCREEN_VS
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) f32 {\n"
"  let px = vec2<i32>(pos.xy) * 2;\n"
"  let dim = vec2<i32>(textureDimensions(srcTex, 0)) - vec2<i32>(1);\n"
"  let a = textureLoad(srcTex, min(px,                  dim), 0).r;\n"
"  let b = textureLoad(srcTex, min(px + vec2<i32>(1,0), dim), 0).r;\n"
"  let c = textureLoad(srcTex, min(px + vec2<i32>(0,1), dim), 0).r;\n"
"  let d = textureLoad(srcTex, min(px + vec2<i32>(1,1), dim), 0).r;\n"
"  return max(max(a, b), max(c, d));\n"
"}\n";

/* Deferred lighting. Reads the G-buffer, writes linear HDR radiance.
 *
 * The BRDF is deliberately the same one the forward pass uses. Anywhere
 * the two disagree is a bug in one of them, and keeping them identical is
 * what makes the forward frame a usable reference image while the deferred
 * frame is being built. */
static const char* GB_LIGHT_WGSL =
"struct LightU {\n"
"  invViewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"      /* xyz cam, w exposure (used by composite) */
"  sunDir: vec4<f32>,\n"      /* xyz direction TOWARD the scene */
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"  clearColor: vec4<f32>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> L: LightU;\n"
"@group(0) @binding(1) var gbaTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var gbbTex: texture_2d<f32>;\n"
"@group(0) @binding(3) var gbcTex: texture_2d<f32>;\n"
"@group(0) @binding(4) var depthTex: texture_depth_2d;\n"
GB_OCT_WGSL
GB_FULLSCREEN_VS
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
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let px = vec2<i32>(pos.xy);\n"
"  let depth = textureLoad(depthTex, px, 0);\n"
/* REVERSE-Z (#367): the far plane is 0, not 1. Depth still at 0 means the
 * geometry pass wrote nothing here. Reconstructing a position from it
 * would place the pixel at infinity and light it as if it were a surface;
 * the background is not a surface, so it passes through as the clear
 * colour. Getting this comparison the wrong way round after the reverse-Z
 * switch would light the sky and discard the scene. */
"  if (depth <= 0.0) {\n"
"    return vec4<f32>(L.clearColor.rgb, 1.0);\n"
"  }\n"
"  let gbb = textureLoad(gbbTex, px, 0);\n"
"  let albedo = gbb.rgb;\n"
"  let gba = textureLoad(gbaTex, px, 0);\n"
"  let mode = gba.w;\n"
/* Unlit surfaces skip every calculation below and emit albedo directly.
 * This early-out is a real part of why the mode field earns its 2 bits. */
"  if (mode > 0.5) {\n"
"    return vec4<f32>(albedo, 1.0);\n"
"  }\n"
"  let dim = vec2<f32>(textureDimensions(gbbTex, 0));\n"
"  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / dim;\n"
/* WebGPU clip space: xy in [-1,1] with +y UP, z in [0,1]. UV runs +y DOWN,
 * hence the flip. Getting this backwards mirrors the lighting vertically,
 * which reads as "the sun is in the wrong place" rather than as a
 * reconstruction bug. The depth value goes in as-is: it is inverted by the
 * projection matrix, and invViewProj is that same matrix's inverse. */
"  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);\n"
"  let wpos4 = L.invViewProj * ndc;\n"
"  let wpos = wpos4.xyz / wpos4.w;\n"
"  let N = octDecode(gba.xy);\n"
"  let gbc = textureLoad(gbcTex, px, 0);\n"
"  let metallic = clamp(gbc.z, 0.0, 1.0);\n"
"  let rough = clamp(gbb.a, 0.045, 1.0);\n"
/* C.w is occlusion for a lit surface and log-encoded emissive for an
 * emitter — the mode decides which, so an emitter is never also occluded
 * and an occluded surface never also glows. */
"  var ao = gbc.w;\n"
"  var emissive = 0.0;\n"
"  if (mode > 0.0) {\n"
"    emissive = exp(ao * 6.91) - 1.0;\n"
"    ao = 1.0;\n"
"  }\n"
"  let V = normalize(L.camPos.xyz - wpos);\n"
"  let Ldir = normalize(-L.sunDir.xyz);\n"
"  let H = normalize(V + Ldir);\n"
"  let NoV = max(dot(N, V), 1e-4);\n"
"  let NoL = max(dot(N, Ldir), 0.0);\n"
"  let NoH = max(dot(N, H), 0.0);\n"
"  let VoH = max(dot(V, H), 0.0);\n"
"  let f0 = mix(vec3<f32>(0.04), albedo, metallic);\n"
"  let Fs = fresnel(VoH, f0);\n"
"  let spec = dGGX(NoH, rough) * gSmith(NoV, NoL, rough) * Fs\n"
"           / max(4.0 * NoV * NoL, 1e-4);\n"
"  let kd = (vec3<f32>(1.0) - Fs) * (1.0 - metallic);\n"
"  let direct = (kd * albedo / PI + spec) * L.sunColor.rgb * NoL;\n"
/* Z-up world (docs/coordinate-system.md), so the hemisphere blends on N.z.
 * The forward pass blends on N.y because it predates the convention — that
 * disagreement is real and tracked, and this is the correct one. */
"  let hemi = mix(L.ambGround.rgb, L.ambSky.rgb, N.z * 0.5 + 0.5);\n"
"  let ambF = fresnel(NoV, f0);\n"
"  let ambient = (hemi * albedo * (1.0 - metallic) + hemi * ambF * (1.0 - rough * 0.7)) * ao;\n"
/* Emissive tints by albedo, matching how it is authored: an emitter's base
 * colour IS the colour it emits, which is why one scalar suffices. */
"  let emit = albedo * emissive;\n"
/* LINEAR HDR out. Exposure, ACES and gamma belong to the composite, so a
 * later SSAO/GI pass can attenuate radiance rather than tonemapped values. */
"  return vec4<f32>(direct + ambient + emit, 1.0);\n"
"}\n";

/* Composite: linear HDR radiance -> the presentable LDR offscreen. */
static const char* GB_COMPOSITE_WGSL =
"@group(0) @binding(0) var<uniform> P: vec4<f32>;\n"   /* x = exposure */
"@group(0) @binding(1) var litTex: texture_2d<f32>;\n"
GB_FULLSCREEN_VS
"fn aces(x: vec3<f32>) -> vec3<f32> {\n"
"  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),\n"
"               vec3<f32>(0.0), vec3<f32>(1.0));\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let hdr = textureLoad(litTex, vec2<i32>(pos.xy), 0).rgb;\n"
"  var c = aces(hdr * P.x);\n"
"  c = pow(c, vec3<f32>(1.0 / 2.2));\n"
"  return vec4<f32>(c, 1.0);\n"
"}\n";

/* Build a fullscreen pipeline: no vertex buffers, no depth, one target. */
static WGPURenderPipeline gb_make_fullscreen_pipeline(const char* wgsl, WGPUTextureFormat fmt,
                                                      const char* label) {
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(wgsl);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = fmt; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;
    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!p) fprintf(stderr, "[deferred] %s pipeline creation FAILED\n", label);
    return p;
}

static void gb_deferred_release_targets(void) {
    for (int i = 0; i < GB_PYRAMID_MAX_MIPS; i++) {
        if (gb_pyr_bind[i])    { wgpuBindGroupRelease(gb_pyr_bind[i]); gb_pyr_bind[i] = NULL; }
        if (gb_pyramid_rt[i])  { wgpuTextureViewRelease(gb_pyramid_rt[i]); gb_pyramid_rt[i] = NULL; }
        if (gb_pyramid_src[i]) { wgpuTextureViewRelease(gb_pyramid_src[i]); gb_pyramid_src[i] = NULL; }
    }
    if (gb_pyramid_tex)    { wgpuTextureRelease(gb_pyramid_tex); gb_pyramid_tex = NULL; }
    if (gb_light_bind)     { wgpuBindGroupRelease(gb_light_bind); gb_light_bind = NULL; }
    if (gb_composite_bind) { wgpuBindGroupRelease(gb_composite_bind); gb_composite_bind = NULL; }
    if (gb_lit_view)       { wgpuTextureViewRelease(gb_lit_view); gb_lit_view = NULL; }
    if (gb_lit_tex)        { wgpuTextureRelease(gb_lit_tex); gb_lit_tex = NULL; }
    gb_pyramid_mips = 0;
}

/* Recreate everything downstream of the G-buffer whenever the G-buffer
 * itself was recreated. Keyed on gb_targets_gen rather than on dimensions:
 * a resize back to a previous size still invalidates the bind groups, and
 * comparing sizes would miss that. */
static void gb_deferred_ensure(void) {
    if (gb_deferred_gen == gb_targets_gen && gb_pyramid_tex && gb_lit_tex) return;
    if (gb_target_w <= 0 || gb_target_h <= 0) return;
    gb_deferred_release_targets();

    int pw = gb_target_w / 2; if (pw < 1) pw = 1;
    int ph = gb_target_h / 2; if (ph < 1) ph = 1;
    int mips = 1;
    {
        int m = pw > ph ? pw : ph;
        while (m > 1 && mips < GB_PYRAMID_MAX_MIPS) { m /= 2; mips++; }
    }

    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)pw; td.size.height = (uint32_t)ph; td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = (uint32_t)mips; td.sampleCount = 1;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    td.format = WGPUTextureFormat_R32Float;
    gb_pyramid_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    for (int i = 0; i < mips; i++) {
        WGPUTextureViewDescriptor vd; memset(&vd, 0, sizeof(vd));
        vd.format = WGPUTextureFormat_R32Float;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = (uint32_t)i; vd.mipLevelCount = 1;
        vd.baseArrayLayer = 0; vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_All;
        gb_pyramid_rt[i]  = wgpuTextureCreateView(gb_pyramid_tex, &vd);
        gb_pyramid_src[i] = wgpuTextureCreateView(gb_pyramid_tex, &vd);
    }
    gb_pyramid_mips = mips;
    gb_pyramid_w = pw; gb_pyramid_h = ph;

    memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)gb_target_w; td.size.height = (uint32_t)gb_target_h;
    td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = 1; td.sampleCount = 1;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    td.format = WGPUTextureFormat_RGBA16Float;   /* linear HDR radiance */
    gb_lit_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    gb_lit_view = wgpuTextureCreateView(gb_lit_tex, NULL);

    gb_deferred_gen = gb_targets_gen;
}

static void gb_deferred_init_pipelines(void) {
    if (!gb_pyr_from_depth_pipeline)
        gb_pyr_from_depth_pipeline = gb_make_fullscreen_pipeline(
            GB_PYR_FROM_DEPTH_WGSL, WGPUTextureFormat_R32Float, "depth-pyramid mip0");
    if (!gb_pyr_reduce_pipeline)
        gb_pyr_reduce_pipeline = gb_make_fullscreen_pipeline(
            GB_PYR_REDUCE_WGSL, WGPUTextureFormat_R32Float, "depth-pyramid reduce");
    if (!gb_light_pipeline) {
        gb_light_pipeline = gb_make_fullscreen_pipeline(
            GB_LIGHT_WGSL, WGPUTextureFormat_RGBA16Float, "lighting");
        WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
        ud.size = GB_LIGHT_BYTES;
        ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        gb_light_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
    }
    if (!gb_composite_pipeline) {
        gb_composite_pipeline = gb_make_fullscreen_pipeline(
            GB_COMPOSITE_WGSL, g_g2d_fmt, "composite");
        WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
        ud.size = 16; ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        gb_composite_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
    }
}

/* One fullscreen pass: bind, draw three vertices, submit. */
static void gb_run_fullscreen(WGPURenderPipeline pipeline, WGPUBindGroup bind,
                              WGPUTextureView target) {
    if (!pipeline || !bind || !target) return;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = target;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
}

/* Build the depth pyramid: mip 0 from the G-buffer's depth, then each
 * level from the one above. Each level is a separate pass because a level
 * cannot be read until the level that produces it has finished. */
void rae_ext_gbuffer_depthPyramid(void) {
    if (!g_wgpu_dev || !gb_depth_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_pyramid_tex || !gb_pyr_from_depth_pipeline || !gb_pyr_reduce_pipeline) return;

    for (int i = 0; i < gb_pyramid_mips; i++) {
        if (!gb_pyr_bind[i]) {
            WGPURenderPipeline p = (i == 0) ? gb_pyr_from_depth_pipeline : gb_pyr_reduce_pipeline;
            WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(p, 0);
            WGPUBindGroupEntry e; memset(&e, 0, sizeof(e));
            e.binding = 0;
            e.textureView = (i == 0) ? gb_depth_view : gb_pyramid_src[i - 1];
            WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
            bgd.layout = bgl; bgd.entryCount = 1; bgd.entries = &e;
            gb_pyr_bind[i] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
            wgpuBindGroupLayoutRelease(bgl);
        }
        gb_run_fullscreen(i == 0 ? gb_pyr_from_depth_pipeline : gb_pyr_reduce_pipeline,
                          gb_pyr_bind[i], gb_pyramid_rt[i]);
    }
    if (getenv("RAE_GBUFFER_DEBUG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[deferred] depth pyramid: %dx%d, %d mips\n",
                    gb_pyramid_w, gb_pyramid_h, gb_pyramid_mips);
            logged = 1;
        }
    }
}

/* Deferred lighting. Sun direction points TOWARD the scene, matching
 * Light3d and the forward path. */
void rae_ext_gbuffer_lighting(float camX, float camY, float camZ, float exposure,
                              float sunX, float sunY, float sunZ,
                              float sunR, float sunG, float sunB,
                              float skyR, float skyG, float skyB,
                              float gndR, float gndG, float gndB) {
    if (!g_wgpu_dev || !gb_a_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_light_pipeline || !gb_lit_view) return;

    float u[GB_LIGHT_BYTES / 4];
    memset(u, 0, sizeof(u));
    /* Invert THIS frame's view-projection — the one the geometry pass
     * rendered with — so reconstruction matches the depth being read. */
    if (!g3d_invert_mat4(gb_viewproj, u)) {
        /* Singular: a degenerate camera. Leaving the matrix zeroed would
         * collapse every pixel to the origin and light the frame from
         * inside itself; identity at least fails visibly and predictably. */
        memset(u, 0, 16 * sizeof(float));
        u[0] = 1.0f; u[5] = 1.0f; u[10] = 1.0f; u[15] = 1.0f;
    }
    u[16] = camX; u[17] = camY; u[18] = camZ; u[19] = exposure;
    u[20] = sunX; u[21] = sunY; u[22] = sunZ; u[23] = 0.0f;
    u[24] = sunR; u[25] = sunG; u[26] = sunB; u[27] = 0.0f;
    u[28] = skyR; u[29] = skyG; u[30] = skyB; u[31] = 0.0f;
    u[32] = gndR; u[33] = gndG; u[34] = gndB; u[35] = 0.0f;
    u[36] = gb_clear[0]; u[37] = gb_clear[1]; u[38] = gb_clear[2]; u[39] = 0.0f;
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_light_ubuf, 0, u, GB_LIGHT_BYTES);

    if (!gb_light_bind) {
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_light_pipeline, 0);
        WGPUBindGroupEntry e[5]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = gb_light_ubuf; e[0].size = GB_LIGHT_BYTES;
        e[1].binding = 1; e[1].textureView = gb_a_view;
        e[2].binding = 2; e[2].textureView = gb_b_view;
        e[3].binding = 3; e[3].textureView = gb_c_view;
        e[4].binding = 4; e[4].textureView = gb_depth_view;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 5; bgd.entries = e;
        gb_light_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        wgpuBindGroupLayoutRelease(bgl);
    }
    gb_run_fullscreen(gb_light_pipeline, gb_light_bind, gb_lit_view);
}

/* Composite radiance into the presentable offscreen. */
void rae_ext_gbuffer_composite(float exposure) {
    if (!g_wgpu_dev || !g_g2d_off_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_composite_pipeline || !gb_lit_view) return;

    float p[4] = { exposure, 0.0f, 0.0f, 0.0f };
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_composite_ubuf, 0, p, sizeof(p));
    if (!gb_composite_bind) {
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_composite_pipeline, 0);
        WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = gb_composite_ubuf; e[0].size = 16;
        e[1].binding = 1; e[1].textureView = gb_lit_view;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
        gb_composite_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        wgpuBindGroupLayoutRelease(bgl);
    }
    gb_run_fullscreen(gb_composite_pipeline, gb_composite_bind, g_g2d_off_view);
}

/* Number of mip levels in the pyramid — 0 before the first build. Lets a
 * caller or test confirm the chain was actually built rather than skipped. */
int64_t rae_ext_gbuffer_pyramidMips(void) { return (int64_t)gb_pyramid_mips; }

void rae_ext_gbuffer_deferredShutdown(void) {
    gb_deferred_release_targets();
    if (gb_pyr_from_depth_pipeline) { wgpuRenderPipelineRelease(gb_pyr_from_depth_pipeline); gb_pyr_from_depth_pipeline = NULL; }
    if (gb_pyr_reduce_pipeline)     { wgpuRenderPipelineRelease(gb_pyr_reduce_pipeline); gb_pyr_reduce_pipeline = NULL; }
    if (gb_light_pipeline)          { wgpuRenderPipelineRelease(gb_light_pipeline); gb_light_pipeline = NULL; }
    if (gb_light_ubuf)              { wgpuBufferRelease(gb_light_ubuf); gb_light_ubuf = NULL; }
    if (gb_composite_pipeline)      { wgpuRenderPipelineRelease(gb_composite_pipeline); gb_composite_pipeline = NULL; }
    if (gb_composite_ubuf)          { wgpuBufferRelease(gb_composite_ubuf); gb_composite_ubuf = NULL; }
    gb_deferred_gen = -1;
}
