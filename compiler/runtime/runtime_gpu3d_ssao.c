/* gpu3d SSAO — horizon-search ambient occlusion with a visibility bitmask.
 *
 * ALGORITHM. This is GTAO (Jimenez et al. 2016) with the occlusion state per
 * slice kept as a 32-bit VISIBILITY BITMASK rather than a single horizon
 * angle (Jover 2023, "Screen Space Indirect Lighting with Visibility
 * Bitmask"). Why not classic hemisphere-sampling SSAO:
 *
 *   - A horizon search integrates the visibility of a whole angular sector
 *     per sample instead of testing one point, so it converges on the
 *     ground-truth cosine-weighted integral with far fewer samples. That
 *     ratio is what makes a mobile budget viable.
 *   - Single-horizon GTAO has to guess what is BEHIND an occluder, because
 *     one angle cannot express "occluded, then open, then occluded again".
 *     Implementations paper over it with a thickness heuristic, which
 *     over-darkens thin geometry — the railing/foliage artefact. A bitmask
 *     records each occluded sector independently, so a thin occluder blocks
 *     only the sectors it actually covers and separated occluders compose
 *     correctly. It costs bit operations, not texture reads.
 *
 * COSINE WEIGHTING. Sector boundaries are uniform in sin(angle), not in
 * angle: the cosine-weighted visibility integral over a slice is
 * proportional to sin, so a plain popcount of uniformly-spaced-in-sin
 * sectors already approximates the weighted integral. No per-sector weight
 * table, no arccos in the inner loop.
 *
 * CONTACT SCALE ONLY (#337). The radius is deliberately short. A
 * large-radius SSAO would be obsoleted by real world-space GI (#339) and
 * would then double-darken with it; contact-scale occlusion stays valid
 * alongside GI because it captures detail below the GI representation's
 * resolution.
 *
 * PERFORMANCE / PORTABILITY.
 *   - Half resolution, then an edge-aware (depth-weighted) bilateral
 *     upsample in the composite. Full-res AO is mostly wasted on a signal
 *     this low-frequency.
 *   - Quality is one knob: slices x steps. The mobile preset is 2x4 = 8
 *     depth taps per pixel at quarter the pixels; desktop is 3x6.
 *   - Interleaved gradient noise rotates the slice basis per pixel, so the
 *     8 taps behave like many more once TAA (#335) accumulates them. This
 *     is the first real consumer of the noise work in #320.
 *   - Fragment passes, not compute: they run everywhere WebGPU runs,
 *     including the WASM/mobile targets, without a workgroup-size guess.
 *
 * Applied to the INDIRECT term only. The scene pass writes ambient to its
 * own target (#337) precisely so this composite can attenuate it without
 * touching direct light — a surface in direct sun inside a crevice is
 * still lit.
 */

/* Defined later in the including TU; the SSAO passes must close the scene
 * pass before they can read its depth/normal/ambient attachments. */
static int rae_g3d_finish_pass(void);

#define G3D_SSAO_PARAM_BYTES 96  /* mat4 invViewProj + camPos vec4 + cfg vec4 */

static WGPUTexture     g3d_ao_tex = NULL;        /* r8unorm, half res */
static WGPUTextureView g3d_ao_view = NULL;
static int             g3d_ao_w = 0, g3d_ao_h = 0;
static WGPURenderPipeline g3d_ssao_pipeline = NULL;
static WGPUBindGroup      g3d_ssao_bind = NULL;
static WGPURenderPipeline g3d_ao_apply_pipeline = NULL;
static WGPURenderPipeline g3d_ao_debug_pipeline = NULL;   /* RAE_SSAO_DEBUG=1 */
static WGPUBindGroup      g3d_ao_debug_bind = NULL;       /* auto layouts are pipeline-exclusive */
static bool g3d_ao_debug = false;
static WGPUBindGroup      g3d_ao_apply_bind = NULL;
static WGPUBuffer         g3d_ssao_param_ubuf = NULL;
static WGPUSampler        g3d_ssao_sampler = NULL;
static bool               g3d_ssao_enabled = true;
static bool               g3d_ssao_pending = false;
static int                g3d_ssao_slices = 3;   /* desktop default */
static int                g3d_ssao_steps = 6;
static float              g3d_ssao_radius = 1.2f;    /* world units: contact scale */
static float              g3d_ssao_intensity = 1.0f;

