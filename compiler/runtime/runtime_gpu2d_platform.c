/* gpu2d platform/window/input, design-coordinate, and offscreen-target state. Raw SDL3/WebGPU calls stay C; app/window policy can migrate later.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* gpu2d SDL3/WebGPU renderer implementation. Raw GPU calls stay C; render/resource policy is a future Rae migration candidate.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* ============================================================
 * GPU 2D renderer surface — lib/gpu2d.rae (#109,
 * docs/webgpu-2d-ui-renderer.md). Wraps the SDL3 window's
 * CAMetalLayer as a wgpu surface and presents frames on the GPU
 * (no CPU framebuffer round-trip, unlike sdl3.updatePixels).
 * Needs both SDL3 (window) and wgpu-native (surface); main.c links
 * both when lib/gpu2d.rae is imported. Window globals (g_sdl_win,
 * g_sdl_w/h) are shared with the SDL3 block so sdl3 input works.
 * Tier 0 slice: window + clear-colour present.
 * ============================================================ */
#ifndef __EMSCRIPTEN__
static SDL_MetalView g_g2d_metal_view = NULL;
#endif
static WGPUSurface   g_g2d_surface = NULL;
static WGPUTextureFormat g_g2d_fmt = WGPUTextureFormat_BGRA8Unorm;

/* Coordinate system (#112): draw coords are in DESIGN units; the renderer
 * maps them to physical pixels via a per-frame scale + offset. Default
 * (design w/h <= 0) is identity — 1 unit = 1 physical px. setDesignResolution
 * opts into a fixed virtual canvas fitted into the window (DPI-independent). */
static double g_g2d_design_w = 0.0, g_g2d_design_h = 0.0;
static int    g_g2d_fit_mode = 0;   /* 0=fit/contain, 1=fill/cover, 2=stretch */

/* Multitouch (#526): active fingers tracked from SDL_EVENT_FINGER_*, so a UI can
 * drive a virtual joystick with one thumb and rotate the camera with another.
 * Positions are window-normalized (0..1); exposed to Rae in design units. */
#define RAE_G2D_MAX_TOUCH 10
static struct { SDL_FingerID id; float nx, ny; unsigned char pressed; } g_g2d_touch[RAE_G2D_MAX_TOUCH];
static int g_g2d_touch_n = 0;
/* Per-frame mouse-wheel accumulator (reset + summed in pollClose, read by
 * gpu2d.wheelMove). Mirrors raylib's GetMouseWheelMove per-frame semantics. */
static float  g_g2d_wheel = 0.0f;
/* #976: UTF-8 text typed since the last poll (SDL_EVENT_TEXT_INPUT), drained
 * once per frame by rae_ext_Gpu2d_textInput. Backspace/enter arrive as key
 * edges (Sdl3.isKeyPressed) — this carries only the characters. */
static char   g_g2d_text_input[256];
static size_t g_g2d_text_input_len = 0;
/* Set when the OS reports a window resize; consumed (cleared) once by
 * gpu2d.windowResized() so the app rebuilds its layout for the new size. */
static int    g_g2d_win_resized = 0;
static int    g_g2d_cursor_kind = -1;
static SDL_Cursor* g_g2d_cursors[7] = {0};
/* Last endFrame surface-present result. Rendering always targets the offscreen
 * texture first, but startup/occlusion can make the surface drawable
 * unavailable; apps use this to keep their first visible frame dirty. */
static int    g_g2d_last_present_ok = 0;

/* Fill `out` (8 floats = 2*vec4): (physW,physH,scaleX,scaleY),(offX,offY,0,0). */
static void rae_g2d_compute_xform(float* out) {
    float physW = (float)g_sdl_w, physH = (float)g_sdl_h;
    float scaleX = 1.0f, scaleY = 1.0f, offX = 0.0f, offY = 0.0f;
    if (g_g2d_design_w > 0.0 && g_g2d_design_h > 0.0) {
        float dW = (float)g_g2d_design_w, dH = (float)g_g2d_design_h;
        float sx = physW / dW, sy = physH / dH;
        if (g_g2d_fit_mode == 2) { scaleX = sx; scaleY = sy; }       /* stretch */
        else {
            float s = (g_g2d_fit_mode == 1) ? (sx > sy ? sx : sy)    /* fill/cover */
                                            : (sx < sy ? sx : sy);   /* fit/contain */
            scaleX = s; scaleY = s;
            offX = (physW - dW * s) * 0.5f;
            offY = (physH - dH * s) * 0.5f;
        }
    }
    out[0] = physW; out[1] = physH; out[2] = scaleX; out[3] = scaleY;
    out[4] = offX;  out[5] = offY;  out[6] = 0.0f;   out[7] = 0.0f;
}
/* #915: the clip stack, per-run scissor and the rounded-clip uniform live on
 * the Rae canvas (lib/Gpu2dCanvas.rae); C only exposes the transform. */
