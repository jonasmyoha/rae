/* Image codec/reference helpers plus SDL3 software window/input/filesystem helpers. Raw codec/SDL calls stay C; policy above them can migrate to Rae.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* ============================================================
 * Rae Image API — PNG encode/decode behind a Rae-owned seam.
 *
 * Implementation today: lodepng (C, zlib-licensed, vendored as runtime/
 * lodepng.{c,h}; see lodepng.VENDOR.md). The seam is intentionally thin so the
 * backend is replaceable — the long-term intent is to own the DEFLATE codec +
 * PNG container in Rae (docs/png-and-deflate-strategy.md). Compiled into this
 * single translation unit, so every build path that compiles rae_runtime.c gets
 * it with no extra build-flag plumbing.
 * ============================================================ */
#include "lodepng.h"
#include "lodepng.c"

/* Vendored stb_image (see stb_image.VENDOR.md) — JPEG decode for
 * gpu2d artwork (#228; decision record docs/image-decoding-design.md).
 * Deliberately restricted: STBI_ONLY_JPEG keeps the attack surface to
 * one parser (PNG stays on lodepng above), STBI_NO_STDIO means stb
 * only ever parses bytes we read ourselves, and the dimension cap
 * matches the previous ImageIO limit. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC   /* raylib links its own stb_image; keep ours TU-local */
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

/* Save w*h*4 top-down RGBA8 bytes to `path` as a PNG. Returns 0 on success. */
static int rae_png_save_rgba32(const char* path, const unsigned char* rgba, int w, int h) {
    if (!path || !rgba || w <= 0 || h <= 0) return 1;
    unsigned err = lodepng_encode32_file(path, rgba, (unsigned)w, (unsigned)h);
    if (err) {
        fprintf(stderr, "[image] PNG encode failed: %s\n", lodepng_error_text(err));
        return (int)err;
    }
    return 0;
}

/* Rae Image API (lib/image.rae): encode a packed-0xRRGGBB Int framebuffer
 * (width*height entries, row-major top-down) to a PNG file. */
rae_Bool rae_ext_image_savePng(rae_String path, const int64_t* pixels, int64_t w, int64_t h) {
    if (!path.data || !pixels || w <= 0 || h <= 0) return false;
    size_t count = (size_t)w * (size_t)h;
    unsigned char* rgba = (unsigned char*)malloc(count * 4);
    if (!rgba) return false;
    for (size_t i = 0; i < count; i++) {
        int64_t p = pixels[i];
        rgba[i * 4 + 0] = (unsigned char)((p >> 16) & 0xFF);  /* R */
        rgba[i * 4 + 1] = (unsigned char)((p >> 8) & 0xFF);   /* G */
        rgba[i * 4 + 2] = (unsigned char)(p & 0xFF);          /* B */
        rgba[i * 4 + 3] = 255;                                /* A */
    }
    int rc = rae_png_save_rgba32((const char*)path.data, rgba, (int)w, (int)h);
    free(rgba);
    if (rc == 0) fprintf(stderr, "[image] saved %s (%lldx%lld)\n",
                         (const char*)path.data, (long long)w, (long long)h);
    return rc == 0;
}

/* Rae Image API (lib/image.rae): decode a PNG file into a packed-Int pixel
 * buffer (width*height entries, row-major top-down). Each Int is 0xAARRGGBB:
 * the low 24 bits are RGB (the same domain savePng consumes — feeding the result
 * straight back to savePng round-trips RGB exactly), the top byte is the alpha
 * channel (so RGBA assets like the MTSDF atlas survive the round trip).
 *
 * Returns a freshly rae_buf_alloc'd Buffer(Int) the caller owns; writes the
 * dimensions through the `mod Int` out-params. On failure returns NULL and sets
 * *w = *h = 0 (the caller checks width > 0). */