/* ---- AO generation: horizon search with a visibility bitmask ---- */
static const char* G3D_SSAO_WGSL =
"struct P {\n"
"  invViewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"      /* xyz camera, w frame index (jitters the noise) */
"  cfg: vec4<f32>,\n"         /* x radius, y intensity, z slices, w steps */
"};\n"
"@group(0) @binding(0) var<uniform> U: P;\n"
"@group(0) @binding(1) var depthTex: texture_depth_2d;\n"
"@group(0) @binding(2) var normalTex: texture_2d<f32>;\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n"
"  var pts = array<vec2<f32>, 3>(\n"
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n"
"  return vec4<f32>(pts[vi], 0.0, 1.0);\n"
"}\n"
/* World position from a depth sample. Reversed nothing: depth is [0,1] and
 * the projection is the same one the scene pass used, so one inverse
 * transform recovers the exact surface point. */
"fn worldFromDepth(uv: vec2<f32>, d: f32) -> vec3<f32> {\n"
"  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, d, 1.0);\n"
"  let p = U.invViewProj * ndc;\n"
"  return p.xyz / p.w;\n"
"}\n"
/* Interleaved gradient noise (Jimenez): one madd and a fract, and it
 * decorrelates neighbours well enough that the bilateral upsample and TAA
 * can resolve the remaining variance. */
"fn ign(px: vec2<f32>, frame: f32) -> f32 {\n"
"  let p = px + 5.588238 * frame;\n"
"  return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));\n"
"}\n"
/* Map a signed elevation angle to a sector index. Uniform in SIN, so a
 * popcount of the mask approximates the COSINE-weighted integral. */
"fn sectorOf(sinA: f32) -> i32 {\n"
"  return i32(clamp((sinA * 0.5 + 0.5) * 32.0, 0.0, 31.0));\n"
"}\n"
"fn sectorSpan(lo: i32, hi: i32) -> u32 {\n"
"  let a = min(lo, hi);\n"
"  let b = max(lo, hi);\n"
"  let count = u32(b - a + 1);\n"
"  if (count >= 32u) { return 0xFFFFFFFFu; }\n"
"  return ((1u << count) - 1u) << u32(a);\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) frag: vec4<f32>) -> @location(0) f32 {\n"
"  let aoDims = vec2<f32>(textureDimensions(normalTex)) * 0.5;\n"
"  let fullDims = vec2<f32>(textureDimensions(normalTex));\n"
"  let uv = frag.xy / aoDims;\n"
"  let fullPx = vec2<i32>(uv * fullDims);\n"
"  let centerDepth = textureLoad(depthTex, fullPx, 0);\n"
"  if (centerDepth >= 1.0) { return 1.0; }\n"   /* sky: unoccluded */
"  let P0 = worldFromDepth(uv, centerDepth);\n"
"  let N = normalize(textureLoad(normalTex, fullPx, 0).xyz);\n"
"  let V = normalize(U.camPos.xyz - P0);\n"
"  let radius = U.cfg.x;\n"
"  let slices = i32(U.cfg.z);\n"
"  let steps = i32(U.cfg.w);\n"
/* Screen-space march length for the world-space radius: project the radius
 * at this pixel's depth so the footprint shrinks with distance instead of
 * sampling ever-wider as geometry recedes. */
"  let pxAtRadius = worldFromDepth(uv + vec2<f32>(0.02, 0.0), centerDepth);\n"
"  let worldPerUv = max(length(pxAtRadius - P0) / 0.02, 1e-4);\n"
"  var marchUv = radius / worldPerUv;\n"
"  marchUv = clamp(marchUv, 1.5 / fullDims.x, 0.08);\n"
"  let rnd = ign(frag.xy, U.camPos.w);\n"
"  var visible = 0.0;\n"
"  var sliceCount = 0.0;\n"
"  for (var s = 0; s < slices; s = s + 1) {\n"
"    let phi = (f32(s) + rnd) / f32(slices) * 3.14159265;\n"
"    let dir = vec2<f32>(cos(phi), sin(phi));\n"
"    var mask: u32 = 0u;\n"
/* Both directions along the slice, so one loop covers the full 180 degrees
 * the slice represents. */
