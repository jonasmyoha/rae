/* gpu2d MSDF text pipeline and glyph queue. Raw atlas upload/pipeline calls stay C; text layout and style policy belongs in Rae.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* --- Text pipeline (#111): MSDF glyph quads --------------------------
 * A second instanced pipeline that samples the MSDF atlas (the same raw
 * RGBA the CPU blit path holds in g_sdf_atlas[]) and antialiases with the
 * median-of-3 + screen-px-range trick. Shares the viewport uniform and
 * premultiplied blend with the box pipeline, so glyphs composite in the
 * same pass after boxes. Instance = 4*vec4: rect, uv (normalised), colour,
 * params(pxRange). One atlas/font per frame (the common UI case). */
#define G2D_TEXT_FLOATS 20

static const char* G2D_TEXT_WGSL =
"struct Glyph {\n"
"  rect: vec4<f32>,\n"
"  uv: vec4<f32>,\n"
"  color: vec4<f32>,\n"
"  params: vec4<f32>,\n"          /* x=pxRange, y=outlineWidth(px), z=softness(px) */
"  outline: vec4<f32>,\n"         /* straight outline colour */
"};\n"
"@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;\n"
"@group(0) @binding(1) var<storage, read> glyphs: array<Glyph>;\n"
"@group(0) @binding(2) var atlasTex: texture_2d<f32>;\n"
"@group(0) @binding(3) var atlasSamp: sampler;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) uv: vec2<f32>,\n"
"  @location(1) @interpolate(flat) inst: u32,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {\n"
"  var corners = array<vec2<f32>, 6>(\n"
"    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),\n"
"    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));\n"
"  let c = corners[vi];\n"
"  let g = glyphs[ii];\n"
"  let phys = uXform[0].xy;\n"
"  let posPx = (g.rect.xy + c * g.rect.zw) * uXform[0].zw + uXform[1].xy;\n"
"  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0,\n"
"                      1.0 - posPx.y / phys.y * 2.0);\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(ndc, 0.0, 1.0);\n"
"  o.uv = g.uv.xy + c * g.uv.zw;\n"
"  o.inst = ii;\n"
"  return o;\n"
"}\n"
"fn median3(r: f32, g: f32, b: f32) -> f32 {\n"
"  return max(min(r, g), min(max(r, g), b));\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @location(0) vec4<f32> {\n"
"  let g = glyphs[in.inst];\n"
"  let s = textureSample(atlasTex, atlasSamp, in.uv);\n"
/* signed distance from the glyph edge, in physical px (design pxRange * avg
 * scale). Positive inside the glyph. */
"  let sc = (uXform[0].z + uXform[0].w) * 0.5;\n"
"  let sd = g.params.x * sc * (median3(s.r, s.g, s.b) - 0.5);\n"
/* softness widens the coverage falloff (px) — 1 = crisp AA, larger = a soft
 * blurred edge (used for soft drop-shadows). */
"  let sw = max(g.params.z, 1.0);\n"
"  let bodyCov = clamp(sd / sw + 0.5, 0.0, 1.0);\n"
"  let ow = g.params.y;\n"
"  if (ow <= 0.0) {\n"
"    let a = g.color.a * bodyCov;\n"
"    return vec4<f32>(g.color.rgb * a, a);\n"   /* premultiplied */
"  }\n"
/* Outline = the glyph dilated by `ow` px; composite body OVER outline,
 * both premultiplied. */
"  let outerCov = clamp((sd + ow) / sw + 0.5, 0.0, 1.0);\n"
"  let ba = g.color.a * bodyCov;\n"
"  let oa = g.outline.a * outerCov;\n"
"  let outA = ba + oa * (1.0 - ba);\n"
"  let outRGB = g.color.rgb * ba + g.outline.rgb * oa * (1.0 - ba);\n"
"  return vec4<f32>(outRGB, outA);\n"           /* premultiplied */
"}\n";