void rae_g2d_xform(float* out) { if (out) rae_g2d_compute_xform(out); }

/* Frames render to this persistent OFFSCREEN texture, not directly to the
 * surface drawable. At endFrame we read it back for screenshots and copy it to
 * the surface drawable for present *best-effort* — so rendering + headless
 * screenshots work even when the OS won't vend a drawable (window occluded /
 * display asleep / headless), where wgpuSurfaceGetCurrentTexture returns no
 * texture. */
/* #920: the offscreen presentable target is the Rae canvas's texture (built
 * to the configured surface size, rebuilt on resize); C only configures the
 * surface and reports its size/format. */
static void rae_g2d_configure(int pw, int ph) {
    if (!g_g2d_surface || pw <= 0 || ph <= 0) return;
    WGPUSurfaceConfiguration cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.device = g_wgpu_dev;
    cfg.format = g_g2d_fmt;
    /* CopyDst so we can copy our offscreen render into the drawable to present. */
    cfg.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
    cfg.width = (uint32_t)pw;
    cfg.height = (uint32_t)ph;
    cfg.presentMode = WGPUPresentMode_Fifo;
    cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(g_g2d_surface, &cfg);
    g_sdl_w = pw; g_sdl_h = ph;
}

/* The configured surface, for the canvas that owns the presentable target:
 * ready once the device + surface exist and have a size; the physical size the
 * target must have (the format is rae_g2d_format). */
int64_t rae_g2d_surface_ready(void) { return (g_wgpu_dev && g_g2d_surface && g_sdl_w > 0 && g_sdl_h > 0) ? 1 : 0; }
int64_t rae_g2d_surface_width(void)  { return (int64_t)g_sdl_w; }
int64_t rae_g2d_surface_height(void) { return (int64_t)g_sdl_h; }

/* Is the window in a state where rendering + presenting is worthwhile? False
 * when hidden / minimized / occluded (app switched away, another window fully
 * covers it, or the dock icon is all that is left). Rendering while dark is pure
 * waste and, on macOS specifically, leaks: the CAMetalLayer stops recycling its
 * drawable pool while backgrounded (so every acquired drawable is a fresh
 * IOSurface), and the deferred pipeline's per-frame GPU allocations pile up in
 * the driver. Apps should skip their render + park in waitEvents when this is
 * false; the present path also refuses to acquire a drawable while dark. */
int rae_g2d_window_visible(void) {
    if (!g_sdl_win) return 1;
    SDL_WindowFlags flags = SDL_GetWindowFlags(g_sdl_win);
    return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) ? 0 : 1;
}

/* ---- window MOVED, as an event flag ----------------------------------
 *
 * The mirror of the resize flag above, and that is all the C here does. An
 * earlier version of this file also owned window-geometry PERSISTENCE — it
 * chose the file path, decided when to write, and restored on startup before
 * any Rae code ran. That was the wrong place for it twice over: it is app
 * behaviour written in C, and it was invisible, so every app got it whether or
 * not its author asked for anything. Persistence now lives in
 * lib/ui/window_geometry.rae and an app that does not call it does not have it.
 *
 * What C keeps is what only C can do: notice the SDL event. */
static int g_g2d_win_moved = 0;

/* True under ANY of the headless-budget envs (#983). Checking only
 * RAE_UI_HEADLESS would leave a run that sets just RAE_SDL_HEADLESS_MS /
 * RAE_HEADLESS_FRAMES (both legitimate ways to bound a capture — see
 * examples/111/112's own "BOTH headless vars" note) still stealing focus.
 * A non-empty value of any of the three counts, matching Headless.rae's
 * `headlessIsActive`. */
static int rae_g2d_headless_requested(void) {
    const char* h = getenv("RAE_UI_HEADLESS");
    if (h && h[0]) return 1;
    const char* hm = getenv("RAE_SDL_HEADLESS_MS");
    if (hm && hm[0]) return 1;
    const char* hf = getenv("RAE_HEADLESS_FRAMES");
    if (hf && hf[0]) return 1;
    return 0;
}

/* The same predicate for Rae (gpu2d.headlessRequested, #988). The render
 * decision in lib/Gpu2d.rae (shouldRenderFrame / renderGate) skips rendering
 * while the window is not visible -- the macOS backgrounded-drawable leak fix
 * -- but a headless window is created SDL_WINDOW_HIDDEN on purpose, so that
 * rule read "hidden" as "backgrounded" and a headless run never rendered a
 * single frame (it parked in waitEvents on a 250ms heartbeat until its budget
 * expired). Headless renders to the offscreen target and screenshots THAT;
 * the drawable it must never touch is already refused by the present path
 * (rae_g2d_present checks rae_g2d_window_visible itself), so telling the app
 * to go ahead and render costs nothing and leaks nothing. */