"    for (var side = 0; side < 2; side = side + 1) {\n"
"      let sgn = select(-1.0, 1.0, side == 0);\n"
"      for (var t = 1; t <= steps; t = t + 1) {\n"
"        let jitter = (f32(t) - 0.5 + rnd) / f32(steps);\n"
"        let sampleUv = uv + dir * (sgn * jitter * marchUv);\n"
"        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0) { continue; }\n"
"        let sd = textureLoad(depthTex, vec2<i32>(sampleUv * fullDims), 0);\n"
"        if (sd >= 1.0) { continue; }\n"
"        let Ps = worldFromDepth(sampleUv, sd);\n"
"        let delta = Ps - P0;\n"
"        let dist = length(delta);\n"
"        if (dist < 1e-5 || dist > radius) { continue; }\n"
"        let dirw = delta / dist;\n"
/* Elevation of the sample above the surface tangent plane, and of the far
 * side of the assumed-thin occluder. Recording the SPAN rather than a
 * single horizon is what the bitmask buys: an occluder blocks only the
 * sectors between its front and back, so geometry behind a thin object
 * still contributes light. */
"        let sinFront = dot(dirw, N);\n"
/* The occluder occupies the angular span between its front surface and its
 * (unknown) back. Push the back AWAY from the camera: deeper geometry
 * subtends a LOWER elevation, so the span runs from sinBack up to
 * sinFront. Offsetting toward the camera instead collapses the span to
 * nothing, which is a silent under-darkening rather than an error.
 * Thickness is a fraction of the radius, the paper's assumed-thickness
 * parameter — there is no second depth layer to read it from. */
"        let backPoint = Ps - V * (radius * 0.5);\n"
"        let sinBack = dot(normalize(backPoint - P0), N);\n"
"        if (sinFront <= 0.0) { continue; }\n"
"        let hi = sectorOf(sinFront);\n"
"        let lo = sectorOf(max(sinBack, 0.0));\n"
"        mask = mask | sectorSpan(lo, hi);\n"
"      }\n"
"    }\n"
/* Only the upper half of the sector range is above the tangent plane; the
 * lower 16 sectors can never be occluded by definition, so normalise
 * against 16 rather than 32. */
"    let occluded = f32(countOneBits(mask));\n"
"    visible = visible + clamp(1.0 - occluded / 16.0, 0.0, 1.0);\n"
"    sliceCount = sliceCount + 1.0;\n"
"  }\n"
"  var ao = select(1.0, visible / sliceCount, sliceCount > 0.0);\n"
"  ao = pow(clamp(ao, 0.0, 1.0), max(U.cfg.y, 0.01));\n"
"  return ao;\n"
"}\n";

/* ---- composite: bilateral upsample + apply to the ambient term ---- */
static const char* G3D_AO_APPLY_WGSL =
"@group(0) @binding(0) var aoTex: texture_2d<f32>;\n"
"@group(0) @binding(1) var ambientTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var depthTex: texture_depth_2d;\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n"
"  var pts = array<vec2<f32>, 3>(\n"
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n"
"  return vec4<f32>(pts[vi], 0.0, 1.0);\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let px = vec2<i32>(frag.xy);\n"
"  let ambient = textureLoad(ambientTex, px, 0).rgb;\n"
"  let aoDims = vec2<i32>(textureDimensions(aoTex));\n"
"  let half = px / 2;\n"
"  let centerDepth = textureLoad(depthTex, px, 0);\n"
/* Depth-weighted (bilateral) 2x2 gather. A plain bilinear upsample bleeds
 * occlusion across silhouettes — the classic halo — because it averages
 * AO from surfaces that are metres apart in world space. Weighting by
 * depth similarity keeps the AO edge on the geometry edge. */
"  var sum = 0.0;\n"
"  var wsum = 0.0;\n"
/* 3x3 in AO space, not 2x2. AO is low-frequency but the half-res horizon
 * search is noisy, and a 2x2 gather leaves a visible stipple that TAA only
 * partly resolves — a wider depth-weighted kernel is the cheapest way to
 * spend samples on this signal. */
"  for (var dy = -1; dy <= 1; dy = dy + 1) {\n"
"    for (var dx = -1; dx <= 1; dx = dx + 1) {\n"
"      let q = clamp(half + vec2<i32>(dx, dy), vec2<i32>(0), aoDims - vec2<i32>(1));\n"
"      let a = textureLoad(aoTex, q, 0).r;\n"
"      let qd = textureLoad(depthTex, clamp(q * 2, vec2<i32>(0), vec2<i32>(textureDimensions(depthTex)) - vec2<i32>(1)), 0);\n"
"      let w = 1.0 / (1.0e-4 + abs(qd - centerDepth) * 400.0);\n"
"      sum = sum + a * w;\n"
"      wsum = wsum + w;\n"
"    }\n"
"  }\n"
"  let ao = select(1.0, sum / wsum, wsum > 0.0);\n"
/* Additive blend onto hdrColor, which holds direct+emissive only. */
"  return vec4<f32>(ambient * ao, 1.0);\n"
"}\n";

