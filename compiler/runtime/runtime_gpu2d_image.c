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




/* rae_g2d_decode_rgba and the file read: runtime_image_decode.c */

/* The app's icon from a PNG or JPEG at `path`: on macOS the Dock's and the
 * app switcher's, for a bare executable too (SDL sets the application icon
 * image); elsewhere what SDL does with a window icon, or nothing (iOS takes
 * its icon from the bundle). Returns 1 when it was set, 0 (logged) when
 * there is no window or the file does not decode. */
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
int64_t rae_ext_Gpu2d_setWindowIcon(rae_String path) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    (void)path;
    return 0;  /* the bundle's icon, and the file is not in the bundle */
#else
    if (!g_sdl_win || !path.data) return 0;
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    const char* cpath = (const char*)path.data;
    const char* why = "decode failed";
    if (!rae_g2d_decode_rgba(cpath, &rgba, &uw, &uh, &why)) {
        fprintf(stderr, "[gpu2d] window icon decode failed (%s): %s\n", cpath, why);
        return 0;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom((int)uw, (int)uh, SDL_PIXELFORMAT_RGBA32, rgba, (int)uw * 4);
    int set = 0;
    if (surface) {
        set = SDL_SetWindowIcon(g_sdl_win, surface) ? 1 : 0;
        SDL_DestroySurface(surface);
    }
    if (!set) fprintf(stderr, "[gpu2d] window icon not set: %s\n", SDL_GetError());
    free(rgba);
    return set;
#endif
}

/* Device-free decode probe (#228): run the exact decode + error
 * policy of gpu2d.loadImage without needing a WebGPU device, so the
 * corrupt-file behavior is testable in the headless suite. Returns
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

