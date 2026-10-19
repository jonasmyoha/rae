/* Set once any frame reaches the surface, from either the 2D or the 3D present
 * path. Read by the headless close check so a run that drew nothing can say
 * so instead of exiting silently with no screenshot. */
rae_Bool g_rae_presented_any = 0;
rae_Bool rae_frame_presented_any(void) { return g_rae_presented_any; }

/* The skip / recovery streak of the present (which statuses retry, when to
 * reconfigure, the log lines) is Rae's since render-perf-11b
 * (lib/gpu/SurfaceRecovery.rae). */

/* gpu2d frame begin/end, screenshot readback, flush, present, and shutdown. Raw GPU frame operations stay C; frame policy can migrate later.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* The gpu2d frame command lifecycle (begin/end) runs in Rae now (lib/gpu2d.rae,
 * #504): Rae creates the encoder + the offscreen render pass and, on end,
 * flushes + submits over the bindings. C keeps the per-frame batch-counter reset
 * and the platform present/screenshot tail. rae_g2d_frame_reset clears the
 * present flag at the top of a frame (the batches, clip stack and viewport
 * uniform are the Rae canvas's since #913/#915). */
void rae_g2d_frame_reset(void) {
    g_g2d_last_present_ok = 0;
}
/* #910: the frame's encoder + pass are owned by the Rae canvas (Gpu2dCanvas);
 * nothing is parked here any more. */

/* Headless verification: copy the just-rendered surface texture back to a
 * mapped buffer and save it as a BMP. Called from endFrame after submit while
 * g_g2d_frame_tex is still alive (before present/release). The surface is
 * configured with CopySrc usage (rae_g2d_configure). Gated on the env var so
 * it costs nothing in normal runs. The readback row stride must be 256-aligned
 * (WebGPU copy requirement); we unpad into a tight RGBA buffer for SDL. */
static void rae_g2d_save_screenshot(const char* path, WGPUTexture tex, int w, int h) {
    if (!path || !tex || !g_wgpu_dev) return;
    if (w <= 0 || h <= 0) return;
    uint32_t bpr = (uint32_t)w * 4u;
    uint32_t padded = (bpr + 255u) & ~255u;            /* 256-byte row align */
    size_t bytes = (size_t)padded * (size_t)h;
    WGPUBufferDescriptor rbd; memset(&rbd, 0, sizeof(rbd));
    rbd.size = bytes; rbd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(g_wgpu_dev, &rbd);
    if (!staging) return;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPUTexelCopyTextureInfo src; memset(&src, 0, sizeof(src));
    src.texture = tex; src.mipLevel = 0; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst; memset(&dst, 0, sizeof(dst));
    dst.buffer = staging; dst.layout.offset = 0;
    dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext; ext.width = (uint32_t)w; ext.height = (uint32_t)h; ext.depthOrArrayLayers = 1;
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    g_wgpu_map_done = 0;
    WGPUBufferMapCallbackInfo mci; memset(&mci, 0, sizeof(mci));
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = rae_wgpu_on_map;
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, mci);
    while (!g_wgpu_map_done) rae_wgpu_poll(1);
    const unsigned char* px = (const unsigned char*)wgpuBufferGetConstMappedRange(staging, 0, bytes);
    if (px) {
        unsigned char* rgba = (unsigned char*)malloc((size_t)w * (size_t)h * 4u);
        if (rgba) {
            int swap_rb = (g_g2d_fmt == WGPUTextureFormat_BGRA8Unorm ||
                           g_g2d_fmt == WGPUTextureFormat_BGRA8UnormSrgb);
            for (int y = 0; y < h; y++) {
                const unsigned char* row = px + (size_t)y * padded;
                unsigned char* orow = rgba + (size_t)y * (size_t)w * 4u;
                for (int x = 0; x < w; x++) {
                    unsigned char c0 = row[x*4+0], c1 = row[x*4+1], c2 = row[x*4+2];
                    if (swap_rb) { orow[x*4+0] = c2; orow[x*4+1] = c1; orow[x*4+2] = c0; }
                    else        { orow[x*4+0] = c0; orow[x*4+1] = c1; orow[x*4+2] = c2; }
                    orow[x*4+3] = 255;
                }
            }
            SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba, w * 4);
            if (s) { SDL_SaveBMP(s, path); SDL_DestroySurface(s); }
            free(rgba);
        }
        wgpuBufferUnmap(staging);
    }
    wgpuBufferRelease(staging);
}