int64_t* rae_ext_image_loadPng(rae_String path, int64_t* w_out, int64_t* h_out) {
    if (w_out) *w_out = 0;
    if (h_out) *h_out = 0;
    if (!path.data) return NULL;
    unsigned char* rgba = NULL;
    unsigned uw = 0, uh = 0;
    unsigned err = lodepng_decode32_file(&rgba, &uw, &uh, (const char*)path.data);
    if (err) {
        fprintf(stderr, "[image] PNG decode failed: %s\n", lodepng_error_text(err));
        return NULL;
    }
    size_t count = (size_t)uw * (size_t)uh;
    int64_t* pixels = (int64_t*)rae_ext_rae_buf_alloc((int64_t)count, (int64_t)sizeof(int64_t));
    if (!pixels) { free(rgba); return NULL; }
    for (size_t i = 0; i < count; i++) {
        int64_t r = rgba[i * 4 + 0];
        int64_t g = rgba[i * 4 + 1];
        int64_t b = rgba[i * 4 + 2];
        int64_t a = rgba[i * 4 + 3];
        pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;   /* 0xAARRGGBB */
    }
    free(rgba);
    if (w_out) *w_out = (int64_t)uw;
    if (h_out) *h_out = (int64_t)uh;
    fprintf(stderr, "[image] loaded %s (%ux%u)\n", (const char*)path.data, uw, uh);
    return pixels;
}

/* ---- DEFLATE/zlib oracle (lib/compress/oracle.rae) -----------------------
 * lodepng's raw DEFLATE + zlib codecs, exposed so the pure-Rae codec can be
 * validated against a reference (round-trip + interop, per
 * docs/png-and-deflate-strategy.md). Bytes cross as Buffer(Int) (each 0..255);
 * the result is a fresh rae_buf_alloc'd Buffer(Int) the caller owns, with the
 * byte count written through `outLen`. Returns NULL on failure (outLen = 0). */
static int64_t* rae_compress_run(const int64_t* data, int64_t len, int64_t* out_len,
                                 int encode, int zlib) {
    if (out_len) *out_len = 0;
    if (!data || len < 0) return NULL;
    unsigned char* in = (unsigned char*)malloc((size_t)len > 0 ? (size_t)len : 1);
    if (!in) return NULL;
    for (int64_t i = 0; i < len; i++) in[i] = (unsigned char)(data[i] & 0xFF);
    unsigned char* out = NULL; size_t outsize = 0; unsigned err;
    if (encode) {
        LodePNGCompressSettings s = lodepng_default_compress_settings;
        err = zlib ? lodepng_zlib_compress(&out, &outsize, in, (size_t)len, &s)
                   : lodepng_deflate(&out, &outsize, in, (size_t)len, &s);
    } else {
        LodePNGDecompressSettings s = lodepng_default_decompress_settings;
        err = zlib ? lodepng_zlib_decompress(&out, &outsize, in, (size_t)len, &s)
                   : lodepng_inflate(&out, &outsize, in, (size_t)len, &s);
    }
    free(in);
    if (err) { free(out); fprintf(stderr, "[compress] lodepng error: %s\n", lodepng_error_text(err)); return NULL; }
    int64_t* result = (int64_t*)rae_ext_rae_buf_alloc((int64_t)outsize, (int64_t)sizeof(int64_t));
    if (!result) { free(out); return NULL; }
    for (size_t i = 0; i < outsize; i++) result[i] = out[i];
    free(out);
    if (out_len) *out_len = (int64_t)outsize;
    return result;
}
int64_t* rae_ext_compress_oracle_deflate(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 1, 0);
}
int64_t* rae_ext_compress_oracle_inflate(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 0, 0);
}
int64_t* rae_ext_compress_oracle_zlibCompress(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 1, 1);
}
int64_t* rae_ext_compress_oracle_zlibDecompress(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 0, 1);
}
/* Decode a PNG (held in a Buffer(Int) of bytes) to 0xAARRGGBB pixels via
 * lodepng — the oracle for testing the pure-Rae PNG decoder. */