rae_Bool rae_ext_Gpu2d_headlessRequested(void) {
    return rae_g2d_headless_requested() ? 1 : 0;
}

void rae_ext_Gpu2d_initWindow(int64_t width, int64_t height, rae_String title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[gpu2d] SDL init failed: %s\n", SDL_GetError());
        return;
    }
    int headless = rae_g2d_headless_requested();
    const char* t = title.data ? (const char*)title.data : "Rae (GPU 2D)";
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifndef __EMSCRIPTEN__
    flags |= SDL_WINDOW_METAL;
#endif
    /* A headless run renders to the offscreen target and screenshots THAT
     * (rae_g2d_present, below) regardless of window visibility — the present-
     * to-drawable step is already best-effort and skipped for a hidden/
     * occluded window (the same path used to avoid the backgrounded-macOS
     * drawable leak) — so SDL_WINDOW_HIDDEN costs nothing here and keeps the
     * window from ever taking OS focus or Space. */
    if (headless) flags |= SDL_WINDOW_HIDDEN;
    g_sdl_win = SDL_CreateWindow(t, (int)width, (int)height, flags);
    if (!g_sdl_win) { fprintf(stderr, "[gpu2d] window failed: %s\n", SDL_GetError()); return; }
    if (!headless) {
        SDL_RaiseWindow(g_sdl_win);
    }
    /* Text input is NOT started here. SDL3 delivers SDL_EVENT_TEXT_INPUT only
     * while text input is started for the window, and starting it makes the
     * whole window a text field to the OS: on macOS, holding a key then opens
     * the press-and-hold accent popover (e -> è é ê …) — in a game that reads
     * WASD. An app turns it on while a text field is on screen
     * (rae_ext_Gpu2d_setTextInput, Gpu2d.setTextInput) and off again. */
#ifndef __EMSCRIPTEN__
    g_g2d_metal_view = SDL_Metal_CreateView(g_sdl_win);
    if (!g_g2d_metal_view) { fprintf(stderr, "[gpu2d] metal view failed: %s\n", SDL_GetError()); return; }
    void* layer = SDL_Metal_GetLayer(g_g2d_metal_view);
#endif

    if (!rae_wgpu_init()) { fprintf(stderr, "[gpu2d] wgpu init failed\n"); return; }

#ifdef __EMSCRIPTEN__
    /* "#canvas" IS REQUIRED, not a default worth parameterizing.
     *
     * Emscripten special-cases this one selector: it is the canvas the runtime
     * treats as "the" canvas (Module.canvas). A surface created from any other
     * selector is accepted and then presents nothing -- measured, with the
     * selector plumbed correctly through to here and the element present and
     * correctly sized: id="canvas" renders, id="raeCanvas" and
     * id="example-viewer-canvas" produce a blank canvas and no error.
     *
     * So a host page embedding the modularized bundle must NAME ITS CANVAS
     * "canvas". Devtools' example viewer did not, which is why an embedded 3D
     * example started, logged normally and stayed blank. Making this
     * configurable was tried and removed: a knob whose every value but one
     * silently fails is worse than the constant it replaced.
     */
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector cs; memset(&cs, 0, sizeof(cs));
    cs.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    cs.selector = rae_wgpu_sv("#canvas");
#else
    WGPUSurfaceSourceMetalLayer ms; memset(&ms, 0, sizeof(ms));
    ms.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    ms.layer = layer;
#endif
    WGPUSurfaceDescriptor sd; memset(&sd, 0, sizeof(sd));
#ifdef __EMSCRIPTEN__
    sd.nextInChain = &cs.chain;
#else
    sd.nextInChain = &ms.chain;
