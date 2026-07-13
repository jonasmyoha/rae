/* gpu2d platform/window/input, design-coordinate, clip, and offscreen-target state. Raw SDL3/WebGPU calls stay C; app/window policy can migrate later.
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
static SDL_MetalView g_g2d_metal_view = NULL;
static WGPUSurface   g_g2d_surface = NULL;
static WGPUTextureFormat g_g2d_fmt = WGPUTextureFormat_BGRA8Unorm;

/* Coordinate system (#112): draw coords are in DESIGN units; the renderer
 * maps them to physical pixels via a per-frame scale + offset. Default
 * (design w/h <= 0) is identity — 1 unit = 1 physical px. setDesignResolution
 * opts into a fixed virtual canvas fitted into the window (DPI-independent). */
static double g_g2d_design_w = 0.0, g_g2d_design_h = 0.0;
static int    g_g2d_fit_mode = 0;   /* 0=fit/contain, 1=fill/cover, 2=stretch */
/* Per-frame mouse-wheel accumulator (reset + summed in pollClose, read by
 * gpu2d.wheelMove). Mirrors raylib's GetMouseWheelMove per-frame semantics. */
static float  g_g2d_wheel = 0.0f;
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
/* per-frame transient handles */
static WGPUCommandEncoder    g_g2d_enc = NULL;
static WGPURenderPassEncoder g_g2d_pass = NULL;

/* ---- Clip / scissor (#144) --------------------------------------------
 * A clip-rect stack in DESIGN units. Each queued box primitive, glyph, and
 * image records the current clip index (parallel arrays); at flush we set
 * the render-pass scissor per contiguous run of same-clip draws, so a run
 * is drawn with wgpuRenderPassEncoderDraw's firstInstance offset. Index 0
 * is the sentinel "no clip" (full framebuffer). pushClipRect intersects
 * with the parent so nested clips compose (a rounded card holding a scroll
 * list clips to the intersection). Reset each beginFrame. The rounded /
 * per-instance clip variant is #118; this is the axis-aligned fast path. */
#define RAE_G2D_MAX_CLIPS 256
typedef struct { float x, y, w, h, radius; int full; } RaeG2dClip;
static RaeG2dClip g_g2d_clips[RAE_G2D_MAX_CLIPS];
static int g_g2d_clip_count = 1;         /* [0] = full sentinel */
static int g_g2d_clip_stack[RAE_G2D_MAX_CLIPS];
static int g_g2d_clip_sp = 0;            /* stack depth */
static int g_g2d_cur_clip = 0;           /* current clip index */
/* parallel clip index per box primitive */
static int* g_g2d_prim_clip = NULL;
static int  g_g2d_prim_clip_cap = 0;     /* in prims */
/* parallel clip index per glyph, per atlas */
static int* g_g2d_text_clip[RAE_SDF_MAX_ATLAS];
static int  g_g2d_text_clip_cap[RAE_SDF_MAX_ATLAS];

static float rae_g2d_maxf(float a, float b) { return a > b ? a : b; }
static float rae_g2d_minf(float a, float b) { return a < b ? a : b; }

static void rae_g2d_clip_reset(void) {
    g_g2d_clips[0].full = 1;
    g_g2d_clips[0].x = 0.0f; g_g2d_clips[0].y = 0.0f;
    g_g2d_clips[0].w = 0.0f; g_g2d_clips[0].h = 0.0f;
    g_g2d_clips[0].radius = 0.0f;
    g_g2d_clip_count = 1;
    g_g2d_clip_sp = 0;
    g_g2d_cur_clip = 0;
}

static void rae_g2d_prim_clip_ensure(int prims) {
    if (prims <= g_g2d_prim_clip_cap) return;
    int cap = g_g2d_prim_clip_cap ? g_g2d_prim_clip_cap : 64;
    while (cap < prims) cap *= 2;
    g_g2d_prim_clip = (int*)realloc(g_g2d_prim_clip, (size_t)cap * sizeof(int));
    g_g2d_prim_clip_cap = cap;
}

static void rae_g2d_text_clip_ensure(int ai, int glyphs) {
    if (glyphs <= g_g2d_text_clip_cap[ai]) return;
    int cap = g_g2d_text_clip_cap[ai] ? g_g2d_text_clip_cap[ai] : 256;
    while (cap < glyphs) cap *= 2;
    g_g2d_text_clip[ai] = (int*)realloc(g_g2d_text_clip[ai], (size_t)cap * sizeof(int));
    g_g2d_text_clip_cap[ai] = cap;
}