int64_t* rae_ext_compress_oracle_decodePng(const int64_t* data, int64_t len, int64_t* w_out, int64_t* h_out) {
    if (w_out) *w_out = 0; if (h_out) *h_out = 0;
    if (!data || len <= 0) return NULL;
    unsigned char* in = (unsigned char*)malloc((size_t)len);
    if (!in) return NULL;
    for (int64_t i = 0; i < len; i++) in[i] = (unsigned char)(data[i] & 0xFF);
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    unsigned err = lodepng_decode32(&rgba, &uw, &uh, in, (size_t)len);
    free(in);
    if (err) { free(rgba); fprintf(stderr, "[png-oracle] decode: %s\n", lodepng_error_text(err)); return NULL; }
    size_t count = (size_t)uw * (size_t)uh;
    int64_t* px = (int64_t*)rae_ext_rae_buf_alloc((int64_t)count, (int64_t)sizeof(int64_t));
    if (!px) { free(rgba); return NULL; }
    for (size_t i = 0; i < count; i++)
        px[i] = ((int64_t)rgba[i*4+3] << 24) | ((int64_t)rgba[i*4+0] << 16) | ((int64_t)rgba[i*4+1] << 8) | rgba[i*4+2];
    free(rgba);
    if (w_out) *w_out = uw; if (h_out) *h_out = uh;
    return px;
}
/* Encode 0xAARRGGBB pixels to PNG bytes via lodepng — oracle for the encoder. */
int64_t* rae_ext_compress_oracle_encodePng(const int64_t* pixels, int64_t w, int64_t h, int64_t* out_len) {
    if (out_len) *out_len = 0;
    if (!pixels || w <= 0 || h <= 0) return NULL;
    size_t count = (size_t)w * (size_t)h;
    unsigned char* rgba = (unsigned char*)malloc(count * 4);
    if (!rgba) return NULL;
    for (size_t i = 0; i < count; i++) {
        int64_t p = pixels[i];
        rgba[i*4+0] = (unsigned char)((p >> 16) & 0xFF);
        rgba[i*4+1] = (unsigned char)((p >> 8) & 0xFF);
        rgba[i*4+2] = (unsigned char)(p & 0xFF);
        rgba[i*4+3] = (unsigned char)((p >> 24) & 0xFF);
    }
    unsigned char* out = NULL; size_t outsize = 0;
    unsigned err = lodepng_encode32(&out, &outsize, rgba, (unsigned)w, (unsigned)h);
    free(rgba);
    if (err) { free(out); fprintf(stderr, "[png-oracle] encode: %s\n", lodepng_error_text(err)); return NULL; }
    int64_t* res = (int64_t*)rae_ext_rae_buf_alloc((int64_t)outsize, (int64_t)sizeof(int64_t));
    if (!res) { free(out); return NULL; }
    for (size_t i = 0; i < outsize; i++) res[i] = out[i];
    free(out);
    if (out_len) *out_len = (int64_t)outsize;
    return res;
}

/* ============================================================
 * SDL3 desktop platform layer — see lib/sdl3.rae (RAE_HAS_SDL3).
 *
 * A handle-free, single-window windowing/present backend parallel to the
 * raylib block: init creates window + renderer + a streaming texture (kept in
 * file-static globals), updatePixels expands the packed-0xRRGGBB framebuffer to
 * RGBA8 and uploads it, present draws it, shouldClose pumps events.
 * Compiled-target only — the Live VM has no SDL bindings.
 *
 * Headless verification: RAE_SDL_SCREENSHOT=<path.bmp> saves the last uploaded
 * frame on close; RAE_SDL_HEADLESS_MS=<ms> auto-closes after that wall-clock
 * budget — so an agent/CI run can render + snapshot without a human closing the
 * window.
 * ============================================================ */
#ifdef RAE_HAS_SDL3
#include <SDL3/SDL.h>