/* Debug view: the upsampled AO as greyscale, REPLACING the frame. */
static const char* G3D_AO_DEBUG_WGSL =
"@group(0) @binding(0) var aoTex: texture_2d<f32>;\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n"
"  var pts = array<vec2<f32>, 3>(\n"
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n"
"  return vec4<f32>(pts[vi], 0.0, 1.0);\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let ao = textureLoad(aoTex, vec2<i32>(frag.xy) / 2, 0).r;\n"
"  return vec4<f32>(ao, ao, ao, 1.0);\n"
"}\n";

static void g3d_configure_ssao(void) {
    static bool done = false;
    if (done) return;
    done = true;
    if (getenv("RAE_NO_SSAO")) g3d_ssao_enabled = false;
    /* Inspect the raw AO buffer. Essential when the effect is applied to the
     * ambient term alone: a correct-but-weak signal and a broken one look
     * almost identical in the composited image. */
    if (getenv("RAE_SSAO_DEBUG")) g3d_ao_debug = true;
    /* One switch picks a whole cost point rather than exposing slices and
     * steps separately: the two only make sense together. */
    const char* q = getenv("RAE_SSAO_QUALITY");
    if (q && q[0]) {
        if (strcmp(q, "mobile") == 0) { g3d_ssao_slices = 2; g3d_ssao_steps = 4; }
        else if (strcmp(q, "low") == 0) { g3d_ssao_slices = 2; g3d_ssao_steps = 6; }
        else if (strcmp(q, "high") == 0) { g3d_ssao_slices = 4; g3d_ssao_steps = 8; }
        else if (strcmp(q, "desktop") == 0) { g3d_ssao_slices = 3; g3d_ssao_steps = 6; }
        else fprintf(stderr, "[gpu3d] WARNING: unknown RAE_SSAO_QUALITY='%s'; "
                             "expected mobile|low|desktop|high\n", q);
    }
    const char* r = getenv("RAE_SSAO_RADIUS");
    if (r && r[0]) {
        float v = (float)atof(r);
        if (v > 0.0f) g3d_ssao_radius = v;
    }
}

static void g3d_ssao_ensure_targets(void) {
    int w = g3d_target_w / 2, h = g3d_target_h / 2;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g3d_ao_view && w == g3d_ao_w && h == g3d_ao_h) return;
    if (g3d_ssao_bind) { wgpuBindGroupRelease(g3d_ssao_bind); g3d_ssao_bind = NULL; }
    if (g3d_ao_apply_bind) { wgpuBindGroupRelease(g3d_ao_apply_bind); g3d_ao_apply_bind = NULL; }
    if (g3d_ao_debug_bind) { wgpuBindGroupRelease(g3d_ao_debug_bind); g3d_ao_debug_bind = NULL; }
    if (g3d_ao_view) { wgpuTextureViewRelease(g3d_ao_view); g3d_ao_view = NULL; }
    if (g3d_ao_tex) { wgpuTextureRelease(g3d_ao_tex); g3d_ao_tex = NULL; }
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h; td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = 1; td.sampleCount = 1;
    td.format = WGPUTextureFormat_R8Unorm;   /* AO is one scalar in [0,1] */
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    g3d_ao_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    g3d_ao_view = wgpuTextureCreateView(g3d_ao_tex, NULL);
    g3d_ao_w = w; g3d_ao_h = h;
}

static WGPURenderPipeline g3d_make_fullscreen_pipeline(const char* wgsl, WGPUTextureFormat fmt,
                                                      bool additive, const char* label) {
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(wgsl);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);
    WGPUBlendState blend; memset(&blend, 0, sizeof(blend));
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = WGPUBlendFactor_One;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_One;
    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = fmt; cts.writeMask = WGPUColorWriteMask_All;
    if (additive) cts.blend = &blend;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;
    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    WGPURenderPipeline pl = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!pl) fprintf(stderr, "[gpu3d] %s pipeline creation FAILED\n", label);
    return pl;
}