static WGPURenderPipeline g_g2d_text_pipeline = NULL;
/* Text glyphs accumulate PER ATLAS so a single frame can mix multiple MSDF
 * fonts (e.g. Roboto body text + the Material-icon atlas) — endFrame emits one
 * text draw per atlas that has glyphs this frame. Indexed by atlas handle-1. */
static WGPUBuffer    g_g2d_text_instbuf[RAE_SDF_MAX_ATLAS];
static WGPUBindGroup g_g2d_text_bind[RAE_SDF_MAX_ATLAS];
static int   g_g2d_text_cap[RAE_SDF_MAX_ATLAS];   /* glyph capacity of instbuf[i] */
static float* g_g2d_text_prims[RAE_SDF_MAX_ATLAS]; /* CPU float accumulation */
static int   g_g2d_text_count[RAE_SDF_MAX_ATLAS];  /* glyphs this frame */
static int   g_g2d_text_capf[RAE_SDF_MAX_ATLAS];   /* float capacity */
static WGPUSampler g_g2d_sampler = NULL;
static WGPUTexture     g_g2d_atlas_tex[RAE_SDF_MAX_ATLAS];
static WGPUTextureView g_g2d_atlas_view[RAE_SDF_MAX_ATLAS];

/* Lazily upload atlas `handle` (1-based, from sdf_text.loadAtlas) as a wgpu
 * texture; cached. Returns its view, or NULL if the atlas isn't loaded. */
static WGPUTextureView rae_g2d_atlas_texview(int handle) {
    int i = handle - 1;
    if (i < 0 || i >= RAE_SDF_MAX_ATLAS || !g_sdf_atlas[i]) return NULL;
    if (g_g2d_atlas_view[i]) return g_g2d_atlas_view[i];
    int aw = g_sdf_atlas_w[i], ah = g_sdf_atlas_h[i];
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)aw; td.size.height = (uint32_t)ah; td.size.depthOrArrayLayers = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;   /* MSDF data is linear, not sRGB */
    td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    WGPUTexelCopyTextureInfo dst; memset(&dst, 0, sizeof(dst));
    dst.texture = tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout; memset(&layout, 0, sizeof(layout));
    layout.bytesPerRow = (uint32_t)(aw * 4); layout.rowsPerImage = (uint32_t)ah;
    WGPUExtent3D ext; ext.width = (uint32_t)aw; ext.height = (uint32_t)ah; ext.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(g_wgpu_queue, &dst, g_sdf_atlas[i], (size_t)aw * ah * 4, &layout, &ext);
    g_g2d_atlas_tex[i] = tex;
    g_g2d_atlas_view[i] = wgpuTextureCreateView(tex, NULL);
    return g_g2d_atlas_view[i];
}

static void rae_g2d_init_text_pipeline(void) {
    if (g_g2d_text_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G2D_TEXT_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUBlendState blend; memset(&blend, 0, sizeof(blend));
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.blend = &blend; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g_g2d_text_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);

    if (!g_g2d_sampler) {
        WGPUSamplerDescriptor sd; memset(&sd, 0, sizeof(sd));
        sd.addressModeU = WGPUAddressMode_ClampToEdge;
        sd.addressModeV = WGPUAddressMode_ClampToEdge;
        sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = WGPUFilterMode_Linear;   /* MSDF requires bilinear */
        sd.minFilter = WGPUFilterMode_Linear;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.lodMaxClamp = 1.0f; sd.maxAnisotropy = 1;
        g_g2d_sampler = wgpuDeviceCreateSampler(g_wgpu_dev, &sd);
    }
}

static void rae_g2d_rebuild_text_bind(int ai) {
    WGPUTextureView view = rae_g2d_atlas_texview(ai + 1);
    if (!view) return;
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_text_pipeline, 0);
    WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
    e[1].binding = 1; e[1].buffer = g_g2d_text_instbuf[ai];
    e[1].size = (uint64_t)g_g2d_text_cap[ai] * G2D_TEXT_FLOATS * sizeof(float);
    e[2].binding = 2; e[2].textureView = view;
    e[3].binding = 3; e[3].sampler = g_g2d_sampler;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
    if (g_g2d_text_bind[ai]) wgpuBindGroupRelease(g_g2d_text_bind[ai]);
    g_g2d_text_bind[ai] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

