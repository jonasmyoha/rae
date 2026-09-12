/* Set once any frame reaches the surface, from either the 2D or the 3D present
 * path. Read by the headless close check so a run that drew nothing can say
 * so instead of exiting silently with no screenshot. */
rae_Bool g_rae_presented_any = 0;
rae_Bool rae_frame_presented_any(void) { return g_rae_presented_any; }

/* BEGIN PRESENT RECOVERY (also compiled by test_present_recovery.sh).
 * Native Occluded is an extension, not a broken swapchain. Spell its ABI value
 * locally so builds with older native headers and browser headers still work.
 * https://github.com/gfx-rs/wgpu-native/blob/trunk/ffi/wgpu.h */
#define RAE_SURFACE_OCCLUDED 0x00030001
static const char* rae_present_status_name(int s) {
    switch (s) {
        case 0:                                                    return "none(0)";
        case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:    return "SuccessOptimal";
        case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal: return "SuccessSuboptimal";
        case WGPUSurfaceGetCurrentTextureStatus_Timeout:           return "Timeout";
        case WGPUSurfaceGetCurrentTextureStatus_Outdated:          return "Outdated";
        case WGPUSurfaceGetCurrentTextureStatus_Lost:              return "Lost";
        case WGPUSurfaceGetCurrentTextureStatus_Error:             return "Error";
        case RAE_SURFACE_OCCLUDED:                                return "Occluded";
        default: return "unknown";
    }
}
/* A skip of 1-2 frames at startup is a normal transient (the drawable size
 * settles a frame after the window appears) and self-heals, so stay quiet until
 * a run of skips looks STUCK — that is the black-screen signature. */
#define RAE_PRESENT_SKIP_ALARM 3
static long g_rae_present_skip_streak = 0;
/* Call when a frame WAS presented — reports recovery only if we had been stuck. */
void rae_present_note_ok(void) {
    if (g_rae_present_skip_streak >= RAE_PRESENT_SKIP_ALARM) {
        fprintf(stderr, "[present] surface recovered after %ld skipped frame(s)\n",
                g_rae_present_skip_streak);
    }
    g_rae_present_skip_streak = 0;
}
/* Call when a frame was NOT presented while a surface exists. `reason` is a
 * status name or "window-not-visible". Silent for the first couple of frames
 * (benign startup transient); once stuck, logs at streak 3..12 then every 120. */
void rae_present_note_skip(const char* which, const char* reason) {
    g_rae_present_skip_streak++;
    long n = g_rae_present_skip_streak;
    if (n < RAE_PRESENT_SKIP_ALARM) return;
    if (n <= 12 || (n % 120) == 0) {
        fprintf(stderr, "[present] %s: nothing presented for %ld frame(s) - surface status=%s. "
                        "Retrying presentation; this alone does not mean the surface is lost.\n",
                which, n, reason);
    }
}

/* Call AFTER releasing the acquired drawable. Occluded/Timeout leave the
 * surface valid: pump events and retry, never rebuild it or steal focus.
 * Retain configuration recovery for actual surface errors, but bound retries
 * when a persistent failure cannot be repaired by reconfiguration. */
static void rae_present_recover(const char* which, int status) {
    if (status == RAE_SURFACE_OCCLUDED ||
        status == WGPUSurfaceGetCurrentTextureStatus_Timeout) {
        g_rae_present_skip_streak++;
        if (g_rae_present_skip_streak == RAE_PRESENT_SKIP_ALARM) {
            fprintf(stderr, "[present] %s: %s(%d); keeping the surface, retrying when drawable is available\n",
                    which, rae_present_status_name(status), status);
        }
        return;
    }
    char reason[48];
    snprintf(reason, sizeof(reason), "%s(%d)", rae_present_status_name(status), status);
    rae_present_note_skip(which, reason);
    long n = g_rae_present_skip_streak;
    if (g_sdl_win && g_g2d_surface && (n <= RAE_PRESENT_SKIP_ALARM || n % 120 == 0)) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_sdl_win, &pw, &ph);
        rae_g2d_configure(pw, ph);
    }
}
/* END PRESENT RECOVERY */

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
 * it for the copy / readback and owns nothing of it). This is the platform
 * tail: the optional headless screenshot, the best-effort copy into the
 * surface drawable + present, and the device poll. */