static void g3d_ssao_init(void) {
    g3d_configure_ssao();
    if (!g3d_ssao_pipeline) {
        g3d_ssao_pipeline = g3d_make_fullscreen_pipeline(G3D_SSAO_WGSL, WGPUTextureFormat_R8Unorm, false, "SSAO");
        WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
        bd.size = G3D_SSAO_PARAM_BYTES;
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        g3d_ssao_param_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        WGPUSamplerDescriptor sd; memset(&sd, 0, sizeof(sd));
        sd.addressModeU = WGPUAddressMode_ClampToEdge;
        sd.addressModeV = WGPUAddressMode_ClampToEdge;
        sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = WGPUFilterMode_Linear;
        sd.minFilter = WGPUFilterMode_Linear;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.maxAnisotropy = 1;
        g3d_ssao_sampler = wgpuDeviceCreateSampler(g_wgpu_dev, &sd);
    }
    /* The apply pass blends ADDITIVELY into hdrColor, which holds
     * direct+emissive: ambient*ao is added back rather than the whole image
     * being rewritten, so nothing else has to be re-read. */
    if (!g3d_ao_apply_pipeline) {
        g3d_ao_apply_pipeline = g3d_make_fullscreen_pipeline(G3D_AO_APPLY_WGSL, WGPUTextureFormat_RGBA16Float, true, "AO apply");
    }
    if (g3d_ao_debug && !g3d_ao_debug_pipeline) {
        g3d_ao_debug_pipeline = g3d_make_fullscreen_pipeline(G3D_AO_DEBUG_WGSL, WGPUTextureFormat_RGBA16Float, false, "AO debug");
    }
}

static void g3d_ssao_ensure_binds(void) {
    if (!g3d_ssao_pipeline || !g3d_ao_view || !g3d_depth_view || !g3d_normal_view) return;
    if (!g3d_ssao_bind) {
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g3d_ssao_pipeline, 0);
        WGPUBindGroupEntry e[3]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = g3d_ssao_param_ubuf; e[0].size = G3D_SSAO_PARAM_BYTES;
        e[1].binding = 1; e[1].textureView = g3d_depth_view;
        e[2].binding = 2; e[2].textureView = g3d_normal_view;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 3; bgd.entries = e;
        g3d_ssao_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        wgpuBindGroupLayoutRelease(bgl);
    }
    if (g3d_ao_debug_pipeline && !g3d_ao_debug_bind && g3d_ambient_view) {
        /* A pipeline created with an auto layout owns that layout exclusively,
         * so the debug pass cannot borrow the apply pass's bind group even
         * though the bindings are identical. */
        /* An auto layout only contains bindings the shader actually
         * REFERENCES, and the debug shader reads aoTex alone — so binding the
         * other two would fail validation on the count. */
        WGPUBindGroupLayout bgl3 = wgpuRenderPipelineGetBindGroupLayout(g3d_ao_debug_pipeline, 0);
        WGPUBindGroupEntry e3[1]; memset(e3, 0, sizeof(e3));
        e3[0].binding = 0; e3[0].textureView = g3d_ao_view;
        WGPUBindGroupDescriptor bgd3; memset(&bgd3, 0, sizeof(bgd3));
        bgd3.layout = bgl3; bgd3.entryCount = 1; bgd3.entries = e3;
        g3d_ao_debug_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd3);
        wgpuBindGroupLayoutRelease(bgl3);
    }
    if (!g3d_ao_apply_bind && g3d_ao_apply_pipeline && g3d_ambient_view) {
        WGPUBindGroupLayout bgl2 = wgpuRenderPipelineGetBindGroupLayout(g3d_ao_apply_pipeline, 0);
        WGPUBindGroupEntry e2[3]; memset(e2, 0, sizeof(e2));
        e2[0].binding = 0; e2[0].textureView = g3d_ao_view;
        e2[1].binding = 1; e2[1].textureView = g3d_ambient_view;
        e2[2].binding = 2; e2[2].textureView = g3d_depth_view;
        WGPUBindGroupDescriptor bgd2; memset(&bgd2, 0, sizeof(bgd2));
        bgd2.layout = bgl2; bgd2.entryCount = 3; bgd2.entries = e2;
        g3d_ao_apply_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd2);
        wgpuBindGroupLayoutRelease(bgl2);
    }
}

static void g3d_run_fullscreen(WGPURenderPipeline pl, WGPUBindGroup bind,
                               WGPUTextureView target, WGPULoadOp load) {
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = target;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = load;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, pl);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
}

/* Clear the AO buffer to fully unoccluded. Used when AO generation is off,
 * so the composite still runs and ambient is added unattenuated. */
static void g3d_clear_ao_to_unoccluded(void) {
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = g3d_ao_view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue.r = 1.0; ca.clearValue.g = 1.0; ca.clearValue.b = 1.0; ca.clearValue.a = 1.0;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
}