#endif
    g_g2d_surface = wgpuInstanceCreateSurface(g_wgpu_inst, &sd);
    if (!g_g2d_surface) { fprintf(stderr, "[gpu2d] surface failed\n"); return; }

    WGPUSurfaceCapabilities caps; memset(&caps, 0, sizeof(caps));
    wgpuSurfaceGetCapabilities(g_g2d_surface, g_wgpu_adapter, &caps);
    if (caps.formatCount > 0) {
        g_g2d_fmt = caps.formats[0];
        for (size_t i = 0; i < caps.formatCount; i++) {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm) { g_g2d_fmt = WGPUTextureFormat_BGRA8Unorm; break; }
        }
    }
    int pw = (int)width, ph = (int)height;
    SDL_GetWindowSizeInPixels(g_sdl_win, &pw, &ph);
    rae_g2d_configure(pw, ph);

    /* Test hook: RAE_GPU2D_TEST_RESIZE=WxH resizes the window (logical
     * points) just after boot so the resize path can be exercised
     * headlessly. Fires a WINDOW_PIXEL_SIZE_CHANGED on the next poll. */
    const char* trs = getenv("RAE_GPU2D_TEST_RESIZE");
    if (trs) {
        int rw = 0, rh = 0;
        if (sscanf(trs, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
            SDL_SetWindowSize(g_sdl_win, rw, rh);
        }
    }

    g_sdl_start_ms = rae_ext_nowMs();
    const char* hm = getenv("RAE_SDL_HEADLESS_MS");
    if (hm) g_sdl_headless_ms = (int64_t)atoll(hm);
    /* RAE_HEADLESS_FRAMES runs an EXACT number of frames instead of an
     * elapsed wall-clock budget. A duration budget lets frame count vary
     * with machine speed, so screenshots are not reproducible; a frame
     * budget plus RAE_FIXED_DT makes a headless capture byte-identical
     * across runs and across refactors. */
    const char* hf = getenv("RAE_HEADLESS_FRAMES");
    if (hf) g_sdl_headless_frames = (int64_t)atoll(hf);
}

/* Pump the OS event queue once per frame AND record input state into the
 * shared SDL3 input arrays (g_sdl_mouse / g_sdl_keydown / g_sdl_pressed) plus
 * the wheel accumulator, so gpu2d apps get working mouse/keyboard/wheel input.
 * (The bare SDL3 backend records this in sdl3_shouldClose, which the gpu2d
 * window path never calls — hence the duplication here.) Edge state
 * (g_sdl_pressed) and the wheel delta are reset each call so they describe
 * only this frame. */
#ifdef __EMSCRIPTEN__
static rae_Bool g_rae_browser_stop_requested = 0;

EMSCRIPTEN_KEEPALIVE void rae_browser_request_stop(void) {
    g_rae_browser_stop_requested = 1;
}
#endif

/* Defined in runtime_gpu2d_frame.c / runtime_gpu3d.c: has any frame reached the
 * surface yet? Used only to explain a headless run that drew nothing. */
extern rae_Bool rae_frame_presented_any(void);

