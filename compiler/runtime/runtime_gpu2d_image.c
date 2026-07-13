/* gpu2d image decode/upload, image-key registry, and image draw queue. Decode/upload stay C; registry/cache policy is a Rae migration candidate.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* --- Image pipeline (#143): textured rounded quads -------------------
 * A third pipeline that samples a per-image RGBA texture with a tint
 * multiply and the same rounded-rect SDF mask the box pipeline uses, so
 * album covers and (white-on-alpha) Material-style icons render on the
 * GPU. Unlike box/text (one instanced draw), each image samples its OWN
 * texture, so images draw one-per-call with a per-draw uniform + bind
 * group. The image count per frame is tiny (a cover + a few icons), so
 * the per-draw bind group is cheap. Drawn after boxes, before text. */
static const char* G2D_IMG_WGSL =
"@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;\n"
"@group(0) @binding(1) var<uniform> uImg: array<vec4<f32>, 4>;\n"  /* rect, tint, params(radius), uv */
"@group(0) @binding(2) var tex: texture_2d<f32>;\n"
"@group(0) @binding(3) var samp: sampler;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) uv: vec2<f32>,\n"
"  @location(1) local: vec2<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> VsOut {\n"
"  var corners = array<vec2<f32>, 6>(\n"
"    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),\n"
"    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));\n"
"  let c = corners[vi];\n"
"  let rect = uImg[0];\n"
"  let phys = uXform[0].xy;\n"
"  let posPx = (rect.xy + c * rect.zw) * uXform[0].zw + uXform[1].xy;\n"
"  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0, 1.0 - posPx.y / phys.y * 2.0);\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(ndc, 0.0, 1.0);\n"
"  let uv = uImg[3];\n"
"  o.uv = uv.xy + c * uv.zw;\n"
"  o.local = c * rect.zw;\n"
"  return o;\n"
"}\n"
"fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {\n"
"  let q = abs(p) - b + vec2<f32>(r, r);\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @location(0) vec4<f32> {\n"
"  let texel = textureSample(tex, samp, in.uv);\n"
"  let tint = uImg[1];\n"
"  let half = uImg[0].zw * 0.5;\n"
"  let rad = uImg[2].x;\n"
"  let d = sdRoundBox(in.local - half, half, rad);\n"
"  let aa = max(fwidth(d), 0.0001);\n"
"  let cov = 1.0 - smoothstep(-aa, aa, d);\n"
"  let a = texel.a * tint.a * cov;\n"
"  return vec4<f32>(texel.rgb * tint.rgb * a, a);\n"  /* premultiplied */
"}\n";

#define RAE_G2D_MAX_IMG 128
static WGPUTexture     g_g2d_img_tex[RAE_G2D_MAX_IMG];
static WGPUTextureView g_g2d_img_view[RAE_G2D_MAX_IMG];
static int g_g2d_img_w[RAE_G2D_MAX_IMG];
static int g_g2d_img_h[RAE_G2D_MAX_IMG];
static int g_g2d_img_n = 0;

typedef struct { int handle; float rect[4]; float tint[4]; float radius; float uv[4]; int clip; } RaeG2dImgCmd;
static RaeG2dImgCmd* g_g2d_img_cmds = NULL;
static int g_g2d_img_cmd_count = 0;
static int g_g2d_img_cmd_cap = 0;

static WGPURenderPipeline g_g2d_img_pipeline = NULL;
static WGPUBuffer* g_g2d_img_ubuf = NULL;   /* pool of 48-byte per-draw uniforms */
static int g_g2d_img_ubuf_n = 0;
static WGPUBindGroup* g_g2d_img_frame_binds = NULL;  /* cached per draw slot */
static int* g_g2d_img_frame_handles = NULL;           /* texture handle bound in each slot */
static int g_g2d_img_frame_bind_n = 0;
static int g_g2d_img_frame_bind_cap = 0;
static WGPUBuffer* g_g2d_frame_bufs = NULL;          /* transient per-flush buffers */
static int g_g2d_frame_buf_n = 0;
static int g_g2d_frame_buf_cap = 0;
static WGPUBindGroup* g_g2d_frame_binds = NULL;      /* transient per-flush bind groups */
static int g_g2d_frame_bind_n = 0;
static int g_g2d_frame_bind_cap = 0;
static WGPUBuffer* g_g2d_box_frame_bufs = NULL;      /* persistent per-flush storage buffers */
static int* g_g2d_box_frame_buf_cap = NULL;          /* capacity in primitives */
static int g_g2d_box_frame_buf_n = 0;
static int g_g2d_box_frame_buf_slots = 0;
static WGPUBuffer* g_g2d_text_frame_bufs[RAE_SDF_MAX_ATLAS];
static int* g_g2d_text_frame_buf_cap[RAE_SDF_MAX_ATLAS];
static int g_g2d_text_frame_buf_n[RAE_SDF_MAX_ATLAS];
static int g_g2d_text_frame_buf_slots[RAE_SDF_MAX_ATLAS];
void rae_ext_gpu2d_flush(void);