static SDL_Window*   g_sdl_win = NULL;
static SDL_Renderer* g_sdl_ren = NULL;
static SDL_Texture*  g_sdl_tex = NULL;
static int g_sdl_w = 0, g_sdl_h = 0;            /* window size */
static int g_sdl_tex_w = 0, g_sdl_tex_h = 0;    /* current texture (framebuffer) size */
static unsigned char* g_sdl_scratch = NULL;   /* RGBA8 expansion of the last frame */
static int64_t g_sdl_scratch_px = 0;
static int64_t g_sdl_start_ms = 0;
static int64_t g_sdl_headless_ms = 0;          /* >0 => auto-close after this budget */
static int64_t g_sdl_target_fps = 0;           /* >0 => cap present rate */
static int64_t g_sdl_last_present_ms = 0;
static unsigned char g_sdl_pressed[SDL_SCANCODE_COUNT]; /* went-down-this-frame edges */
static unsigned char g_sdl_keydown[SDL_SCANCODE_COUNT]; /* held state, from key down/up events */
static unsigned char g_sdl_mouse[8];                    /* held state, by SDL button index (1=L,2=M,3=R) */
static unsigned char g_sdl_mouse_pressed[8];            /* went-down-this-frame edges */
static unsigned char g_sdl_mouse_released[8];           /* went-up-this-frame edges */
static bool g_sdl_mouse_captured = false;

/* Map raylib/GLFW key codes (letters = ASCII uppercase, arrows = 262-265, plus
 * a few common keys) to SDL scancodes so ported examples keep their key ints. */
static SDL_Scancode rae_sdl_scancode(int64_t key) {
    if (key >= 'A' && key <= 'Z') return SDL_GetScancodeFromKey((SDL_Keycode)(key + 32), NULL); /* lowercase */
    if (key >= '0' && key <= '9') return SDL_GetScancodeFromKey((SDL_Keycode)key, NULL);
    if (key >= 290 && key <= 301) return (SDL_Scancode)(SDL_SCANCODE_F1 + (int)(key - 290)); /* raylib F1..F12 */
    switch (key) {
        case 32:  return SDL_SCANCODE_SPACE;
        case 256: return SDL_SCANCODE_ESCAPE;
        case 257: return SDL_SCANCODE_RETURN;
        case 262: return SDL_SCANCODE_RIGHT;
        case 263: return SDL_SCANCODE_LEFT;
        case 264: return SDL_SCANCODE_DOWN;
        case 265: return SDL_SCANCODE_UP;
        case 340: return SDL_SCANCODE_LSHIFT;
        default:  return SDL_SCANCODE_UNKNOWN;
    }
}

void rae_ext_sdl3_initWindow(int64_t width, int64_t height, rae_String title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
        return;
    }
    g_sdl_w = (int)width; g_sdl_h = (int)height;
    const char* t = title.data ? (const char*)title.data : "Rae (SDL3)";
    if (!SDL_CreateWindowAndRenderer(t, (int)width, (int)height, SDL_WINDOW_RESIZABLE, &g_sdl_win, &g_sdl_ren)) {
        fprintf(stderr, "[sdl] window/renderer failed: %s\n", SDL_GetError());
        return;
    }
    /* Texture is created lazily by sdlUpdatePixels (its size can differ from the
     * window and can change at runtime — e.g. a preview/final quality toggle). */
    g_sdl_start_ms = rae_ext_nowMs();
    g_sdl_last_present_ms = g_sdl_start_ms;
    const char* hm = getenv("RAE_SDL_HEADLESS_MS");
    if (hm) g_sdl_headless_ms = (int64_t)atoll(hm);
}

void rae_ext_sdl3_setTargetFPS(int64_t fps) {
    g_sdl_target_fps = fps > 0 ? fps : 0;
}

/* Held key/mouse state is tracked from explicit down/up EVENTS (not the live
 * SDL_GetKeyboardState/SDL_GetMouseState snapshots) so a release is never lost:
 *  - on window focus loss the OS stops sending our up events -> clear all held
 *    state, else a key/button held at focus-out would stick forever;
 *  - while any mouse button is held we SDL_CaptureMouse so a drag that releases
 *    OUTSIDE the window still delivers its button-up (the stuck-drag bug). */
