/* Image files decoded to RGBA8, in every build (lodepng and stb_image are in
 * runtime_image_sdl3.c): for Gpu2d's images and for the thumbnail threads
 * (lib/Thumbnails). Both decoders keep their state on the stack or per thread,
 * so any thread may decode. */

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

/* Does the JPEG's own picture end (EOI)? Its segments are stepped over by
 * their lengths, so an EXIF thumbnail's EOI does not count, and what a camera
 * appends after the picture (a motion photo's video) does not matter. */
static int rae_jpeg_is_whole(const unsigned char* b, size_t len) {
    size_t i = 2;
    while (i + 1 < len) {
        if (b[i] != 0xFF) { i++; continue; }          /* picture data */
        unsigned char m = b[i + 1];
        if (m == 0xD9) return 1;
        if (m == 0x00 || m == 0xFF || m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {
            i += (m == 0xFF) ? 1 : 2;                 /* stuffing, fill, no length */
            continue;
        }
        if (i + 3 >= len) return 0;
        i += 2 + (((size_t)b[i + 2] << 8) | b[i + 3]);
    }
    return 0;
}

/* Decode an image file to RGBA8 (#228; contract + rationale in
 * docs/image-decoding-design.md): strict magic-byte dispatch, ONE
 * decoder per format, no fallback cascade.
 *   FF D8 FF     -> vendored stb_image (JPEG, every platform)
 *   89 50 4E 47  -> lodepng (PNG)
 *   anything else -> unsupported
 * Output is straight-alpha RGBA8, no color management.
 *
 * stb is the sole JPEG decoder. macOS previously used ImageIO, but
 * ImageIO silently rendered truncated downloads half-gray (no error);
 * stb correctly rejects them, and decodes every valid file (verified
 * ok=102/102 on the cached Spotify artwork set). The truncation guard
 * below turns a partial download into a loud failure the caller can
 * evict + re-fetch, rather than a decoder-dependent glitch.
 * Returns 1 with a malloc-compatible *out_rgba on success; on
 * failure returns 0 and points *out_err at a static reason string.
 * `max_pixels` > 0 refuses a larger picture by its header, before any of it is
 * decoded. */
static int rae_g2d_decode_rgba_limited(const char* path, int64_t max_pixels,
                                       unsigned char** out_rgba,
                                       unsigned* out_w, unsigned* out_h,
                                       const char** out_err) {
    *out_rgba = NULL; *out_w = 0; *out_h = 0; *out_err = "unreadable file";
    size_t len = 0;
    unsigned char* bytes = rae_g2d_read_whole_file(path, &len);
    if (!bytes) return 0;
    if (max_pixels > 0 && len > (size_t)0x7fffffff) { free(bytes); *out_err = "file too large"; return 0; }
    if (len >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        /* Truncation guard: an interrupted download has no EOI marker after
         * its picture data (stb would show the rest gray). Fail loudly so
         * callers can evict the bad cache entry and re-fetch. */
        if (!rae_jpeg_is_whole(bytes, len)) {
            free(bytes);
            *out_err = "truncated JPEG (missing EOI marker)";
            return 0;
        }
        int w = 0, h = 0, comp = 0;
        if (max_pixels > 0 && (!stbi_info_from_memory(bytes, (int)len, &w, &h, &comp)
                               || (int64_t)w * (int64_t)h > max_pixels)) {
            free(bytes);
            *out_err = "too large, or not a JPEG";
            return 0;
        }
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
        if (max_pixels > 0) {
            LodePNGState state;
            unsigned w = 0, h = 0;
            lodepng_state_init(&state);
            unsigned bad = lodepng_inspect(&w, &h, &state, bytes, len);
            lodepng_state_cleanup(&state);
            if (bad || (int64_t)w * (int64_t)h > max_pixels) {
                free(bytes);
                *out_err = "too large, or not a PNG";
                return 0;
            }
        }
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

static inline int rae_g2d_decode_rgba(const char* path, unsigned char** out_rgba,
                                      unsigned* out_w, unsigned* out_h,
                                      const char** out_err) {
    return rae_g2d_decode_rgba_limited(path, 0, out_rgba, out_w, out_h, out_err);
}

/* lib/Thumbnails: the file at `path` as a malloc'd RGBA8 buffer, its size
 * through width and height; NULL (and 0, 0) when it is no PNG or JPEG, broken,
 * or of more than max_pixels pixels. Freed with rae_ext_Thumbnails_free. */
uint8_t* rae_ext_Thumbnails_decode(rae_String path, int64_t max_pixels, int64_t* width, int64_t* height) {
    unsigned char* rgba = NULL;
    unsigned w = 0, h = 0;
    const char* why = NULL;
    *width = 0;
    *height = 0;
    if (!path.data || !rae_g2d_decode_rgba_limited((const char*)path.data, max_pixels, &rgba, &w, &h, &why)) return NULL;
    *width = (int64_t)w;
    *height = (int64_t)h;
    return rgba;
}
void rae_ext_Thumbnails_free(uint8_t* rgba) { free(rgba); }