static void rae_g2d_keep_frame_buf(WGPUBuffer b) {
    if (!b) return;
    if (g_g2d_frame_buf_n + 1 > g_g2d_frame_buf_cap) {
        int cap = g_g2d_frame_buf_cap ? g_g2d_frame_buf_cap * 2 : 64;
        g_g2d_frame_bufs = (WGPUBuffer*)realloc(g_g2d_frame_bufs, (size_t)cap * sizeof(WGPUBuffer));
        g_g2d_frame_buf_cap = cap;
    }
    g_g2d_frame_bufs[g_g2d_frame_buf_n++] = b;
}

static void rae_g2d_keep_frame_bind(WGPUBindGroup b) {
    if (!b) return;
    if (g_g2d_frame_bind_n + 1 > g_g2d_frame_bind_cap) {
        int cap = g_g2d_frame_bind_cap ? g_g2d_frame_bind_cap * 2 : 64;
        g_g2d_frame_binds = (WGPUBindGroup*)realloc(g_g2d_frame_binds, (size_t)cap * sizeof(WGPUBindGroup));
        g_g2d_frame_bind_cap = cap;
    }
    g_g2d_frame_binds[g_g2d_frame_bind_n++] = b;
}

static WGPUBuffer rae_g2d_box_frame_buffer(int prims) {
    int slot = g_g2d_box_frame_buf_n++;
    if (slot >= g_g2d_box_frame_buf_slots) {
        int old = g_g2d_box_frame_buf_slots;
        int cap = old ? old * 2 : 64;
        while (cap <= slot) cap *= 2;
        g_g2d_box_frame_bufs = (WGPUBuffer*)realloc(g_g2d_box_frame_bufs, (size_t)cap * sizeof(WGPUBuffer));
        g_g2d_box_frame_buf_cap = (int*)realloc(g_g2d_box_frame_buf_cap, (size_t)cap * sizeof(int));
        for (int i = old; i < cap; i++) { g_g2d_box_frame_bufs[i] = NULL; g_g2d_box_frame_buf_cap[i] = 0; }
        g_g2d_box_frame_buf_slots = cap;
    }
    if (!g_g2d_box_frame_bufs[slot] || g_g2d_box_frame_buf_cap[slot] < prims) {
        if (g_g2d_box_frame_bufs[slot]) wgpuBufferRelease(g_g2d_box_frame_bufs[slot]);
        int cap = g_g2d_box_frame_buf_cap[slot] ? g_g2d_box_frame_buf_cap[slot] : 4;
        while (cap < prims) cap *= 2;
        WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
        bd.size = (uint64_t)cap * G2D_PRIM_FLOATS * sizeof(float);
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        g_g2d_box_frame_bufs[slot] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        g_g2d_box_frame_buf_cap[slot] = cap;
    }
    return g_g2d_box_frame_bufs[slot];
}