/* Resolve a clip index to a framebuffer-pixel scissor rect (via the design→
 * physical xform) and set it on the active pass, clamped to the attachment. */
static void rae_g2d_set_scissor(int clipidx) {
    if (!g_g2d_pass) return;
    float xf[8]; rae_g2d_compute_xform(xf);
    float physW = xf[0], physH = xf[1], sx = xf[2], sy = xf[3], ox = xf[4], oy = xf[5];
    float x0, y0, x1, y1;
    if (clipidx <= 0 || clipidx >= g_g2d_clip_count || g_g2d_clips[clipidx].full) {
        x0 = 0.0f; y0 = 0.0f; x1 = physW; y1 = physH;
    } else {
        RaeG2dClip* c = &g_g2d_clips[clipidx];
        x0 = c->x * sx + ox;            y0 = c->y * sy + oy;
        x1 = (c->x + c->w) * sx + ox;   y1 = (c->y + c->h) * sy + oy;
    }
    x0 = rae_g2d_maxf(0.0f, x0); y0 = rae_g2d_maxf(0.0f, y0);
    x1 = rae_g2d_minf(physW, x1); y1 = rae_g2d_minf(physH, y1);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    uint32_t ix = (uint32_t)(x0 + 0.5f), iy = (uint32_t)(y0 + 0.5f);
    uint32_t iw = (uint32_t)(x1 - x0 + 0.5f), ih = (uint32_t)(y1 - y0 + 0.5f);
    uint32_t pw = (uint32_t)(physW + 0.5f), ph = (uint32_t)(physH + 0.5f);
    if (ix > pw) ix = pw;
    if (iy > ph) iy = ph;
    if (ix + iw > pw) iw = pw - ix;
    if (iy + ih > ph) ih = ph - iy;
    wgpuRenderPassEncoderSetScissorRect(g_g2d_pass, ix, iy, iw, ih);
}

static void rae_g2d_push_clip(double x, double y, double w, double h, double radius) {
    RaeG2dClip child; child.full = 0; child.radius = (float)radius;
    RaeG2dClip* parent = &g_g2d_clips[g_g2d_cur_clip];
    if (parent->full) {
        child.x = (float)x; child.y = (float)y; child.w = (float)w; child.h = (float)h;
    } else {
        float px0 = rae_g2d_maxf(parent->x, (float)x);
        float py0 = rae_g2d_maxf(parent->y, (float)y);
        float px1 = rae_g2d_minf(parent->x + parent->w, (float)(x + w));
        float py1 = rae_g2d_minf(parent->y + parent->h, (float)(y + h));
        child.x = px0; child.y = py0;
        child.w = rae_g2d_maxf(0.0f, px1 - px0);
        child.h = rae_g2d_maxf(0.0f, py1 - py0);
        /* Keep the larger of the two radii — a rounded child inside a
         * rectangular parent still wants its own rounding; the scissor
         * bbox already enforces the parent's straight edges. */
        child.radius = rae_g2d_maxf((float)radius, parent->radius);
    }
    int idx = g_g2d_cur_clip;
    if (g_g2d_clip_count < RAE_G2D_MAX_CLIPS) {
        idx = g_g2d_clip_count++;
        g_g2d_clips[idx] = child;
    }
    if (g_g2d_clip_sp < RAE_G2D_MAX_CLIPS) g_g2d_clip_stack[g_g2d_clip_sp++] = g_g2d_cur_clip;
    g_g2d_cur_clip = idx;
}

/* Fill an 8-float box-clip uniform: [x,y,w,h] design units + [radius,enabled].
 * `enabled` is set only for a rounded clip — a rectangular clip is handled by
 * the scissor alone, so its SDF stays off. */
static void rae_g2d_fill_clip_uniform(int clipidx, float* cu) {
    if (clipidx > 0 && clipidx < g_g2d_clip_count
        && !g_g2d_clips[clipidx].full && g_g2d_clips[clipidx].radius > 0.0f) {
        RaeG2dClip* c = &g_g2d_clips[clipidx];
        cu[0] = c->x; cu[1] = c->y; cu[2] = c->w; cu[3] = c->h;
        cu[4] = c->radius; cu[5] = 1.0f; cu[6] = 0.0f; cu[7] = 0.0f;
    } else {
        for (int i = 0; i < 8; i++) cu[i] = 0.0f;
    }
}