void rae_g2d_tick(void) { rae_g2d_tick_virtual_clock(); }
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

    /* Present best-effort: copy the offscreen image into the surface drawable
     * and present. If the OS won't vend a drawable (window occluded / display
     * asleep / headless), skip — the frame already rendered + screenshotted.
     *
     * DO NOT acquire a drawable while the window is hidden / minimized /
     * occluded. On macOS the CAMetalLayer stops recycling its drawable pool once
     * the app is backgrounded and hands out a fresh IOSurface-backed texture per
     * frame — which wgpu still reports as SuccessOptimal, and whose submissions
     * retire normally — so nothing signals a problem while RSS balloons ~100
     * textures/frame after each app switch. Skipping the acquire entirely while
     * dark is the only thing that keeps the drawable count flat (matches ImGui's
     * Metal backend and bevy's occlusion handling). */
    int presented = 0;
    if (g_g2d_surface && rae_g2d_window_visible()) {
        WGPUSurfaceTexture st; memset(&st, 0, sizeof(st));
        wgpuSurfaceGetCurrentTexture(g_g2d_surface, &st);
        if (st.texture &&
            (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
             st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
            WGPUCommandEncoder penc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
            WGPUTexelCopyTextureInfo cs; memset(&cs, 0, sizeof(cs)); cs.texture = tex; cs.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyTextureInfo cd; memset(&cd, 0, sizeof(cd)); cd.texture = st.texture; cd.aspect = WGPUTextureAspect_All;
            /* the target is built to the configured size; copy the smaller of the two */
            uint32_t cw = (uint32_t)(width < g_sdl_w ? width : g_sdl_w);
            uint32_t ch = (uint32_t)(height < g_sdl_h ? height : g_sdl_h);
            WGPUExtent3D ext; ext.width = cw; ext.height = ch; ext.depthOrArrayLayers = 1;
            wgpuCommandEncoderCopyTextureToTexture(penc, &cs, &cd, &ext);
            WGPUCommandBuffer pcb = wgpuCommandEncoderFinish(penc, NULL);
            wgpuQueueSubmit(g_wgpu_queue, 1, &pcb);
            wgpuCommandBufferRelease(pcb); wgpuCommandEncoderRelease(penc);
#ifndef __EMSCRIPTEN__
            /* Browser WebGPU presents the canvas texture when control returns to
             * the browser; Emdawn rejects explicit wgpuSurfacePresent calls. */
            wgpuSurfacePresent(g_g2d_surface);
            g_rae_presented_any = 1;
#endif
            g_g2d_last_present_ok = 1;
            presented = 1;
            rae_present_note_ok();
        }
        /* SDL visibility can precede native visibility. Release the drawable
         * first, then apply status-specific recovery (not always configure). */
        if (st.texture) wgpuTextureRelease(st.texture);
        if (!presented) rae_present_recover("gpu2d", st.status);
    } else {
        rae_present_note_skip("gpu2d", "window-not-visible");
    }
    /* wgpu-native retires deferred object destruction and presentation work
     * from device polling. A NON-blocking poll retires the per-frame instance
     * buffers and bind groups released above — that is why an occluded or
     * headless window (present skipped) holds steady RSS.
     *
     * But the surface-present submission is different: wgpu-native does not
     * retire ITS resources until that submission completes, and under Fifo
     * that is at the next vsync — one frame later. A non-blocking poll never
     * catches up, so on every presented frame ~7 KB is left queued and RSS
     * climbs about 0.4 MB/s while animating (~2 GB over 15 minutes). Only a
     * busy render loop that presents every frame hits this; an event-driven
     * UI that idles in waitEvents presents rarely and barely shows it, which
     * is why it went unnoticed until the animated example.
     *
     * On a presented frame, poll with wait=true so that submission is
     * retired here. This does NOT cost frame rate: Fifo already paces the
     * loop to vsync, and blocking here simply moves the wait that would
     * otherwise happen inside the next wgpuSurfaceGetCurrentTexture. When
     * nothing presented (occluded / headless), the cheap non-blocking poll
     * is enough and must stay non-blocking so those paths never stall. */
    rae_wgpu_poll(presented ? 1 : 0);
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
    if (g_g2d_metal_view) { SDL_Metal_DestroyView(g_g2d_metal_view); g_g2d_metal_view = NULL; }
#endif
    if (g_sdl_win) { SDL_DestroyWindow(g_sdl_win); g_sdl_win = NULL; }
}