/* Called by Rae's endFrame AFTER the frame was submitted through the manager.
 * #920: the presentable target is the canvas's texture, passed in (C borrows
 * it for the readback and owns nothing of it): the optional headless
 * screenshot. The drawable is Rae's to acquire (rae_surface_acquire). */
void rae_g2d_tick(void) {
    if (g_g2d_test_pointer_state >= 2) g_g2d_test_pointer_paints++;
    rae_g2d_tick_virtual_clock();
}
void rae_g2d_present(void* texture, int64_t width, int64_t height) {
    WGPUTexture tex = (WGPUTexture)texture;
    if (!tex || !g_wgpu_dev) return;
    rae_wgpu_report_periodic();
    /* Per-frame GPU objects (bind groups, clip uniforms) are the Rae canvas's
     * (#907-#910) and are retired there at frame end. */

    /* Headless screenshot reads the offscreen target — works even when the
     * surface can't vend a drawable. */
    /* Either headless budget enables the capture: the ms budget (legacy)
     * or the deterministic frame budget. */
    if (g_sdl_headless_ms > 0 || g_sdl_headless_frames > 0) {
        g_rae_presented_any = 1;
        const char* shot = getenv("RAE_GPU2D_SCREENSHOT");
        if (shot) rae_g2d_save_screenshot(shot, tex, (int)width, (int)height);
    }

    /* The drawable is Rae's (lib/gpu/Surface.rae): it acquired it for the
     * frame or acquires it now, and presents. */
}

/* The window surface for Rae's present (lib/gpu/Surface.rae, render-perf-11b):
 * Rae acquires the drawable, renders the frame straight into it or copies the
 * offscreen target in, and presents, over the WebGPU bindings. C reports the
 * platform facts and does the platform calls below, one each. */
void* rae_surface_handle(void) { return (void*)g_g2d_surface; }

/* SDL's window state (hidden / minimized / occluded): Rae never acquires a
 * drawable while it is false (the macOS drawable leak, see
 * rae_g2d_window_visible). */
int64_t rae_surface_window_visible(void) { return rae_g2d_window_visible() ? 1 : 0; }

/* Configure the surface at the window's current pixel size (Rae's recovery
 * calls it after an Outdated / Lost / Error acquire). */
void rae_surface_reconfigure(void) {
    if (!g_sdl_win || !g_g2d_surface) return;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_sdl_win, &pw, &ph);
    rae_g2d_configure(pw, ph);
}

/* A frame reached the surface: the flags C's headless close check and
 * Gpu2d.lastPresentOk read. */
void rae_surface_note_presented(void) {
    g_rae_presented_any = 1;
    g_g2d_last_present_ok = 1;
}

rae_Bool rae_ext_Gpu2d_lastPresentOk(void) {
    return g_g2d_last_present_ok != 0;
}

/* #920: closeWindow releases no Rae-owned object — the offscreen target is
 * the canvas's (canvasShutdown retires it); only the surface + cursors here. */
void rae_ext_Gpu2d_closeWindow(void) {
    if (g_g2d_surface) { wgpuSurfaceRelease(g_g2d_surface); g_g2d_surface = NULL; }
    for (int i = 0; i < 7; i++) {
        if (g_g2d_cursors[i]) { SDL_DestroyCursor(g_g2d_cursors[i]); g_g2d_cursors[i] = NULL; }
    }
    g_g2d_cursor_kind = -1;
#ifndef __EMSCRIPTEN__
    g_g2d_presentation_layer = NULL;  /* it goes with the view */
    if (g_g2d_metal_view) { SDL_Metal_DestroyView(g_g2d_metal_view); g_g2d_metal_view = NULL; }
#endif
    if (g_sdl_win) { SDL_DestroyWindow(g_sdl_win); g_sdl_win = NULL; }
}