void rae_ext_gpu2d_pushClipRect(double x, double y, double w, double h) {
    rae_g2d_push_clip(x, y, w, h, 0.0);
}

/* #118: rounded clip. The box pipeline applies the rounded-rect SDF in the
 * fragment shader (analytic AA on the corners); the axis-aligned scissor
 * (#144) still bounds all pipelines to the clip bbox. */
void rae_ext_gpu2d_pushClipRoundedRect(double x, double y, double w, double h, double radius) {
    rae_g2d_push_clip(x, y, w, h, radius);
}

void rae_ext_gpu2d_popClipRect(void) {
    if (g_g2d_clip_sp > 0) {
        g_g2d_cur_clip = g_g2d_clip_stack[--g_g2d_clip_sp];
    } else {
        g_g2d_cur_clip = 0;
    }
}

/* Frames render to this persistent OFFSCREEN texture, not directly to the
 * surface drawable. At endFrame we read it back for screenshots and copy it to
 * the surface drawable for present *best-effort* — so rendering + headless
 * screenshots work even when the OS won't vend a drawable (window occluded /
 * display asleep / headless), where wgpuSurfaceGetCurrentTexture returns no
 * texture. */
static WGPUTexture     g_g2d_off_tex = NULL;
static WGPUTextureView g_g2d_off_view = NULL;
static int g_g2d_off_w = 0, g_g2d_off_h = 0;

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
    /* (Re)create the offscreen render target at the new size. */
    if (g_g2d_off_w != pw || g_g2d_off_h != ph || !g_g2d_off_tex) {
        if (g_g2d_off_view) { wgpuTextureViewRelease(g_g2d_off_view); g_g2d_off_view = NULL; }
        if (g_g2d_off_tex)  { wgpuTextureRelease(g_g2d_off_tex);  g_g2d_off_tex = NULL; }
        WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = (uint32_t)pw; td.size.height = (uint32_t)ph; td.size.depthOrArrayLayers = 1;
        td.format = g_g2d_fmt; td.mipLevelCount = 1; td.sampleCount = 1;
        g_g2d_off_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
        g_g2d_off_view = wgpuTextureCreateView(g_g2d_off_tex, NULL);
        g_g2d_off_w = pw; g_g2d_off_h = ph;
    }
}

void rae_ext_gpu2d_initWindow(int64_t width, int64_t height, rae_String title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[gpu2d] SDL init failed: %s\n", SDL_GetError());
        return;
    }
    const char* t = title.data ? (const char*)title.data : "Rae (GPU 2D)";
    g_sdl_win = SDL_CreateWindow(t, (int)width, (int)height,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_sdl_win) { fprintf(stderr, "[gpu2d] window failed: %s\n", SDL_GetError()); return; }
    SDL_RaiseWindow(g_sdl_win);
    g_g2d_metal_view = SDL_Metal_CreateView(g_sdl_win);
    if (!g_g2d_metal_view) { fprintf(stderr, "[gpu2d] metal view failed: %s\n", SDL_GetError()); return; }
    void* layer = SDL_Metal_GetLayer(g_g2d_metal_view);

    if (!rae_wgpu_init()) { fprintf(stderr, "[gpu2d] wgpu init failed\n"); return; }

    WGPUSurfaceSourceMetalLayer ms; memset(&ms, 0, sizeof(ms));
    ms.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    ms.layer = layer;
    WGPUSurfaceDescriptor sd; memset(&sd, 0, sizeof(sd));
    sd.nextInChain = &ms.chain;
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
}

/* Pump the OS event queue once per frame AND record input state into the
 * shared SDL3 input arrays (g_sdl_mouse / g_sdl_keydown / g_sdl_pressed) plus
 * the wheel accumulator, so gpu2d apps get working mouse/keyboard/wheel input.
 * (The bare SDL3 backend records this in sdl3_shouldClose, which the gpu2d
 * window path never calls — hence the duplication here.) Edge state
 * (g_sdl_pressed) and the wheel delta are reset each call so they describe
 * only this frame. */