rae_Bool rae_ext_Gpu2d_pollClose(void) {
#ifdef __EMSCRIPTEN__
    /* Browser WebGPU presents at requestAnimationFrame boundaries. Asyncify
     * lets the current Rae loop await that boundary without source changes. */
    rae_browser_next_frame();
    if (g_rae_browser_stop_requested) return 1;
#endif
    memset(g_sdl_pressed, 0, sizeof(g_sdl_pressed));
    memset(g_sdl_mouse_pressed, 0, sizeof(g_sdl_mouse_pressed));
    memset(g_sdl_mouse_released, 0, sizeof(g_sdl_mouse_released));
    for (int ti = 0; ti < g_g2d_touch_n; ti++) g_g2d_touch[ti].pressed = 0;   /* clear edge */
    g_g2d_wheel = 0.0f;
    g_g2d_text_input_len = 0; g_g2d_text_input[0] = 0;
    SDL_Event e;
    rae_Bool quit = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: quit = 1; break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit = 1; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED: {
                int pw = 0, ph = 0; SDL_GetWindowSizeInPixels(g_sdl_win, &pw, &ph);
                rae_g2d_configure(pw, ph);
                g_g2d_win_resized = 1;   /* consumed once by gpu2d.windowResized() */
                break;
            }
            case SDL_EVENT_WINDOW_MOVED:
                g_g2d_win_moved = 1;   /* consumed once by gpu2d.windowMoved() */
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) quit = 1;
                if (e.key.scancode < SDL_SCANCODE_COUNT) {
                    g_sdl_keydown[e.key.scancode] = 1;
                    if (!e.key.repeat) g_sdl_pressed[e.key.scancode] = 1;  /* edge */
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (e.key.scancode < SDL_SCANCODE_COUNT) g_sdl_keydown[e.key.scancode] = 0;
                break;
            case SDL_EVENT_TEXT_INPUT: {
                size_t n = strlen(e.text.text);
                if (g_g2d_text_input_len + n < sizeof(g_g2d_text_input)) {
                    memcpy(g_g2d_text_input + g_g2d_text_input_len, e.text.text, n);
                    g_g2d_text_input_len += n;
                    g_g2d_text_input[g_g2d_text_input_len] = 0;
                }
                break;
            }
            case SDL_EVENT_FINGER_DOWN:
                if (g_g2d_touch_n < RAE_G2D_MAX_TOUCH) {
                    g_g2d_touch[g_g2d_touch_n].id = e.tfinger.fingerID;
                    g_g2d_touch[g_g2d_touch_n].nx = e.tfinger.x;
                    g_g2d_touch[g_g2d_touch_n].ny = e.tfinger.y;
                    g_g2d_touch[g_g2d_touch_n].pressed = 1;   /* edge, cleared next frame */
                    g_g2d_touch_n++;
                }
                break;
            case SDL_EVENT_FINGER_MOTION:
                for (int ti = 0; ti < g_g2d_touch_n; ti++) {
                    if (g_g2d_touch[ti].id == e.tfinger.fingerID) {
                        g_g2d_touch[ti].nx = e.tfinger.x;
                        g_g2d_touch[ti].ny = e.tfinger.y;
                        break;
                    }
                }
                break;
            case SDL_EVENT_FINGER_UP:
                for (int ti = 0; ti < g_g2d_touch_n; ti++) {
                    if (g_g2d_touch[ti].id == e.tfinger.fingerID) {
                        g_g2d_touch[ti] = g_g2d_touch[g_g2d_touch_n - 1];
                        g_g2d_touch_n--;
                        break;
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button < 8) {
                    g_sdl_mouse[e.button.button] = 1;
                    g_sdl_mouse_pressed[e.button.button] = 1;
                }
                if (!g_sdl_mouse_captured) { SDL_CaptureMouse(true); g_sdl_mouse_captured = true; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (e.button.button < 8) {
                    g_sdl_mouse[e.button.button] = 0;
                    g_sdl_mouse_released[e.button.button] = 1;
                }
                bool any = false;
                for (int b = 0; b < 8; b++) if (g_sdl_mouse[b]) any = true;
                if (!any && g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                g_g2d_wheel += e.wheel.y;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                memset(g_sdl_keydown, 0, sizeof(g_sdl_keydown));
                memset(g_sdl_mouse, 0, sizeof(g_sdl_mouse));
                memset(g_sdl_mouse_pressed, 0, sizeof(g_sdl_mouse_pressed));
                memset(g_sdl_mouse_released, 0, sizeof(g_sdl_mouse_released));
                if (g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            default: break;
        }
    }
    if (quit) return 1;
    if (g_sdl_headless_frames > 0) {
        if (g_sdl_frames_done >= g_sdl_headless_frames) return 1;
        g_sdl_frames_done++;
        return 0;   /* frame budget takes precedence over the ms budget */
    }
    if (g_sdl_headless_ms > 0) {
        int64_t now = rae_ext_nowMs();
        if (!rae_frame_presented_any()) {
            /* The budget is wall clock from WINDOW CREATION, so it normally
             * covers asset loading as well as the loop. But startup (asset
             * loading, shader pipeline build) can occasionally outrun that
             * budget on a loaded/shared machine even though the app goes on
             * to render fine (#986) -- the budget is meant to bound RENDER
             * time, not contention on the box. Give startup a generous grace
             * (see RAE_HEADLESS_STARTUP_GRACE_MS in runtime_image_sdl3.c)
             * before giving up: an app that still loads for longer than that
             * exits having drawn nothing, writes no screenshot and returns 0
             * -- which is indistinguishable from a broken renderer. Say what
             * actually happened rather than leaving a silent black run. */
            int64_t startupCap = g_sdl_headless_ms + RAE_HEADLESS_STARTUP_GRACE_MS;
            if (now - g_sdl_start_ms >= startupCap) {
                fprintf(stderr,
                    "warning: RAE_SDL_HEADLESS_MS=%lld elapsed (grace extended to %lldms) "
                    "before the first frame. Startup alone took longer than the budget "
                    "even with grace, so nothing rendered and no screenshot was written. "
                    "The budget is wall clock from window creation and includes asset "
                    "loading -- raise it.\n",
                    (long long)g_sdl_headless_ms, (long long)startupCap);
                fflush(stderr);
                return 1;
            }
            return 0;
        }
        /* Rendering has started: give the app its full configured budget
         * measured from the first presented frame, so a slow-starting run
         * still gets the number of rendered frames the example expects. */
        if (g_sdl_first_frame_ms == 0) g_sdl_first_frame_ms = now;
        if (now - g_sdl_first_frame_ms >= g_sdl_headless_ms) return 1;
    }
    return 0;
}

/* Block up to timeoutSec for the next OS event (or wake immediately if one is
 * already queued), leaving events in the queue for the following pollClose to
 * drain. Passing NULL means SDL doesn't dequeue the event. This is the idle
 * half of the hybrid loop: busy-render while animating, park here when idle so
 * the app sits at ~0% CPU until input arrives. timeoutSec <= 0 returns at once.
 *
 * Headless runs never receive real OS events (the window is hidden and has no
 * interactive session), so an app whose "animating" flag goes false before its
 * first/deterministic frame -- e.g. before RAE_SDL_HEADLESS_MS's own budget
 * check next runs -- would otherwise block here for lib/ui/EventLoop.rae's
 * idleCapSec (30s), a single OS call our headless-quit check in pollClose
 * cannot interrupt because it only runs BETWEEN loop iterations (#986: this,
 * not slow asset loading, is why widening RAE_SDL_HEADLESS_MS's own grace
 * period alone did not fix the flaky GPU3D/deferred gates -- the process was
 * stuck inside this single call, never returning to let pollClose re-check
 * the deadline). Clamp the wait to a short, frequent poll instead so a
 * headless run's own loop -- and therefore its RAE_SDL_HEADLESS_MS budget --
 * stays in control regardless of the app's idle policy. */
#define RAE_HEADLESS_WAIT_EVENTS_CAP_MS 20
void rae_ext_Gpu2d_waitEvents(float timeoutSec){
    int ms = (int)(timeoutSec * 1000.0);
    if (ms < 0) ms = 0;
    if (rae_g2d_headless_requested() && ms > RAE_HEADLESS_WAIT_EVENTS_CAP_MS) {
        ms = RAE_HEADLESS_WAIT_EVENTS_CAP_MS;
    }
    SDL_WaitEventTimeout(NULL, ms);
}

/* Thread-safe waker (#950, lib/ui/EventLoop.rae `wake`): post a user event so
 * a `waitEvents` blocked on the UI thread returns at once. A `spawn`'d worker
 * calls this right after it posts its result on a Channel, so an idle loop
 * reacts to the completion instead of noticing it on its next timeout tick.
 * SDL_PushEvent is documented thread-safe; SDL_EVENT_USER needs no
 * registration and the pump's default arm drops it unread. Before SDL is
 * initialised the push simply fails, which is the right no-op for a headless
 * run. Builds without an SDL3 window get the no-op in rae_runtime.c. */
void rae_ext_EventLoop_wake(void){
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    SDL_PushEvent(&ev);
}

/* Pointer position in DESIGN units (the same coordinate space drawRect etc.
 * take), so hit-testing matches what was drawn. SDL reports logical window
 * points; we scale to physical px (× dpr) then invert the design fit transform
 * (subtract the letterbox offset, divide by scale). */
static void rae_g2d_pointer_design(double* dx, double* dy) {
    float mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
    int lw = 0, lh = 0; if (g_sdl_win) SDL_GetWindowSize(g_sdl_win, &lw, &lh);
    double sclx = (lw > 0) ? (double)g_sdl_w / (double)lw : 1.0;
    double scly = (lh > 0) ? (double)g_sdl_h / (double)lh : 1.0;
    float xf[8]; rae_g2d_compute_xform(xf);
    double physX = (double)mx * sclx, physY = (double)my * scly;
    *dx = (xf[2] != 0.0f) ? (physX - xf[4]) / xf[2] : physX;
    *dy = (xf[3] != 0.0f) ? (physY - xf[5]) / xf[3] : physY;
}
float rae_ext_Gpu2d_pointerX(void){ double x, y; rae_g2d_pointer_design(&x, &y); return x; }
/* The characters typed since the last event poll, as one UTF-8 String. */
rae_String rae_ext_Gpu2d_textInput(void){ return rae_ext_rae_str_from_cstr(g_g2d_text_input); }

/* Start / stop SDL text input for the window: on while a text field is on
 * screen, off otherwise (see the note at window creation). Idempotent — the
 * app calls it every frame with its current state. Never started headless
 * (#983): a text-input window accepts stray terminal keystrokes during an
 * automated run. */
static int g_g2d_text_input_active = 0;
void rae_ext_Gpu2d_setTextInput(rae_Bool active) {
    if (!g_sdl_win) return;
    int want = active ? 1 : 0;
    if (want && rae_g2d_headless_requested()) want = 0;
    if (want == g_g2d_text_input_active) return;
    g_g2d_text_input_active = want;
    if (want) SDL_StartTextInput(g_sdl_win); else SDL_StopTextInput(g_sdl_win);
}
float rae_ext_Gpu2d_pointerY(void){ double x, y; rae_g2d_pointer_design(&x, &y); return y; }

/* Multitouch fingers in design units (#526). touchX/Y clamp to 0 out of range. */
static void rae_g2d_touch_design(int i, double* dx, double* dy) {
    float xf[8]; rae_g2d_compute_xform(xf);
    double physX = (double)g_g2d_touch[i].nx * (double)g_sdl_w;
    double physY = (double)g_g2d_touch[i].ny * (double)g_sdl_h;
    *dx = (xf[2] != 0.0f) ? (physX - xf[4]) / xf[2] : physX;
    *dy = (xf[3] != 0.0f) ? (physY - xf[5]) / xf[3] : physY;
}
int64_t rae_ext_Gpu2d_touchCount(void) { return (int64_t)g_g2d_touch_n; }
float rae_ext_Gpu2d_touchX(int64_t i) {
    if (i < 0 || i >= g_g2d_touch_n) return 0.0f;
    double x, y; rae_g2d_touch_design((int)i, &x, &y); return (float)x;
}
float rae_ext_Gpu2d_touchY(int64_t i) {
    if (i < 0 || i >= g_g2d_touch_n) return 0.0f;
    double x, y; rae_g2d_touch_design((int)i, &x, &y); return (float)y;
}
/* A stable per-finger id (SDL_FingerID) so a widget can grab a finger at press
 * and hold it for the whole drag, and whether it went down THIS frame. */
int64_t rae_ext_Gpu2d_touchId(int64_t i) {
    if (i < 0 || i >= g_g2d_touch_n) return -1;
    return (int64_t)g_g2d_touch[(int)i].id;
}
rae_Bool rae_ext_Gpu2d_touchPressed(int64_t i) {
    if (i < 0 || i >= g_g2d_touch_n) return 0;
    return g_g2d_touch[(int)i].pressed ? 1 : 0;
}

/* OS safe-area insets (notch / home indicator / rounded corners) in DESIGN units
 * (#525). SDL_GetWindowSafeArea reports the safe rect in window points; the inset
 * distances scale to physical px then design units the same way the pointer does. */
static void rae_g2d_safe_insets_design(double* it, double* ib, double* il, double* ir) {
    *it = *ib = *il = *ir = 0.0;
    if (!g_sdl_win) return;
    SDL_Rect safe;
    if (!SDL_GetWindowSafeArea(g_sdl_win, &safe)) return;
    int lw = 0, lh = 0; SDL_GetWindowSize(g_sdl_win, &lw, &lh);
    if (lw <= 0 || lh <= 0) return;
    double topL = (double)safe.y;
    double botL = (double)lh - (double)(safe.y + safe.h);
    double leftL = (double)safe.x;
    double rightL = (double)lw - (double)(safe.x + safe.w);
    double sx = (double)g_sdl_w / (double)lw, sy = (double)g_sdl_h / (double)lh;
    float xf[8]; rae_g2d_compute_xform(xf);
    double scaleX = (xf[2] != 0.0f) ? (double)xf[2] : 1.0;
    double scaleY = (xf[3] != 0.0f) ? (double)xf[3] : 1.0;
    *it = topL * sy / scaleY;
    *ib = botL * sy / scaleY;
    *il = leftL * sx / scaleX;
    *ir = rightL * sx / scaleX;
}
float rae_ext_Gpu2d_safeTop(void){ double t,b,l,r; rae_g2d_safe_insets_design(&t,&b,&l,&r); return (float)t; }
float rae_ext_Gpu2d_safeBottom(void){ double t,b,l,r; rae_g2d_safe_insets_design(&t,&b,&l,&r); return (float)b; }
float rae_ext_Gpu2d_safeLeft(void){ double t,b,l,r; rae_g2d_safe_insets_design(&t,&b,&l,&r); return (float)l; }
float rae_ext_Gpu2d_safeRight(void){ double t,b,l,r; rae_g2d_safe_insets_design(&t,&b,&l,&r); return (float)r; }
/* Left mouse button held this frame (button index 1 in SDL). */
rae_Bool rae_ext_Gpu2d_pointerDown(void) { return g_sdl_mouse[SDL_BUTTON_LEFT] != 0; }
rae_Bool rae_ext_Gpu2d_pointerPressed(void) { return g_sdl_mouse_pressed[SDL_BUTTON_LEFT] != 0; }
rae_Bool rae_ext_Gpu2d_pointerReleased(void) { return g_sdl_mouse_released[SDL_BUTTON_LEFT] != 0; }
/* Per-frame wheel delta (positive = wheel/scroll up). */
float rae_ext_Gpu2d_wheelMove(void){ return (double)g_g2d_wheel; }

void rae_ext_Gpu2d_setMouseCursor(int64_t kind) {
    if (!g_sdl_win) return;
    if (kind < 0 || kind > 6) kind = 0;
    if ((int)kind == g_g2d_cursor_kind) return;

    SDL_SystemCursor cursor = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (kind) {
        case 1: cursor = SDL_SYSTEM_CURSOR_POINTER; break;
        case 2: cursor = SDL_SYSTEM_CURSOR_TEXT; break;
        case 3: cursor = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
        case 4: cursor = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
        case 5: cursor = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
        case 6: cursor = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
        default: cursor = SDL_SYSTEM_CURSOR_DEFAULT; break;
    }

    if (!g_g2d_cursors[kind]) {
        g_g2d_cursors[kind] = SDL_CreateSystemCursor(cursor);
    }
    if (g_g2d_cursors[kind]) {
        SDL_SetCursor(g_g2d_cursors[kind]);
        g_g2d_cursor_kind = (int)kind;
    }
}
/* Monotonic wall-clock seconds since process start — for scroll timing without
 * pulling in the raylib-backed getTime. */
/* Deterministic virtual clock for reproducible rendering tests.
 *
 * Animated examples derive everything from nowSeconds(), so a headless
 * capture lands on an arbitrary frame and screenshots are not reproducible
 * — which makes byte-identical regression testing of the renderer
 * impossible. With RAE_FIXED_DT=<seconds> the clock instead advances by
 * exactly that step once per presented frame, so frame N is identical
 * across runs and across refactors.
 *
 * The base is an arbitrary fixed epoch rather than 0 so that code treating
 * the value as a real timestamp still sees a plausible one, and so f32
 * misuse of it stays as visible as it would be in production. */
static double g_g2d_fixed_dt = -1.0;   /* <0 = real clock */
static double g_g2d_virtual_now = 1700000000.0;
static bool   g_g2d_fixed_dt_read = false;

static void g2d_fixed_dt_init(void) {
    if (g_g2d_fixed_dt_read) return;
    g_g2d_fixed_dt_read = true;
    const char* e = getenv("RAE_FIXED_DT");
    if (e && *e) {
        double v = atof(e);
        if (v > 0.0) g_g2d_fixed_dt = v;
    }
}

/* Called once per presented frame by the frame path. */
void rae_g2d_tick_virtual_clock(void) {
    g2d_fixed_dt_init();
    if (g_g2d_fixed_dt > 0.0) g_g2d_virtual_now += g_g2d_fixed_dt;
}

double rae_ext_Gpu2d_nowSeconds(void){
    g2d_fixed_dt_init();
    if (g_g2d_fixed_dt > 0.0) return g_g2d_virtual_now;
    return (double)rae_ext_nowMs() / 1000.0;
}

int64_t rae_ext_Gpu2d_windowWidth(void) { return g_sdl_w; }
int64_t rae_ext_Gpu2d_windowHeight(void) { return g_sdl_h; }
void rae_ext_Gpu2d_setWindowPosition(int64_t x, int64_t y) {
    if (g_sdl_win) SDL_SetWindowPosition(g_sdl_win, (int)x, (int)y);
}
/* Counterpart to setWindowPosition, so an app can restore the whole
 * placement it saved. Restoring the position without the size drops the
 * window back in the right corner at the wrong shape, which looks more
 * broken than not restoring at all. */
void rae_ext_Gpu2d_setWindowSize(int64_t w, int64_t h) {
    if (!g_sdl_win) return;
    if (w <= 0 || h <= 0) return;
    SDL_SetWindowSize(g_sdl_win, (int)w, (int)h);
}
int64_t rae_ext_Gpu2d_windowPositionX(void) {
    int x = 0, y = 0;
    if (g_sdl_win) SDL_GetWindowPosition(g_sdl_win, &x, &y);
    (void)y;
    return (int64_t)x;
}
int64_t rae_ext_Gpu2d_windowPositionY(void) {
    int x = 0, y = 0;
    if (g_sdl_win) SDL_GetWindowPosition(g_sdl_win, &x, &y);
    (void)x;
    return (int64_t)y;
}
/* True once per OS resize (edge-triggered): returns the pending flag and
 * clears it, so the app rebuilds its layout extent for the new window. */
rae_Bool rae_ext_Gpu2d_windowResized(void) {
    rae_Bool r = (rae_Bool)g_g2d_win_resized;
    g_g2d_win_resized = 0;
    return r;
}

/* One-shot, same contract as windowResized: reading it clears it. */
rae_Bool rae_ext_Gpu2d_windowMoved(void) {
    int v = g_g2d_win_moved;
    g_g2d_win_moved = 0;
    return v ? 1 : 0;
}

/* Coordinate system (#112). */
void rae_ext_Gpu2d_setDesignResolution(float w, float h, int64_t fit){
    g_g2d_design_w = w; g_g2d_design_h = h; g_g2d_fit_mode = (int)fit;
}
float rae_ext_Gpu2d_designWidth(void){ return (g_g2d_design_w > 0.0) ? g_g2d_design_w : (double)g_sdl_w; }
float rae_ext_Gpu2d_designHeight(void){ return (g_g2d_design_h > 0.0) ? g_g2d_design_h : (double)g_sdl_h; }
float rae_ext_Gpu2d_dpr(void){
    int lw = 0, lh = 0; if (g_sdl_win) SDL_GetWindowSize(g_sdl_win, &lw, &lh);
    (void)lh; return (lw > 0) ? (double)g_sdl_w / (double)lw : 1.0;
}