static void rae_g2d_ensure_text_inst(int ai, int prims) {
    if (g_g2d_text_instbuf[ai] && prims <= g_g2d_text_cap[ai]) return;
    int cap = g_g2d_text_cap[ai] ? g_g2d_text_cap[ai] : 256;
    while (cap < prims) cap *= 2;
    if (g_g2d_text_instbuf[ai]) wgpuBufferRelease(g_g2d_text_instbuf[ai]);
    if (g_g2d_text_bind[ai]) { wgpuBindGroupRelease(g_g2d_text_bind[ai]); g_g2d_text_bind[ai] = NULL; }
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = (uint64_t)cap * G2D_TEXT_FLOATS * sizeof(float);
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g_g2d_text_instbuf[ai] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    g_g2d_text_cap[ai] = cap;
}

/* Full glyph submit with an optional outline (outlineWidth px + colour) and
 * softness (edge-falloff width in px; 1 = crisp, larger = soft/blurred). */
void rae_ext_gpu2d_drawGlyphEx(double sx0, double sy0, double sx1, double sy1,
                               double u0, double v0, double u1, double v1,
                               int64_t atlas, double pxRange, int64_t color,
                               double outlineWidth, int64_t outlineColor, double softness) {
    int ai = (int)atlas - 1;
    if (ai < 0 || ai >= RAE_SDF_MAX_ATLAS) return;
    int need = (g_g2d_text_count[ai] + 1) * G2D_TEXT_FLOATS;
    if (need > g_g2d_text_capf[ai]) {
        int cap = g_g2d_text_capf[ai] ? g_g2d_text_capf[ai] : (256 * G2D_TEXT_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_text_prims[ai] = (float*)realloc(g_g2d_text_prims[ai], (size_t)cap * sizeof(float));
        g_g2d_text_capf[ai] = cap;
    }
    float* p = g_g2d_text_prims[ai] + g_g2d_text_count[ai] * G2D_TEXT_FLOATS;
    p[0]=(float)sx0; p[1]=(float)sy0; p[2]=(float)(sx1-sx0); p[3]=(float)(sy1-sy0);
    p[4]=(float)u0; p[5]=(float)v0; p[6]=(float)(u1-u0); p[7]=(float)(v1-v0);
    /* straight (non-premultiplied) colour 0xAARRGGBB; the shader premultiplies */
    uint32_t c = (uint32_t)color;
    p[8]  = (float)((c >> 16) & 0xFF) / 255.0f;
    p[9]  = (float)((c >> 8)  & 0xFF) / 255.0f;
    p[10] = (float)( c        & 0xFF) / 255.0f;
    p[11] = (float)((c >> 24) & 0xFF) / 255.0f;
    p[12]=(float)pxRange; p[13]=(float)outlineWidth; p[14]=(float)softness; p[15]=0.0f;
    uint32_t oc = (uint32_t)outlineColor;
    p[16] = (float)((oc >> 16) & 0xFF) / 255.0f;
    p[17] = (float)((oc >> 8)  & 0xFF) / 255.0f;
    p[18] = (float)( oc        & 0xFF) / 255.0f;
    p[19] = (float)((oc >> 24) & 0xFF) / 255.0f;
    rae_g2d_text_clip_ensure(ai, g_g2d_text_count[ai] + 1);
    g_g2d_text_clip[ai][g_g2d_text_count[ai]] = g_g2d_cur_clip;
    g_g2d_text_count[ai]++;
}

/* Back-compat: glyph with no outline. */
void rae_ext_gpu2d_drawGlyph(double sx0, double sy0, double sx1, double sy1,
                             double u0, double v0, double u1, double v1,
                             int64_t atlas, double pxRange, int64_t color) {
    rae_ext_gpu2d_drawGlyphEx(sx0, sy0, sx1, sy1, u0, v0, u1, v1, atlas, pxRange, color, 0.0, 0, 1.0);
}