/* Generate AO, then composite ambient*AO into hdrColor. Ends the scene pass
 * first so depth/normal/ambient are complete. Runs BEFORE TAA and tonemap:
 * AO belongs on linear radiance, and letting TAA see the result is what
 * makes the half-res noise resolve over a few frames. */
void rae_ext_gpu3d_ssao(void) {
    if (!g3d_ssao_pending) return;
    rae_g3d_finish_pass();
    g3d_ssao_init();
    g3d_ssao_ensure_targets();
    g3d_ssao_ensure_binds();
    if (!g3d_ao_apply_pipeline || !g3d_ao_apply_bind) return;
    if (g3d_ssao_enabled && (!g3d_ssao_pipeline || !g3d_ssao_bind)) return;
    g3d_ssao_pending = false;

    /* invViewProj and camPos are already computed for the frame uniform;
     * reuse them rather than inverting the matrix a second time. */
    float u[24]; memset(u, 0, sizeof(u));
    memcpy(u, g3d_frame_inv_viewproj, 16 * sizeof(float));
    u[16] = g3d_frame_campos[0];
    u[17] = g3d_frame_campos[1];
    u[18] = g3d_frame_campos[2];
    /* Frame index advances the noise so TAA integrates different sample
     * sets instead of accumulating one fixed pattern. */
    u[19] = (float)(g3d_taa_frame % 64);
    u[20] = g3d_ssao_radius;
    u[21] = g3d_ssao_intensity;
    u[22] = (float)g3d_ssao_slices;
    u[23] = (float)g3d_ssao_steps;
    wgpuQueueWriteBuffer(g_wgpu_queue, g3d_ssao_param_ubuf, 0, u, sizeof(u));

    /* The COMPOSITE is not optional: hdrColor holds direct+emissive only, so
     * the indirect term has to be added back every frame or every surface
     * facing away from the sun goes black. Only GENERATION is switchable —
     * with AO off the buffer is cleared to 1 and the composite adds plain
     * ambient, which also makes RAE_NO_SSAO an honest A/B rather than a
     * different lighting model. */
    if (g3d_ssao_enabled) {
        g3d_run_fullscreen(g3d_ssao_pipeline, g3d_ssao_bind, g3d_ao_view, WGPULoadOp_Clear);
    } else {
        g3d_clear_ao_to_unoccluded();
    }
    if (g3d_ao_debug && g3d_ao_debug_pipeline && g3d_ao_debug_bind) {
        g3d_run_fullscreen(g3d_ao_debug_pipeline, g3d_ao_debug_bind, g3d_hdr_view, WGPULoadOp_Clear);
    } else {
        g3d_run_fullscreen(g3d_ao_apply_pipeline, g3d_ao_apply_bind, g3d_hdr_view, WGPULoadOp_Load);
    }
}

static void g3d_ssao_shutdown(void) {
    if (g3d_ssao_bind) { wgpuBindGroupRelease(g3d_ssao_bind); g3d_ssao_bind = NULL; }
    if (g3d_ao_apply_bind) { wgpuBindGroupRelease(g3d_ao_apply_bind); g3d_ao_apply_bind = NULL; }
    if (g3d_ao_debug_bind) { wgpuBindGroupRelease(g3d_ao_debug_bind); g3d_ao_debug_bind = NULL; }
    if (g3d_ao_view) { wgpuTextureViewRelease(g3d_ao_view); g3d_ao_view = NULL; }
    if (g3d_ao_tex) { wgpuTextureRelease(g3d_ao_tex); g3d_ao_tex = NULL; }
    if (g3d_ssao_sampler) { wgpuSamplerRelease(g3d_ssao_sampler); g3d_ssao_sampler = NULL; }
    if (g3d_ssao_param_ubuf) { wgpuBufferRelease(g3d_ssao_param_ubuf); g3d_ssao_param_ubuf = NULL; }
    if (g3d_ssao_pipeline) { wgpuRenderPipelineRelease(g3d_ssao_pipeline); g3d_ssao_pipeline = NULL; }
    if (g3d_ao_apply_pipeline) { wgpuRenderPipelineRelease(g3d_ao_apply_pipeline); g3d_ao_apply_pipeline = NULL; }
    if (g3d_ao_debug_pipeline) { wgpuRenderPipelineRelease(g3d_ao_debug_pipeline); g3d_ao_debug_pipeline = NULL; }
    g3d_ao_w = 0; g3d_ao_h = 0;
    g3d_ssao_pending = false;
}