static WGPUBuffer rae_g2d_text_frame_buffer(int ai, int glyphs) {
    int slot = g_g2d_text_frame_buf_n[ai]++;
    if (slot >= g_g2d_text_frame_buf_slots[ai]) {
        int old = g_g2d_text_frame_buf_slots[ai];
        int cap = old ? old * 2 : 64;
        while (cap <= slot) cap *= 2;
        g_g2d_text_frame_bufs[ai] = (WGPUBuffer*)realloc(g_g2d_text_frame_bufs[ai], (size_t)cap * sizeof(WGPUBuffer));
        g_g2d_text_frame_buf_cap[ai] = (int*)realloc(g_g2d_text_frame_buf_cap[ai], (size_t)cap * sizeof(int));
        for (int i = old; i < cap; i++) { g_g2d_text_frame_bufs[ai][i] = NULL; g_g2d_text_frame_buf_cap[ai][i] = 0; }
        g_g2d_text_frame_buf_slots[ai] = cap;
    }
    if (!g_g2d_text_frame_bufs[ai][slot] || g_g2d_text_frame_buf_cap[ai][slot] < glyphs) {
        if (g_g2d_text_frame_bufs[ai][slot]) wgpuBufferRelease(g_g2d_text_frame_bufs[ai][slot]);
        int cap = g_g2d_text_frame_buf_cap[ai][slot] ? g_g2d_text_frame_buf_cap[ai][slot] : 16;
        while (cap < glyphs) cap *= 2;
        WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
        bd.size = (uint64_t)cap * G2D_TEXT_FLOATS * sizeof(float);
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        g_g2d_text_frame_bufs[ai][slot] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        g_g2d_text_frame_buf_cap[ai][slot] = cap;
    }
    return g_g2d_text_frame_bufs[ai][slot];
}

/* Read a whole file into a malloc'd buffer. Returns NULL on failure. */
static unsigned char* rae_g2d_read_whole_file(const char* path, size_t* out_len) {
    if (!path || !out_len) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

/* Decode an image file to RGBA8 (#228; contract + rationale in
 * docs/image-decoding-design.md): strict magic-byte dispatch, ONE
 * decoder per format, no fallback cascade.
 *   FF D8 FF     -> vendored stb_image (JPEG, every platform)
 *   89 50 4E 47  -> lodepng (PNG)
 *   anything else -> unsupported
 * Output is straight-alpha RGBA8, no colour management.
 *
 * stb is the sole JPEG decoder. macOS previously used ImageIO, but
 * ImageIO silently rendered truncated downloads half-grey (no error);
 * stb correctly rejects them, and decodes every valid file (verified
 * ok=102/102 on the cached Spotify artwork set). The truncation guard
 * below turns a partial download into a loud failure the caller can
 * evict + re-fetch, rather than a decoder-dependent glitch.
 * Returns 1 with a malloc-compatible *out_rgba on success; on
 * failure returns 0 and points *out_err at a static reason string. */
static int rae_g2d_decode_rgba(const char* path, unsigned char** out_rgba,
                               unsigned* out_w, unsigned* out_h,
                               const char** out_err) {
    *out_rgba = NULL; *out_w = 0; *out_h = 0; *out_err = "unreadable file";
    size_t len = 0;
    unsigned char* bytes = rae_g2d_read_whole_file(path, &len);
    if (!bytes) return 0;
    if (len >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        /* Truncation guard: a JPEG without its EOI marker (FF D9) in
         * the last 64 bytes is an interrupted download. Fail loudly
         * so callers can evict the bad cache entry and re-fetch. */
        {
            size_t scan = len < 64 ? len : 64;
            int has_eoi = 0;
            for (size_t i = len - scan; i + 1 < len; i++) {
                if (bytes[i] == 0xFF && bytes[i + 1] == 0xD9) { has_eoi = 1; break; }
            }
            if (!has_eoi) {
                free(bytes);
                *out_err = "truncated JPEG (missing EOI marker)";
                return 0;
            }
        }
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 4);
        free(bytes);
        if (!px) {
            *out_err = stbi_failure_reason();
            return 0;
        }
        *out_rgba = px; *out_w = (unsigned)w; *out_h = (unsigned)h;
        return 1;
    }
    if (len >= 4 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
        unsigned err = lodepng_decode32(out_rgba, out_w, out_h, bytes, len);
        free(bytes);
        if (err) {
            *out_err = lodepng_error_text(err);
            return 0;
        }
        return 1;
    }
    free(bytes);
    *out_err = "unsupported format (not JPEG/PNG)";
    return 0;
}

/* Device-free decode probe (#228): run the exact decode + error
 * policy of gpu2d.loadImage without needing a WebGPU device, so the
 * corrupt-file behaviour is testable in the headless suite. Returns
 * 1 when the file decodes, 0 (plus the standard stderr line) when it
 * doesn't. Also handy as a CLI-side asset validator. */