rae_Bool rae_ext_gpu2d_pollClose(void) {
    memset(g_sdl_pressed, 0, sizeof(g_sdl_pressed));
    memset(g_sdl_mouse_pressed, 0, sizeof(g_sdl_mouse_pressed));
    memset(g_sdl_mouse_released, 0, sizeof(g_sdl_mouse_released));
    g_g2d_wheel = 0.0f;
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
    if (g_sdl_headless_ms > 0 && (rae_ext_nowMs() - g_sdl_start_ms) >= g_sdl_headless_ms) return 1;
    return 0;
}

/* Block up to timeoutSec for the next OS event (or wake immediately if one is
 * already queued), leaving events in the queue for the following pollClose to
 * drain. Passing NULL means SDL doesn't dequeue the event. This is the idle
 * half of the hybrid loop: busy-render while animating, park here when idle so
 * the app sits at ~0% CPU until input arrives. timeoutSec <= 0 returns at once. */
void rae_ext_gpu2d_waitEvents(double timeoutSec) {
    int ms = (int)(timeoutSec * 1000.0);
    if (ms < 0) ms = 0;
    SDL_WaitEventTimeout(NULL, ms);
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
double rae_ext_gpu2d_pointerX(void) { double x, y; rae_g2d_pointer_design(&x, &y); return x; }
double rae_ext_gpu2d_pointerY(void) { double x, y; rae_g2d_pointer_design(&x, &y); return y; }
/* Left mouse button held this frame (button index 1 in SDL). */
rae_Bool rae_ext_gpu2d_pointerDown(void) { return g_sdl_mouse[SDL_BUTTON_LEFT] != 0; }
rae_Bool rae_ext_gpu2d_pointerPressed(void) { return g_sdl_mouse_pressed[SDL_BUTTON_LEFT] != 0; }
rae_Bool rae_ext_gpu2d_pointerReleased(void) { return g_sdl_mouse_released[SDL_BUTTON_LEFT] != 0; }
/* Per-frame wheel delta (positive = wheel/scroll up). */
double rae_ext_gpu2d_wheelMove(void) { return (double)g_g2d_wheel; }

void rae_ext_gpu2d_setMouseCursor(int64_t kind) {
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
double rae_ext_gpu2d_nowSeconds(void) { return (double)rae_ext_nowMs() / 1000.0; }

int64_t rae_ext_gpu2d_windowWidth(void) { return g_sdl_w; }
int64_t rae_ext_gpu2d_windowHeight(void) { return g_sdl_h; }
void rae_ext_gpu2d_setWindowPosition(int64_t x, int64_t y) {
    if (g_sdl_win) SDL_SetWindowPosition(g_sdl_win, (int)x, (int)y);
}
int64_t rae_ext_gpu2d_windowPositionX(void) {
    int x = 0, y = 0;
    if (g_sdl_win) SDL_GetWindowPosition(g_sdl_win, &x, &y);
    (void)y;
    return (int64_t)x;
}
int64_t rae_ext_gpu2d_windowPositionY(void) {
    int x = 0, y = 0;
    if (g_sdl_win) SDL_GetWindowPosition(g_sdl_win, &x, &y);
    (void)x;
    return (int64_t)y;
}
/* True once per OS resize (edge-triggered): returns the pending flag and
 * clears it, so the app rebuilds its layout extent for the new window. */
rae_Bool rae_ext_gpu2d_windowResized(void) {
    rae_Bool r = (rae_Bool)g_g2d_win_resized;
    g_g2d_win_resized = 0;
    return r;
}

/* Coordinate system (#112). */
void rae_ext_gpu2d_setDesignResolution(double w, double h, int64_t fit) {
    g_g2d_design_w = w; g_g2d_design_h = h; g_g2d_fit_mode = (int)fit;
}
double rae_ext_gpu2d_designWidth(void)  { return (g_g2d_design_w > 0.0) ? g_g2d_design_w : (double)g_sdl_w; }
double rae_ext_gpu2d_designHeight(void) { return (g_g2d_design_h > 0.0) ? g_g2d_design_h : (double)g_sdl_h; }
double rae_ext_gpu2d_dpr(void) {
    int lw = 0, lh = 0; if (g_sdl_win) SDL_GetWindowSize(g_sdl_win, &lw, &lh);
    (void)lh; return (lw > 0) ? (double)g_sdl_w / (double)lw : 1.0;
}