rae_Bool rae_ext_sdl3_shouldClose(void) {
    memset(g_sdl_pressed, 0, sizeof(g_sdl_pressed));
    SDL_Event ev;
    rae_Bool quit = false;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT: quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) quit = true;
                if (ev.key.scancode < SDL_SCANCODE_COUNT) {
                    g_sdl_keydown[ev.key.scancode] = 1;
                    if (!ev.key.repeat) g_sdl_pressed[ev.key.scancode] = 1;  /* edge */
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (ev.key.scancode < SDL_SCANCODE_COUNT) g_sdl_keydown[ev.key.scancode] = 0;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button < 8) g_sdl_mouse[ev.button.button] = 1;
                if (!g_sdl_mouse_captured) { SDL_CaptureMouse(true); g_sdl_mouse_captured = true; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (ev.button.button < 8) g_sdl_mouse[ev.button.button] = 0;
                bool any = false;
                for (int b = 0; b < 8; b++) if (g_sdl_mouse[b]) any = true;
                if (!any && g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                /* never see the up events while unfocused -> nothing stays stuck */
                memset(g_sdl_keydown, 0, sizeof(g_sdl_keydown));
                memset(g_sdl_mouse, 0, sizeof(g_sdl_mouse));
                if (g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            default: break;
        }
    }
    if (quit) return true;
    if (g_sdl_headless_ms > 0 && rae_ext_nowMs() - g_sdl_start_ms >= g_sdl_headless_ms) return true;
    return false;
}

int64_t rae_ext_sdl3_getMouseX(void) {
    float x = 0, y = 0; SDL_GetMouseState(&x, &y); return (int64_t)x;
}
int64_t rae_ext_sdl3_getMouseY(void) {
    float x = 0, y = 0; SDL_GetMouseState(&x, &y); return (int64_t)y;
}
/* Current renderer output size in pixels — tracks window resizes (the window
 * is created SDL_WINDOW_RESIZABLE). Apps poll this to re-render at the new size. */
int64_t rae_ext_sdl3_windowWidth(void) {
    int w = g_sdl_w, h = 0; if (g_sdl_ren) SDL_GetRenderOutputSize(g_sdl_ren, &w, &h); return (int64_t)w;
}
int64_t rae_ext_sdl3_windowHeight(void) {
    int w = 0, h = g_sdl_h; if (g_sdl_ren) SDL_GetRenderOutputSize(g_sdl_ren, &w, &h); return (int64_t)h;
}
rae_Bool rae_ext_sdl3_isMouseButtonDown(int64_t button) {
    /* raylib button (0=L,1=R,2=M) -> SDL button index (1=L,2=M,3=R). */
    int sdlb = button == 1 ? SDL_BUTTON_RIGHT : (button == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT);
    return sdlb < 8 && g_sdl_mouse[sdlb] != 0;
}
rae_Bool rae_ext_sdl3_isKeyDown(int64_t key) {
    SDL_Scancode sc = rae_sdl_scancode(key);
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT) return false;
    return g_sdl_keydown[sc] != 0;
}
rae_Bool rae_ext_sdl3_isKeyPressed(int64_t key) {
    SDL_Scancode sc = rae_sdl_scancode(key);
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT) return false;
    return g_sdl_pressed[sc] != 0;
}