int64_t rae_ext_gpu2d_decodeImageProbe(rae_String path) {
    if (!path.data) return 0;
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    const char* why = "decode failed";
    const char* cpath = (const char*)path.data;
    if (!rae_g2d_decode_rgba(cpath, &rgba, &uw, &uh, &why)) {
        fprintf(stderr, "[gpu2d] image decode failed (%s): %s\n", cpath, why);
        return 0;
    }
    free(rgba);
    return 1;
}

/* Decode an image file and upload it as an RGBA8 texture. Decode policy
 * lives in rae_g2d_decode_rgba; a failure logs one line and returns
 * handle 0, which callers already render as their placeholder. */
int64_t rae_ext_gpu2d_loadImage(rae_String path) {
    if (!path.data || !g_wgpu_dev || g_g2d_img_n >= RAE_G2D_MAX_IMG) return 0;
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    const char* cpath = (const char*)path.data;
    const char* why = "decode failed";
    if (!rae_g2d_decode_rgba(cpath, &rgba, &uw, &uh, &why)) {
        fprintf(stderr, "[gpu2d] image decode failed (%s): %s\n", cpath, why);
        return 0;
    }
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = uw; td.size.height = uh; td.size.depthOrArrayLayers = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm; td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    WGPUTexelCopyTextureInfo dst; memset(&dst, 0, sizeof(dst));
    dst.texture = tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout; memset(&layout, 0, sizeof(layout));
    layout.bytesPerRow = uw * 4; layout.rowsPerImage = uh;
    WGPUExtent3D ext; ext.width = uw; ext.height = uh; ext.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(g_wgpu_queue, &dst, rgba, (size_t)uw * uh * 4, &layout, &ext);
    free(rgba);
    int i = g_g2d_img_n;
    g_g2d_img_tex[i] = tex;
    g_g2d_img_view[i] = wgpuTextureCreateView(tex, NULL);
    g_g2d_img_w[i] = (int)uw; g_g2d_img_h[i] = (int)uh;
    return (int64_t)(++g_g2d_img_n);   /* 1-based */
}

/* Name->handle registry, so a renderer can resolve a Sprite.textureKey to an
 * uploaded image without a Rae-side map (module-level heap globals miscompile).
 * The gpu2d UI backend loads album covers / icons by key and draws by key. */
#define RAE_G2D_MAX_IMG_KEYS 128
static char g_g2d_img_key[RAE_G2D_MAX_IMG_KEYS][96];
static int  g_g2d_img_key_handle[RAE_G2D_MAX_IMG_KEYS];
static int  g_g2d_img_key_n = 0;

void rae_ext_gpu2d_drawImage(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint);  /* defined below */

static int rae_g2d_handle_for_key(const char* k) {
    if (!k) return 0;
    for (int i = 0; i < g_g2d_img_key_n; i++)
        if (strcmp(g_g2d_img_key[i], k) == 0) return g_g2d_img_key_handle[i];
    return 0;
}

/* Decode+upload `path` and register it under `key` (returns the handle, 0 on
 * failure). Re-registering a key updates it. */
int64_t rae_ext_gpu2d_loadImageKey(rae_String key, rae_String path) {
    int64_t h = rae_ext_gpu2d_loadImage(path);
    if (h <= 0 || !key.data) return h;
    int slot = -1;
    for (int i = 0; i < g_g2d_img_key_n; i++)
        if (strcmp(g_g2d_img_key[i], (const char*)key.data) == 0) { slot = i; break; }
    if (slot < 0 && g_g2d_img_key_n < RAE_G2D_MAX_IMG_KEYS) slot = g_g2d_img_key_n++;
    if (slot >= 0) {
        strncpy(g_g2d_img_key[slot], (const char*)key.data, 95);
        g_g2d_img_key[slot][95] = '\0';
        g_g2d_img_key_handle[slot] = (int)h;
    }
    return h;
}

/* True if `key` resolves to a loaded image (so a renderer can fall back to a
 * placeholder / mat: glyph when it doesn't). */
rae_Bool rae_ext_gpu2d_hasImageKey(rae_String key) {
    return key.data && rae_g2d_handle_for_key((const char*)key.data) > 0;
}

/* Draw a registered image by key (no-op if the key isn't registered). */
void rae_ext_gpu2d_drawImageKey(rae_String key, double x, double y, double w, double h, double radius, int64_t tint) {
    if (!key.data) return;
    int handle = rae_g2d_handle_for_key((const char*)key.data);
    if (handle > 0) rae_ext_gpu2d_drawImage(x, y, w, h, radius, (int64_t)handle, tint);
}

