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
/* The image WGSL is a Rae asset since #908: lib/gpu2d_image.wgsl. */

/* #908: image textures + views, the image pipeline, sampler, per-draw uniforms
 * and bind groups, the draw queue and the key registry are all Rae now
 * (lib/Gpu2dCanvas.rae over a manager). C keeps CPU decode only. */
static WGPUBuffer* g_g2d_frame_bufs = NULL;          /* transient per-flush buffers */
static int g_g2d_frame_buf_n = 0;
static int g_g2d_frame_buf_cap = 0;
static WGPUBindGroup* g_g2d_frame_binds = NULL;      /* transient per-flush bind groups */
static int g_g2d_frame_bind_n = 0;
static int g_g2d_frame_bind_cap = 0;
static WGPUBuffer* g_g2d_text_frame_bufs[RAE_SDF_MAX_ATLAS];
static int* g_g2d_text_frame_buf_cap[RAE_SDF_MAX_ATLAS];
static int g_g2d_text_frame_buf_n[RAE_SDF_MAX_ATLAS];
static int g_g2d_text_frame_buf_slots[RAE_SDF_MAX_ATLAS];
void rae_ext_Gpu2d_flush(void);

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
int64_t rae_ext_Gpu2d_decodeImageProbe(rae_String path) {
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

/* CPU decode for the Rae image pass (#908): decode `path` to a malloc'd RGBA8
 * buffer (NULL on failure, logged once); the size of the LAST successful decode
 * is read back through the width/height accessors, and the buffer is freed
 * with rae_g2d_decode_free once Rae uploaded it into a manager texture. */
static unsigned g_g2d_decoded_w = 0, g_g2d_decoded_h = 0;
void* rae_g2d_decode_image(rae_String path) {
    if (!path.data) return NULL;
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    const char* cpath = (const char*)path.data;
    const char* why = "decode failed";
    if (!rae_g2d_decode_rgba(cpath, &rgba, &uw, &uh, &why)) {
        fprintf(stderr, "[gpu2d] image decode failed (%s): %s\n", cpath, why);
        return NULL;
    }
    g_g2d_decoded_w = uw; g_g2d_decoded_h = uh;
    return (void*)rgba;
}
int64_t rae_g2d_decoded_width(void)  { return (int64_t)g_g2d_decoded_w; }
int64_t rae_g2d_decoded_height(void) { return (int64_t)g_g2d_decoded_h; }
void rae_g2d_decode_free(void* rgba) { free(rgba); }