void rae_ext_sdl3_updatePixels(const int64_t* pixels, int64_t w, int64_t h) {
    if (!pixels || w <= 0 || h <= 0 || !g_sdl_ren) return;
    int64_t count = w * h;
    /* (Re)create the texture when the framebuffer size changes. */
    if (!g_sdl_tex || (int)w != g_sdl_tex_w || (int)h != g_sdl_tex_h) {
        if (g_sdl_tex) SDL_DestroyTexture(g_sdl_tex);
        g_sdl_tex = SDL_CreateTexture(g_sdl_ren, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
        if (!g_sdl_tex) { fprintf(stderr, "[sdl] texture failed: %s\n", SDL_GetError()); return; }
        SDL_SetTextureScaleMode(g_sdl_tex, SDL_SCALEMODE_LINEAR);
        g_sdl_tex_w = (int)w; g_sdl_tex_h = (int)h;
    }
    if (count > g_sdl_scratch_px) {
        unsigned char* grown = (unsigned char*)realloc(g_sdl_scratch, (size_t)count * 4);
        if (!grown) return;
        g_sdl_scratch = grown; g_sdl_scratch_px = count;
    }
    for (int64_t i = 0; i < count; i++) {
        int64_t p = pixels[i];
        g_sdl_scratch[i * 4 + 0] = (unsigned char)((p >> 16) & 0xFF);  /* R */
        g_sdl_scratch[i * 4 + 1] = (unsigned char)((p >> 8) & 0xFF);   /* G */
        g_sdl_scratch[i * 4 + 2] = (unsigned char)(p & 0xFF);          /* B */
        g_sdl_scratch[i * 4 + 3] = 255;                                /* A */
    }
    SDL_UpdateTexture(g_sdl_tex, NULL, g_sdl_scratch, (int)w * 4);
}

void rae_ext_sdl3_present(void) {
    if (!g_sdl_ren) return;
    SDL_SetRenderDrawColor(g_sdl_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_sdl_ren);
    if (g_sdl_tex) {
        /* Fit the texture into the window preserving its aspect ratio —
         * pillarbox (bars left/right) or letterbox (bars top/bottom) — instead
         * of stretching, so a non-matching window doesn't skew the image. */
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(g_sdl_ren, &ow, &oh);
        if (ow > 0 && oh > 0 && g_sdl_tex_w > 0 && g_sdl_tex_h > 0) {
            float ta = (float)g_sdl_tex_w / (float)g_sdl_tex_h;
            float wa = (float)ow / (float)oh;
            SDL_FRect dst;
            if (wa > ta) {            /* window wider than image -> pillarbox */
                dst.h = (float)oh; dst.w = (float)oh * ta;
                dst.x = ((float)ow - dst.w) * 0.5f; dst.y = 0.0f;
            } else {                  /* window taller than image -> letterbox */
                dst.w = (float)ow; dst.h = (float)ow / ta;
                dst.x = 0.0f; dst.y = ((float)oh - dst.h) * 0.5f;
            }
            SDL_RenderTexture(g_sdl_ren, g_sdl_tex, NULL, &dst);
        } else {
            SDL_RenderTexture(g_sdl_ren, g_sdl_tex, NULL, NULL);
        }
    }
    SDL_RenderPresent(g_sdl_ren);
    if (g_sdl_target_fps > 0) {
        int64_t frame_ms = 1000 / g_sdl_target_fps;
        int64_t now = rae_ext_nowMs();
        int64_t elapsed = now - g_sdl_last_present_ms;
        if (elapsed < frame_ms) SDL_Delay((Uint32)(frame_ms - elapsed));
        g_sdl_last_present_ms = rae_ext_nowMs();
    }
}

void rae_ext_sdl3_setTitle(rae_String title) {
    if (g_sdl_win && title.data) SDL_SetWindowTitle(g_sdl_win, (const char*)title.data);
}

void rae_ext_sdl3_closeWindow(void) {
    /* Headless snapshot: dump the last uploaded frame as a BMP (reliable —
     * straight from our pixel buffer, not a GPU read-back). */
    const char* shot = getenv("RAE_SDL_SCREENSHOT");
    if (shot && g_sdl_scratch && g_sdl_tex_w > 0 && g_sdl_tex_h > 0) {
        SDL_Surface* s = SDL_CreateSurfaceFrom(g_sdl_tex_w, g_sdl_tex_h, SDL_PIXELFORMAT_RGBA32,
                                               g_sdl_scratch, g_sdl_tex_w * 4);
        if (s) { SDL_SaveBMP(s, shot); SDL_DestroySurface(s); }
    }
    if (g_sdl_tex) SDL_DestroyTexture(g_sdl_tex);
    if (g_sdl_ren) SDL_DestroyRenderer(g_sdl_ren);
    if (g_sdl_win) SDL_DestroyWindow(g_sdl_win);
    SDL_Quit();
    g_sdl_tex = NULL; g_sdl_ren = NULL; g_sdl_win = NULL;
}

/* ---- MTSDF text compositing into the packed-0xRRGGBB framebuffer (see
 * lib/sdf_text.rae). Rae parses the atlas JSON + lays out glyphs; here we hold
 * the raw RGBA atlas and composite one glyph quad at a time: bilinear-sample
 * the field, median(r,g,b), screenPxRange smoothstep -> coverage, alpha-blend
 * the text colour over the framebuffer. This is the SDL3 (CPU-framebuffer) port
 * of the raylib MSDF shader — no GPU shader needed. ---- */
#define RAE_SDF_MAX_ATLAS 8
static unsigned char* g_sdf_atlas[RAE_SDF_MAX_ATLAS];
static int g_sdf_atlas_w[RAE_SDF_MAX_ATLAS];
static int g_sdf_atlas_h[RAE_SDF_MAX_ATLAS];
static int g_sdf_atlas_n = 0;

int64_t rae_ext_sdf_text_loadAtlas(rae_String path, int64_t w, int64_t h) {
    if (!path.data || w <= 0 || h <= 0 || g_sdf_atlas_n >= RAE_SDF_MAX_ATLAS) return 0;
    FILE* f = fopen((const char*)path.data, "rb");
    if (!f) { fprintf(stderr, "[sdf] cannot open %s\n", (const char*)path.data); return 0; }
    size_t bytes = (size_t)w * (size_t)h * 4;
    unsigned char* px = (unsigned char*)malloc(bytes);
    if (!px) { fclose(f); return 0; }
    size_t got = fread(px, 1, bytes, f);
    fclose(f);
    if (got != bytes) { free(px); fprintf(stderr, "[sdf] short read on %s\n", (const char*)path.data); return 0; }
    g_sdf_atlas[g_sdf_atlas_n] = px; g_sdf_atlas_w[g_sdf_atlas_n] = (int)w; g_sdf_atlas_h[g_sdf_atlas_n] = (int)h;
    return ++g_sdf_atlas_n;  /* 1-based */
}

static float rae_sdf_median(float a, float b, float c) {
    return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
}

/* sx0..sy1: dest rect in framebuffer pixels (sy0 top, sy1 bottom). au0..av1:
 * source rect in atlas pixels, top-left origin. */
void rae_ext_sdf_text_blitGlyph(int64_t* fb, int64_t fbW, int64_t fbH, int64_t atlas,
                          double sx0, double sy0, double sx1, double sy1,
                          double au0, double av0, double au1, double av1,
                          double screenPxRange, int64_t rgb) {
    if (!fb || atlas < 1 || atlas > g_sdf_atlas_n) return;
    const unsigned char* ap = g_sdf_atlas[atlas - 1];
    int aw = g_sdf_atlas_w[atlas - 1], ah = g_sdf_atlas_h[atlas - 1];
    float tr = (float)((rgb >> 16) & 0xFF), tg = (float)((rgb >> 8) & 0xFF), tb = (float)(rgb & 0xFF);
    int x0 = (int)floor(sx0), x1 = (int)ceil(sx1);
    int y0 = (int)floor(sy0), y1 = (int)ceil(sy1);
    double sw = sx1 - sx0, sh = sy1 - sy0;
    if (sw <= 0 || sh <= 0) return;
    for (int py = y0; py < y1; py++) {
        if (py < 0 || py >= fbH) continue;
        for (int px = x0; px < x1; px++) {
            if (px < 0 || px >= fbW) continue;
            double fx = ((double)px + 0.5 - sx0) / sw;
            double fy = ((double)py + 0.5 - sy0) / sh;
            /* atlas sample point (texel-centre bilinear) */
            double gx = au0 + fx * (au1 - au0) - 0.5;
            double gy = av0 + fy * (av1 - av0) - 0.5;
            int ix = (int)floor(gx), iy = (int)floor(gy);
            double tx = gx - ix, ty = gy - iy;
            float chan[3] = {0, 0, 0};
            for (int ch = 0; ch < 3; ch++) {
                float s00, s10, s01, s11;
                #define RAE_SDF_TX(xx, yy) (ap[(((yy) < 0 ? 0 : (yy) >= ah ? ah - 1 : (yy)) * aw + ((xx) < 0 ? 0 : (xx) >= aw ? aw - 1 : (xx))) * 4 + ch] / 255.0f)
                s00 = RAE_SDF_TX(ix, iy);     s10 = RAE_SDF_TX(ix + 1, iy);
                s01 = RAE_SDF_TX(ix, iy + 1); s11 = RAE_SDF_TX(ix + 1, iy + 1);
                #undef RAE_SDF_TX
                float top = s00 + (s10 - s00) * (float)tx;
                float bot = s01 + (s11 - s01) * (float)tx;
                chan[ch] = top + (bot - top) * (float)ty;
            }
            float sd = rae_sdf_median(chan[0], chan[1], chan[2]);
            float cov = (sd - 0.5f) * (float)screenPxRange + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            int64_t bg = fb[py * fbW + px];
            float br = (float)((bg >> 16) & 0xFF), bgc = (float)((bg >> 8) & 0xFF), bb = (float)(bg & 0xFF);
            int rr = (int)(tr * cov + br * (1.0f - cov) + 0.5f);
            int gg = (int)(tg * cov + bgc * (1.0f - cov) + 0.5f);
            int bbv = (int)(tb * cov + bb * (1.0f - cov) + 0.5f);
            fb[py * fbW + px] = (int64_t)((rr << 16) | (gg << 8) | bbv);
        }
    }
}

/* ---- Rae Filesystem & Paths API (lib/filesystem.rae) — thin wrappers over
 * SDL3's SDL_filesystem.h: known folders, mkdir, exists, plus a date helper and
 * a render-output next-index scan. See docs/filesystem-and-paths.md. ---- */
rae_String rae_ext_filesystem_userFolder(int64_t kind) {
    SDL_Folder f = SDL_FOLDER_DESKTOP;
    if (kind == 1) f = SDL_FOLDER_PICTURES;
    else if (kind == 2) f = SDL_FOLDER_DOCUMENTS;
    else if (kind == 3) f = SDL_FOLDER_HOME;
    const char* p = SDL_GetUserFolder(f);
    return rae_str_from_cstr_impl(p ? p : "", RAE_SITE_READ_FILE);
}

rae_String rae_ext_filesystem_prefDir(rae_String org, rae_String app) {
    char* p = SDL_GetPrefPath(org.data ? (const char*)org.data : "Rae",
                              app.data ? (const char*)app.data : "app");
    rae_String s = rae_str_from_cstr_impl(p ? p : "", RAE_SITE_READ_FILE);
    if (p) SDL_free(p);
    return s;
}

rae_Bool rae_ext_filesystem_makeDir(rae_String path) {
    if (!path.data) return false;
    return SDL_CreateDirectory((const char*)path.data);
}

rae_Bool rae_ext_filesystem_exists(rae_String path) {
    if (!path.data) return false;
    SDL_PathInfo info;
    return SDL_GetPathInfo((const char*)path.data, &info);
}

/* Today's local date as "YYYY-MM-DD". */
rae_String rae_ext_filesystem_today(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return rae_str_from_cstr_impl(buf, RAE_SITE_READ_FILE);
}

/* Scan `dir` for files named "<prefix><N>.png" and return max(N)+1 (1 if none),
 * so a caller can mint a non-overwriting filename. */
int64_t rae_ext_filesystem_nextIndex(rae_String dir, rae_String prefix) {
    if (!dir.data || !prefix.data) return 1;
    DIR* d = opendir((const char*)dir.data);
    if (!d) return 1;
    int max_n = 0;
    size_t plen = (size_t)prefix.len;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, (const char*)prefix.data, plen) == 0) {
            int v = atoi(e->d_name + plen);
            if (v > max_n) max_n = v;
        }
    }
    closedir(d);
    return (int64_t)(max_n + 1);
}
#endif /* RAE_HAS_SDL3 */