static void rae_g2d_queue_image(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint,
                                float u0, float v0, float u1, float v1) {
    if (handle < 1 || handle > g_g2d_img_n) return;
    if (g_g2d_img_cmd_count + 1 > g_g2d_img_cmd_cap) {
        int cap = g_g2d_img_cmd_cap ? g_g2d_img_cmd_cap * 2 : 32;
        g_g2d_img_cmds = (RaeG2dImgCmd*)realloc(g_g2d_img_cmds, (size_t)cap * sizeof(RaeG2dImgCmd));
        g_g2d_img_cmd_cap = cap;
    }
    RaeG2dImgCmd* c = &g_g2d_img_cmds[g_g2d_img_cmd_count++];
    c->handle = (int)handle;
    c->rect[0] = (float)x; c->rect[1] = (float)y; c->rect[2] = (float)w; c->rect[3] = (float)h;
    uint32_t t = (uint32_t)tint;   /* straight (non-premultiplied); shader premultiplies */
    c->tint[0] = (float)((t >> 16) & 0xFF) / 255.0f;
    c->tint[1] = (float)((t >> 8)  & 0xFF) / 255.0f;
    c->tint[2] = (float)( t        & 0xFF) / 255.0f;
    c->tint[3] = (float)((t >> 24) & 0xFF) / 255.0f;
    c->radius = (float)radius;
    c->uv[0] = u0; c->uv[1] = v0; c->uv[2] = u1 - u0; c->uv[3] = v1 - v0;
    c->clip = g_g2d_cur_clip;
}

/* Queue an image draw for this frame (handle from loadImage). tint is
 * 0xAARRGGBB applied multiplicatively (use 0xFFFFFFFF for the unmodified
 * image). radius rounds the corners (design units). */
void rae_ext_gpu2d_drawImage(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint) {
    rae_g2d_queue_image(x, y, w, h, radius, handle, tint, 0.0f, 0.0f, 1.0f, 1.0f);
}

void rae_ext_gpu2d_drawImageKeyScaled(rae_String key, double x, double y, double w, double h, double radius, int64_t tint, int64_t scaleMode) {
    if (!key.data) return;
    int handle = rae_g2d_handle_for_key((const char*)key.data);
    if (handle <= 0 || handle > g_g2d_img_n || w <= 0.0 || h <= 0.0) return;
    int idx = handle - 1;
    double iw = (double)g_g2d_img_w[idx];
    double ih = (double)g_g2d_img_h[idx];
    if (iw <= 0.0 || ih <= 0.0) {
        rae_ext_gpu2d_drawImage(x, y, w, h, radius, (int64_t)handle, tint);
        return;
    }
    double img_aspect = iw / ih;
    double dst_aspect = w / h;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (scaleMode == 0) { /* fit: preserve aspect by letterboxing inside dst */
        if (img_aspect > dst_aspect) {
            double draw_h = w / img_aspect;
            y += (h - draw_h) * 0.5;
            h = draw_h;
        } else {
            double draw_w = h * img_aspect;
            x += (w - draw_w) * 0.5;
            w = draw_w;
        }
    } else if (scaleMode == 1) { /* fill: preserve aspect by center-cropping source */
        if (img_aspect > dst_aspect) {
            double visible_w = dst_aspect / img_aspect;
            u0 = (float)((1.0 - visible_w) * 0.5);
            u1 = (float)(u0 + visible_w);
        } else if (img_aspect < dst_aspect) {
            double visible_h = img_aspect / dst_aspect;
            v0 = (float)((1.0 - visible_h) * 0.5);
            v1 = (float)(v0 + visible_h);
        }
    }
    rae_g2d_queue_image(x, y, w, h, radius, (int64_t)handle, tint, u0, v0, u1, v1);
}

static void rae_g2d_init_img_pipeline(void) {
    if (g_g2d_img_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G2D_IMG_WGSL);
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
    g_g2d_img_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    /* Reuse the text sampler (linear, clamp-to-edge); create if text path
     * hasn't run yet. */
    if (!g_g2d_sampler) {
        WGPUSamplerDescriptor sd; memset(&sd, 0, sizeof(sd));
        sd.addressModeU = WGPUAddressMode_ClampToEdge;
        sd.addressModeV = WGPUAddressMode_ClampToEdge;
        sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = WGPUFilterMode_Linear;
        sd.minFilter = WGPUFilterMode_Linear;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.lodMaxClamp = 1.0f; sd.maxAnisotropy = 1;
        g_g2d_sampler = wgpuDeviceCreateSampler(g_wgpu_dev, &sd);
    }
}

/* Emit one draw per queued image into the active render pass. */
static void rae_g2d_flush_images(void) {
    if (g_g2d_img_cmd_count <= 0) return;
    rae_g2d_init_img_pipeline();
    /* Grow the per-draw uniform-buffer pool to cover this frame. */
    int total_img_uniforms = g_g2d_img_frame_bind_n + g_g2d_img_cmd_count;
    if (g_g2d_img_ubuf_n < total_img_uniforms) {
        g_g2d_img_ubuf = (WGPUBuffer*)realloc(g_g2d_img_ubuf, (size_t)total_img_uniforms * sizeof(WGPUBuffer));
        for (int i = g_g2d_img_ubuf_n; i < total_img_uniforms; i++) {
            WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
            bd.size = 64; bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            g_g2d_img_ubuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        }
        g_g2d_img_ubuf_n = total_img_uniforms;
    }
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_img_pipeline, 0);
    if (g_g2d_img_frame_bind_cap < total_img_uniforms) {
        int old_cap = g_g2d_img_frame_bind_cap;
        g_g2d_img_frame_binds = (WGPUBindGroup*)realloc(g_g2d_img_frame_binds, (size_t)total_img_uniforms * sizeof(WGPUBindGroup));
        g_g2d_img_frame_handles = (int*)realloc(g_g2d_img_frame_handles, (size_t)total_img_uniforms * sizeof(int));
        for (int i = old_cap; i < total_img_uniforms; i++) {
            g_g2d_img_frame_binds[i] = NULL;
            g_g2d_img_frame_handles[i] = 0;
        }
        g_g2d_img_frame_bind_cap = total_img_uniforms;
    }
    wgpuRenderPassEncoderSetPipeline(g_g2d_pass, g_g2d_img_pipeline);
    for (int i = 0; i < g_g2d_img_cmd_count; i++) {
        RaeG2dImgCmd* c = &g_g2d_img_cmds[i];
        int idx = c->handle - 1;
        if (idx < 0 || idx >= g_g2d_img_n || !g_g2d_img_view[idx]) continue;
        int uniform_slot = g_g2d_img_frame_bind_n;
        float u[16];
        u[0]=c->rect[0]; u[1]=c->rect[1]; u[2]=c->rect[2]; u[3]=c->rect[3];
        u[4]=c->tint[0]; u[5]=c->tint[1]; u[6]=c->tint[2]; u[7]=c->tint[3];
        u[8]=c->radius;  u[9]=0.0f; u[10]=0.0f; u[11]=0.0f;
        u[12]=c->uv[0];  u[13]=c->uv[1]; u[14]=c->uv[2]; u[15]=c->uv[3];
        wgpuQueueWriteBuffer(g_wgpu_queue, g_g2d_img_ubuf[uniform_slot], 0, u, sizeof(u));
        WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
        e[1].binding = 1; e[1].buffer = g_g2d_img_ubuf[uniform_slot]; e[1].size = 64;
        e[2].binding = 2; e[2].textureView = g_g2d_img_view[idx];
        e[3].binding = 3; e[3].sampler = g_g2d_sampler;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
        if (g_g2d_img_frame_binds[uniform_slot] == NULL ||
            g_g2d_img_frame_handles[uniform_slot] != c->handle) {
            if (g_g2d_img_frame_binds[uniform_slot]) wgpuBindGroupRelease(g_g2d_img_frame_binds[uniform_slot]);
            g_g2d_img_frame_binds[uniform_slot] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
            g_g2d_img_frame_handles[uniform_slot] = c->handle;
        }
        WGPUBindGroup bind = g_g2d_img_frame_binds[uniform_slot];
        g_g2d_img_frame_bind_n++;
        rae_g2d_set_scissor(c->clip);
        wgpuRenderPassEncoderSetBindGroup(g_g2d_pass, 0, bind, 0, NULL);
        wgpuRenderPassEncoderDraw(g_g2d_pass, 6, 1, 0, 0);
    }
    wgpuBindGroupLayoutRelease(bgl);
    g_g2d_img_cmd_count = 0;
}

