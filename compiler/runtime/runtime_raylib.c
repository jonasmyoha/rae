/* Legacy raylib and GLFW-backed renderer/window bindings. Retained for legacy examples; not a strategic migration target.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

#ifdef RAE_HAS_RAYLIB
/* Raylib wrappers for C backend */
#include <raylib.h>
#include <rlgl.h>
#if defined(__APPLE__)
/* Apple deprecated the entire OpenGL framework in 10.14 in favour
 * of Metal, but the symbols still link and work. raylib itself goes
 * through OpenGL → Metal under the hood here. Silence the noise. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

/* Phase 0 of the color-management plan (docs/color-management-plan.md).
 *
 * Raylib hands the OS a raw OpenGL framebuffer and walks away —
 * macOS WindowServer then interprets those bytes through whatever
 * the host display's profile is (Display P3 on every modern Mac).
 * sRGB-encoded PNG content displayed as P3 looks visibly over-
 * saturated, especially on stylised game art with bright primaries.
 *
 * The fix until the Metal backend lands is to tell macOS the
 * window's framebuffer is sRGB. WindowServer then does the right
 * sRGB → P3 conversion for free. Drives the existing Cocoa colour-
 * management path that Preview, Safari and Chrome already use.
 *
 * Pure C, no Objective-C compiler step: routes through the objc
 * runtime's C API (`objc_msgSend` etc.). Apple-only — non-Apple
 * builds compile this away. */
#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
static void rae_macos_set_window_srgb(void) {
    Class app_cls = objc_getClass("NSApplication");
    if (!app_cls) return;
    SEL shared_sel = sel_registerName("sharedApplication");
    id app = ((id (*)(Class, SEL))objc_msgSend)(app_cls, shared_sel);
    if (!app) return;
    SEL windows_sel = sel_registerName("windows");
    id windows = ((id (*)(id, SEL))objc_msgSend)(app, windows_sel);
    if (!windows) return;
    Class space_cls = objc_getClass("NSColorSpace");
    if (!space_cls) return;
    SEL srgb_sel = sel_registerName("sRGBColorSpace");
    id srgb = ((id (*)(Class, SEL))objc_msgSend)(space_cls, srgb_sel);
    if (!srgb) return;
    SEL count_sel = sel_registerName("count");
    SEL at_idx_sel = sel_registerName("objectAtIndex:");
    SEL set_space_sel = sel_registerName("setColorSpace:");
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(windows, count_sel);
    for (unsigned long i = 0; i < n; i++) {
        id w = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, at_idx_sel, i);
        if (w) ((void (*)(id, SEL, id))objc_msgSend)(w, set_space_sel, srgb);
    }
}
#else
static void rae_macos_set_window_srgb(void) { /* no-op on non-Apple */ }
#endif

void rae_ext_initWindow(int64_t width, int64_t height, rae_String title) {
    InitWindow((int)width, (int)height, (const char*)title.data);
    rae_macos_set_window_srgb();
}

void rae_ext_setConfigFlags(int64_t flags) {
    SetConfigFlags((unsigned int)flags);
}

Texture rae_ext_loadStreamTexture(int64_t width, int64_t height) {
    Image img = GenImageColor((int)width, (int)height, BLACK);
    Texture t = LoadTextureFromImage(img);
    UnloadImage(img);
    return t;
}

void rae_ext_updateStreamTexture(Texture texture, const int64_t* pixels, int64_t count) {
    if (!pixels || count <= 0) return;
    /* Reusable scratch for the packed-Int -> RGBA8 expansion. Display runs on
     * the main thread only, so a static buffer is fine and avoids a per-frame
     * malloc of the (large, at Full HD) pixel array. */
    static unsigned char* scratch = NULL;
    static int64_t scratch_count = 0;
    if (count > scratch_count) {
        unsigned char* grown = (unsigned char*)realloc(scratch, (size_t)count * 4);
        if (!grown) return;
        scratch = grown;
        scratch_count = count;
    }
    const int64_t* px = (const int64_t*)pixels;
    for (int64_t i = 0; i < count; i++) {
        int64_t p = px[i];
        scratch[i * 4 + 0] = (unsigned char)((p >> 16) & 0xFF);
        scratch[i * 4 + 1] = (unsigned char)((p >> 8) & 0xFF);
        scratch[i * 4 + 2] = (unsigned char)(p & 0xFF);
        scratch[i * 4 + 3] = 255;
    }
    UpdateTexture(texture, scratch);
}

void rae_ext_drawCubeWires(Vector3 pos, double width, double height, double length, Color color) {
    DrawCubeWires(pos, (float)width, (float)height, (float)length, color);
}

void rae_ext_drawSphere(Vector3 centerPos, double radius, Color color) {
    DrawSphere(centerPos, (float)radius, color);
}

void rae_ext_drawCircle(double x, double y, double radius, Color color) {
    DrawCircle((int)x, (int)y, (float)radius, color);
}

void rae_ext_drawCircleGradient(int64_t x, int64_t y, double radius, Color color1, Color color2) {
    DrawCircleGradient((int)x, (int)y, (float)radius, color1, color2);
}

void rae_ext_drawRectangleGradientV(int64_t x, int64_t y, int64_t width, int64_t height, Color color1, Color color2) {
    DrawRectangleGradientV((int)x, (int)y, (int)width, (int)height, color1, color2);
}

void rae_ext_drawRectangleGradientH(int64_t x, int64_t y, int64_t width, int64_t height, Color color1, Color color2) {
    DrawRectangleGradientH((int)x, (int)y, (int)width, (int)height, color1, color2);
}

double rae_ext_getTime(void) {
    return GetTime();
}

Color rae_ext_colorFromHSV(double hue, double saturation, double value) {
    return ColorFromHSV((float)hue, (float)saturation, (float)value);
}

void rae_ext_takeScreenshot(rae_String fileName) {
    TakeScreenshot((const char*)fileName.data);
}

void rae_ext_drawCylinder(Vector3 position, double radiusTop, double radiusBottom, double height, int64_t slices, Color color) {
    DrawCylinder(position, (float)radiusTop, (float)radiusBottom, (float)height, (int)slices, color);
}

void rae_ext_drawGrid(int64_t slices, double spacing) {
    DrawGrid((int)slices, (float)spacing);
}

void rae_ext_beginMode3D(Camera3D camera) {
    BeginMode3D(camera);
}

void rae_ext_endMode3D(void) {
    EndMode3D();
}

void rae_ext_beginMode2D(Camera2D camera) {
    BeginMode2D(camera);
}

void rae_ext_endMode2D(void) {
    EndMode2D();
}

void rae_ext_drawRectangle(double x, double y, double width, double height, Color color) {
    DrawRectangle((int)x, (int)y, (int)width, (int)height, color);
}

void rae_ext_drawRectangleLines(double x, double y, double width, double height, Color color) {
    DrawRectangleLines((int)x, (int)y, (int)width, (int)height, color);
}

void rae_ext_drawRectangleRounded(double x, double y, double width, double height, double roundness, int64_t segments, Color color) {
    Rectangle rec = {(float)x, (float)y, (float)width, (float)height};
    DrawRectangleRounded(rec, (float)roundness, (int)segments, color);
}

void rae_ext_drawCube(Vector3 pos, double width, double height, double length, Color color) {
    DrawCube(pos, (float)width, (float)height, (float)length, color);
}

void rae_ext_drawText(rae_String text, double x, double y, double fontSize, Color color) {
    DrawText((const char*)text.data, (int)x, (int)y, (int)fontSize, color);
}

rae_Bool rae_ext_windowShouldClose(void) { return WindowShouldClose(); }
void rae_ext_closeWindow(void) { CloseWindow(); }
void rae_ext_setTargetFPS(int64_t fps) { SetTargetFPS((int)fps); }

/* GLFW wait-events bindings. raylib bundles GLFW statically inside
 * libraylib.a; the dynamic libraylib.dylib does NOT export these
 * symbols. ALL build paths must link libraylib.a directly:
 *   - compiler/Makefile (the rae driver binary)
 *   - compiler/src/main.c (the gcc command rae emits when running
 *     a Compiled-target program)
 *   - compiler/tools/run_examples.sh
 *   - examples/98_mobile_ui/snapshot.sh
 *   - rae-devtools-web/src/server/config.ts (the IDE-style runner)
 *
 * If you're adding a new launcher: link `/opt/homebrew/lib/libraylib.a`
 * directly, NOT `-lraylib`. The dynamic library is missing the GLFW
 * symbols we need for the event-driven UI loop.
 *
 * No GLFW header is on the include path (Homebrew's raylib formula
 * does not ship glfw3.h); declare the prototypes locally. All three
 * should be called only after initWindow() has run — GLFW must be
 * initialised by then. */
extern void glfwWaitEventsTimeout(double timeout);
extern void glfwWaitEvents(void);
extern void glfwPostEmptyEvent(void);

/* Window-close callback: by default raylib's own callback sets
 * CORE.Window.shouldClose=TRUE when the user clicks the red X. We
 * override it with this waker so the wait-events main loop wakes up
 * immediately on close. The override needs to do BOTH jobs raylib's
 * default does PLUS post an empty event:
 *   1. glfwSetWindowShouldClose so raylib's WindowShouldClose() picks
 *      it up on the next call.
 *   2. glfwPostEmptyEvent so glfwWaitEventsTimeout returns now instead
 *      of waiting out the rest of its timeout. */
typedef struct GLFWwindow GLFWwindow;
typedef void (*GLFWwindowclosefun)(GLFWwindow*);
typedef void (*GLFWmousebuttonfun)(GLFWwindow*, int, int, int);
extern GLFWwindow* glfwGetCurrentContext(void);
extern void glfwSetWindowCloseCallback(GLFWwindow* w, GLFWwindowclosefun cb);
extern void glfwSetWindowShouldClose(GLFWwindow* w, int value);
extern GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun cb);
#define RAE_GLFW_PRESS 1
#define RAE_GLFW_RELEASE 0
#define RAE_GLFW_MOUSE_BUTTON_LEFT 0

static void rae_glfw_close_waker(GLFWwindow* w) {
  /* macOS quirk: clicking the red-X invokes this delegate-driven
   * callback (we see it fire in stderr), but `glfwPostEmptyEvent`
   * from inside the delegate does NOT reliably wake the
   * `glfwWaitEventsTimeout` blocked in `nextEventMatchingMask` — and
   * the wait's own timeout doesn't fire either while the close path
   * is pending. So the window appeared "stuck open" until the user
   * clicked again somewhere in the app to generate an event the
   * wait would actually return from.
   *
   * Fix: exit the process directly from the callback. The OS
   * reclaims the GL context, GPU textures, and any other resources;
   * we lose the explicit `closeWindow` / texture-unload calls but
   * those are best-effort hygiene anyway. The diagnostic print is
   * kept so future investigations into a different macOS path don't
   * have to be re-instrumented. */
  fprintf(stderr, "[close-waker] fired (w=%p) — exiting\n", (void*)w);
  fflush(stderr);
  glfwSetWindowShouldClose(w, 1);
  // `_exit`, not `exit`: skip atexit handlers. raylib / GLFW
  // cleanup (CloseWindow, glfwTerminate) can hang waiting on the
  // GL context / GPU sync, and any user-registered atexit hook is
  // run on a thread that may already hold locks acquired in the
  // delegate-driven close path. The OS reclaims the GL context,
  // textures, fds, and process memory regardless. Without this,
  // the app process can stay alive after the window is gone,
  // which in turn keeps the rae watch supervisor waiting and
  // makes the devtools Stop button look broken.
  _exit(0);
}

/* macOS click-drop workaround. raylib's IsMouseButtonPressed/Released
 * are computed by comparing the previous poll's button state to the
 * current poll's — if a press+release pair arrives between two polls
 * (easy on macOS, where mouse-button events don't reliably wake
 * glfwWaitEventsTimeout), the post-poll state is up→up with no edge
 * and BOTH flags return false. The click vanishes.
 *
 * Fix: chain our own GLFW mouse-button callback below raylib's so we
 * see every transition per-event rather than per-poll. Counters are
 * single-threaded (GLFW callbacks fire on the main thread, same as
 * the rest of the loop), so plain int suffices — no atomics. */
static int g_mouse_press_pending = 0;
static int g_mouse_release_pending = 0;
static GLFWmousebuttonfun g_prev_mouse_button_cb = NULL;
static int g_mouse_button_hook_installed = 0;

static void rae_glfw_mouse_button_chain_cb(GLFWwindow* w, int button, int action, int mods) {
  if (g_prev_mouse_button_cb) g_prev_mouse_button_cb(w, button, action, mods);
  if (button == RAE_GLFW_MOUSE_BUTTON_LEFT) {
    if (action == RAE_GLFW_PRESS) g_mouse_press_pending = 1;
    else if (action == RAE_GLFW_RELEASE) g_mouse_release_pending = 1;
  }
}

void rae_ext_installMouseButtonHook(void) {
  if (g_mouse_button_hook_installed) return;
  GLFWwindow* w = glfwGetCurrentContext();
  if (!w) {
    fprintf(stderr, "[mouse-hook] FAILED: no current GLFW context\n");
    fflush(stderr);
    return;
  }
  g_prev_mouse_button_cb = glfwSetMouseButtonCallback(w, rae_glfw_mouse_button_chain_cb);
  g_mouse_button_hook_installed = 1;
  fprintf(stderr, "[mouse-hook] installed (prev cb=%p)\n", (void*)g_prev_mouse_button_cb);
  fflush(stderr);
}

rae_Bool rae_ext_mouseHookDrainPressed(void) {
  int v = g_mouse_press_pending;
  g_mouse_press_pending = 0;
  return v != 0;
}

rae_Bool rae_ext_mouseHookDrainReleased(void) {
  int v = g_mouse_release_pending;
  g_mouse_release_pending = 0;
  return v != 0;
}

/* True once installMouseButtonHook has successfully chained the GLFW
 * callback. Lets the input layer use the hook as the sole authoritative
 * edge source and skip raylib's poll-to-poll edges entirely, while
 * still falling back to polling for hosts that never install it. */
rae_Bool rae_ext_mouseHookActive(void) {
  return g_mouse_button_hook_installed != 0;
}

void rae_ext_waitEventsTimeout(double seconds) { glfwWaitEventsTimeout(seconds); }
void rae_ext_waitEvents(void) { glfwWaitEvents(); }
void rae_ext_postEmptyEvent(void) { glfwPostEmptyEvent(); }
void rae_ext_installWindowCloseWaker(void) {
  GLFWwindow* w = glfwGetCurrentContext();
  if (w) {
    glfwSetWindowCloseCallback(w, rae_glfw_close_waker);
    fprintf(stderr, "[close-waker] installed for window %p\n", (void*)w);
  } else {
    fprintf(stderr, "[close-waker] FAILED: no current GLFW context\n");
  }
  fflush(stderr);
}

void rae_ext_beginDrawing(void) { BeginDrawing(); }
void rae_ext_endDrawing(void) { EndDrawing(); }
void rae_ext_clearBackground(Color color) { ClearBackground(color); }
rae_Bool rae_ext_isKeyDown(int64_t key) { return IsKeyDown((int)key); }
rae_Bool rae_ext_isKeyPressed(int64_t key) { return IsKeyPressed((int)key); }
int64_t rae_ext_getMouseX(void) { return (int64_t)GetMouseX(); }
int64_t rae_ext_getMouseY(void) { return (int64_t)GetMouseY(); }
double rae_ext_getMouseWheelMove(void) { return (double)GetMouseWheelMove(); }
rae_Bool rae_ext_isMouseButtonDown(int64_t button) { return IsMouseButtonDown((int)button); }
rae_Bool rae_ext_isMouseButtonPressed(int64_t button) { return IsMouseButtonPressed((int)button); }
rae_Bool rae_ext_isMouseButtonReleased(int64_t button) { return IsMouseButtonReleased((int)button); }
void rae_ext_setMouseScale(double scaleX, double scaleY) { SetMouseScale((float)scaleX, (float)scaleY); }
int64_t rae_ext_getScreenWidth(void) { return (int64_t)GetScreenWidth(); }
int64_t rae_ext_getScreenHeight(void) { return (int64_t)GetScreenHeight(); }
rae_Bool rae_ext_isWindowResized(void) { return IsWindowResized(); }
int64_t rae_ext_getRenderWidth(void) { return (int64_t)GetRenderWidth(); }
int64_t rae_ext_getRenderHeight(void) { return (int64_t)GetRenderHeight(); }
int64_t rae_ext_getCurrentMonitor(void) { return (int64_t)GetCurrentMonitor(); }
int64_t rae_ext_getMonitorWidth(int64_t monitor) { return (int64_t)GetMonitorWidth((int)monitor); }
int64_t rae_ext_getMonitorHeight(int64_t monitor) { return (int64_t)GetMonitorHeight((int)monitor); }
int64_t rae_ext_getMonitorRefreshRate(int64_t monitor) { return (int64_t)GetMonitorRefreshRate((int)monitor); }
void rae_ext_setWindowSize(int64_t width, int64_t height) { SetWindowSize((int)width, (int)height); }
void rae_ext_setWindowPosition(int64_t x, int64_t y) { SetWindowPosition((int)x, (int)y); }
int64_t rae_ext_getWindowPositionX(void) { Vector2 p = GetWindowPosition(); return (int64_t)p.x; }
int64_t rae_ext_getWindowPositionY(void) { Vector2 p = GetWindowPosition(); return (int64_t)p.y; }
Texture rae_ext_loadTexture(rae_String fileName) { return LoadTexture((const char*)fileName.data); }

/* Set the GPU sampling filter for a texture id. `filter` matches
 * raylib's TextureFilter enum (0=POINT, 1=BILINEAR, 2=TRILINEAR, ...).
 * MSDF/MTSDF atlases REQUIRE bilinear (1): the distance-field decode
 * relies on the GPU interpolating distance values between texels;
 * point sampling produces jagged, stair-stepped glyph edges. The
 * filter is GL state keyed by texture id, so passing the Texture by
 * value still affects the live texture. */
void rae_ext_setTextureFilter(Texture texture, int64_t filter) {
  SetTextureFilter(texture, (int)filter);
}

Texture rae_ext_loadCircleCroppedTexture(rae_String fileName) {
  /* Load `fileName`, force RGBA, and zero the alpha channel of every
   * pixel outside an inscribed circle. Used by the mobile UI for
   * "profile picture"-style round avatars without needing a
   * fragment-shader pipeline. The smaller of width/height bounds
   * the circle so rectangular sources still produce a centered
   * circular crop. */
  Image img = LoadImage((const char*)fileName.data);
  if (img.data == NULL) {
    return (Texture){0};
  }
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  float cx = (float)img.width / 2.0f;
  float cy = (float)img.height / 2.0f;
  float r = (img.width < img.height ? (float)img.width : (float)img.height) / 2.0f;
  float r2 = r * r;
  unsigned char* data = (unsigned char*)img.data;
  for (int y = 0; y < img.height; y++) {
    for (int x = 0; x < img.width; x++) {
      float dx = (float)x + 0.5f - cx;
      float dy = (float)y + 0.5f - cy;
      float d2 = dx * dx + dy * dy;
      if (d2 > r2) {
        data[(y * img.width + x) * 4 + 3] = 0;
      }
    }
  }
  Texture tex = LoadTextureFromImage(img);
  UnloadImage(img);
  return tex;
}
Texture rae_ext_loadRoundedCroppedTexture(rae_String fileName, double radius) {
  /* Load `fileName`, force RGBA, downscale to a thumbnail-friendly
   * size, then zero the alpha channel of every pixel outside the
   * rounded-rectangle of the given corner radius. Pre-baking the
   * alpha mask lets the renderer use plain `DrawTexture` with no
   * clipping logic.
   *
   * Downscale matters: `radius` is intended as DISPLAY pixels (a
   * scene file saying `"radius": 8` means "8 px at display size").
   * Source covers are typically ~600x600 — baking only 8 pixels of
   * curve on that source gives ~0.7 display pixels when the thumb
   * renders at 56x56, indistinguishable from a sharp rectangle.
   * Resizing to a 128-side image first makes the same 8-pixel curve
   * map to ~3.5 display pixels, which reads as proper rounding. */
  Image img = LoadImage((const char*)fileName.data);
  if (img.data == NULL) {
    return (Texture){0};
  }
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  /* Downscale longest side to `thumbMax` while preserving aspect.
   * 64 is small enough that even a small `radius` reads as proper
   * rounding when the thumb renders at typical UI sizes (40-80 px),
   * but big enough that the cover detail stays recognisable. */
  const int thumbMax = 64;
  int origMaxSide = (img.width > img.height) ? img.width : img.height;
  if (origMaxSide > thumbMax) {
    float k = (float)thumbMax / (float)origMaxSide;
    int newW = (int)((float)img.width * k);
    int newH = (int)((float)img.height * k);
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;
    ImageResize(&img, newW, newH);
  }
  float r = (float)radius;
  if (r < 0.0f) r = 0.0f;
  float maxR = (img.width < img.height ? (float)img.width : (float)img.height) / 2.0f;
  if (r > maxR) r = maxR;
  float r2 = r * r;
  float w1 = (float)img.width - 1.0f;
  float h1 = (float)img.height - 1.0f;
  unsigned char* data = (unsigned char*)img.data;
  for (int y = 0; y < img.height; y++) {
    for (int x = 0; x < img.width; x++) {
      /* Distance from this pixel to the nearest corner-center; if
       * inside the radius, alpha stays; if outside AND we're inside
       * the corner box, alpha is zeroed. */
      float fx = (float)x;
      float fy = (float)y;
      int inCorner = 0;
      float cx = 0.0f, cy = 0.0f;
      if (fx < r && fy < r) { inCorner = 1; cx = r; cy = r; }
      else if (fx > w1 - r && fy < r) { inCorner = 1; cx = w1 - r; cy = r; }
      else if (fx < r && fy > h1 - r) { inCorner = 1; cx = r; cy = h1 - r; }
      else if (fx > w1 - r && fy > h1 - r) { inCorner = 1; cx = w1 - r; cy = h1 - r; }
      if (inCorner) {
        float dx = fx - cx;
        float dy = fy - cy;
        if (dx * dx + dy * dy > r2) {
          data[(y * img.width + x) * 4 + 3] = 0;
        }
      }
    }
  }
  Texture tex = LoadTextureFromImage(img);
  UnloadImage(img);
  return tex;
}
void rae_ext_unloadTexture(Texture texture) { UnloadTexture(texture); }

/* Silence raylib's INFO/DEBUG logs (TEXTURE / SHADER / GL / ...).
 * Per-frame texture churn — e.g. the dock's frosted-glass blur
 * updating its cached texture every frame — otherwise floods the
 * terminal with "TEXTURE: [ID N] Texture loaded successfully" lines.
 * Pass `LOG_WARNING` (4) at boot to keep only warnings and errors. */
void rae_ext_raylibSetLogLevel(int64_t level) {
  SetTraceLogLevel((int)level);
}

/* Rounded textured rect via a fragment shader. One global Shader,
 * lazy-loaded on first use, used to mask any sprite with a non-zero
 * corner radius at draw time. Pass the on-screen sprite size + the
 * radius (both in pixels) before each draw; the shader computes a
 * signed-distance value to a rounded-rect boundary and antialiases
 * the alpha at the curve. */
static Shader g_rae_rounded_shader = {0};
static int g_rae_rounded_loc_size = -1;
static int g_rae_rounded_loc_radius = -1;
static int g_rae_rounded_loaded = 0;

static void rae_rounded_shader_ensure(void) {
  if (g_rae_rounded_loaded) return;
  const char* fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 uSize;\n"
    "uniform float uRadius;\n"
    "float sdRoundRect(vec2 p, vec2 b, float r) {\n"
    "  vec2 q = abs(p) - b + vec2(r);\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
    "}\n"
    "void main() {\n"
    "  vec4 texel = texture(texture0, fragTexCoord);\n"
    "  vec4 c = texel * colDiffuse * fragColor;\n"
    "  vec2 p = (fragTexCoord - vec2(0.5)) * uSize;\n"
    "  float d = sdRoundRect(p, uSize * 0.5, uRadius);\n"
    "  float aa = clamp(0.5 - d, 0.0, 1.0);\n"
    "  c.a *= aa;\n"
    "  finalColor = c;\n"
    "}\n";
  g_rae_rounded_shader = LoadShaderFromMemory(NULL, fs);
  g_rae_rounded_loc_size = GetShaderLocation(g_rae_rounded_shader, "uSize");
  g_rae_rounded_loc_radius = GetShaderLocation(g_rae_rounded_shader, "uRadius");
  g_rae_rounded_loaded = 1;
}

void rae_ext_roundedSpriteBegin(double width, double height, double radius) {
  rae_rounded_shader_ensure();
  float size[2] = { (float)width, (float)height };
  float r = (float)radius;
  SetShaderValue(g_rae_rounded_shader, g_rae_rounded_loc_size, size, SHADER_UNIFORM_VEC2);
  SetShaderValue(g_rae_rounded_shader, g_rae_rounded_loc_radius, &r, SHADER_UNIFORM_FLOAT);
  BeginShaderMode(g_rae_rounded_shader);
}

void rae_ext_roundedSpriteEnd(void) {
  EndShaderMode();
}

/* ---- MTSDF text shader -----------------------------------------------
 *
 * Multi-channel + true-distance signed-distance-field text. The atlas
 * is generated offline by Chlumsky's `msdf-atlas-gen` (-type mtsdf).
 * RGB carries the MSDF median-trick channels (clean reconstruction of
 * sharp corners) and A carries the straight Euclidean distance — used
 * for the outline + shadow bands so they don't develop the false
 * intersections that pure RGB median has inside concave glyph features.
 *
 * `uPxRange` is the atlas's authored distanceRange (4 by default in
 * gen-msdf.sh) scaled to on-screen pixels — caller passes
 * `pxRange * onScreenSize / atlasFontSize`. It defines the anti-alias
 * band width: 1 means hard pixels, larger = softer edges.
 *
 * Outline: bands the silhouette by an additional `uOutlineWidth` screen
 * pixels around the body, painted in `uOutlineColor`.
 *
 * Shadow: re-samples the atlas at fragTexCoord - uShadowOffset (so a
 * positive uShadowOffset is "shadow falls down-right"). `uShadowSoftness`
 * widens the falloff in pixels — 1 = hard, 4-8 = nicely blurred. Shadow
 * paints UNDER the outline + body.
 *
 * The shader composites in this order: shadow → outline → body. Each
 * layer multiplies its coverage against the per-vertex `fragColor` (so
 * the entity's Active/Opacity chain still fades the whole glyph). */
static Shader g_rae_mtsdf_shader = {0};
static int g_rae_mtsdf_loaded = 0;
static int g_rae_mtsdf_loc_pxrange = -1;
static int g_rae_mtsdf_loc_textColor = -1;
static int g_rae_mtsdf_loc_outlineColor = -1;
static int g_rae_mtsdf_loc_outlineWidth = -1;
static int g_rae_mtsdf_loc_shadowColor = -1;
static int g_rae_mtsdf_loc_shadowOffset = -1;
static int g_rae_mtsdf_loc_shadowSoftness = -1;

static void rae_mtsdf_shader_ensure(void) {
  if (g_rae_mtsdf_loaded) return;
  const char* fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform float uPxRange;\n"
    "uniform vec4  uTextColor;\n"
    "uniform vec4  uOutlineColor;\n"
    "uniform float uOutlineWidth;\n"
    "uniform vec4  uShadowColor;\n"
    "uniform vec2  uShadowOffset;\n"
    "uniform float uShadowSoftness;\n"
    "float median3(float r, float g, float b) {\n"
    "  return max(min(r,g), min(max(r,g), b));\n"
    "}\n"
    "float msdfDistPx(vec4 sample4) {\n"
    "  return uPxRange * (median3(sample4.r, sample4.g, sample4.b) - 0.5);\n"
    "}\n"
    /* Composite one straight-alpha layer (color `srgb`, coverage-scaled
     * alpha `sa`) over a PREMULTIPLIED accumulator. Working in
     * premultiplied space is what kills the edge halo: the previous
     * shader multiplied glyph RGB by coverage and then the alpha-blend
     * stage multiplied by coverage again (coverage^2), darkening
     * anti-aliased edges — invisible on dark UI, an obvious grey
     * "outline" on light backgrounds. */
    "vec4 overPremul(vec4 dst, vec3 srgb, float sa) {\n"
    "  vec3 sp = srgb * sa;\n"
    "  return vec4(sp + dst.rgb * (1.0 - sa), sa + dst.a * (1.0 - sa));\n"
    "}\n"
    "void main() {\n"
    "  vec4 atlasSample = texture(texture0, fragTexCoord);\n"
    "  float bodyDist = msdfDistPx(atlasSample);\n"
    "  float bodyCov  = clamp(bodyDist + 0.5, 0.0, 1.0);\n"
    "  float outlineCov = 0.0;\n"
    "  if (uOutlineWidth > 0.0 && uOutlineColor.a > 0.0) {\n"
    "    float outlineTotal = clamp((bodyDist + uOutlineWidth) + 0.5, 0.0, 1.0);\n"
    "    outlineCov = clamp(outlineTotal - bodyCov, 0.0, 1.0);\n"
    "  }\n"
    "  float shadowCov = 0.0;\n"
    "  if (uShadowColor.a > 0.0) {\n"
    "    vec4 sSample = texture(texture0, fragTexCoord - uShadowOffset);\n"
    "    float sDist = msdfDistPx(sSample);\n"
    "    float soft = max(uShadowSoftness, 1.0);\n"
    "    shadowCov = clamp(sDist / soft + 0.5, 0.0, 1.0);\n"
    "  }\n"
    /* Bottom-to-top: shadow, then outline ring, then body. */
    "  vec4 acc = vec4(0.0);\n"
    "  acc = overPremul(acc, uShadowColor.rgb,  uShadowColor.a  * shadowCov);\n"
    "  acc = overPremul(acc, uOutlineColor.rgb, uOutlineColor.a * outlineCov);\n"
    "  acc = overPremul(acc, uTextColor.rgb,    uTextColor.a    * bodyCov);\n"
    /* Entity tint/fade (fragColor * colDiffuse) scales the whole
     * premultiplied result so alpha and colour stay consistent. */
    "  vec4 tint = fragColor * colDiffuse;\n"
    "  acc.rgb *= tint.rgb;\n"
    "  acc *= tint.a;\n"
    "  finalColor = acc;\n"
    "}\n";
  g_rae_mtsdf_shader = LoadShaderFromMemory(NULL, fs);
  g_rae_mtsdf_loc_pxrange        = GetShaderLocation(g_rae_mtsdf_shader, "uPxRange");
  g_rae_mtsdf_loc_textColor      = GetShaderLocation(g_rae_mtsdf_shader, "uTextColor");
  g_rae_mtsdf_loc_outlineColor   = GetShaderLocation(g_rae_mtsdf_shader, "uOutlineColor");
  g_rae_mtsdf_loc_outlineWidth   = GetShaderLocation(g_rae_mtsdf_shader, "uOutlineWidth");
  g_rae_mtsdf_loc_shadowColor    = GetShaderLocation(g_rae_mtsdf_shader, "uShadowColor");
  g_rae_mtsdf_loc_shadowOffset   = GetShaderLocation(g_rae_mtsdf_shader, "uShadowOffset");
  g_rae_mtsdf_loc_shadowSoftness = GetShaderLocation(g_rae_mtsdf_shader, "uShadowSoftness");
  g_rae_mtsdf_loaded = 1;
}

/* Caller passes: pxRange (in screen px — i.e. atlas pxrange * screen-
 * size / atlas-font-size), the text/outline/shadow colors as 0-255
 * RGBA, the outline width in screen px, shadow offset in texCoord
 * units (atlas-uv space, NOT screen-px — Rae side knows the atlas
 * dimensions and converts), and shadow softness in screen px. */
void rae_ext_msdfBegin(double pxRange,
                       Color textColor, Color outlineColor, double outlineWidth,
                       Color shadowColor, double shadowOffX, double shadowOffY,
                       double shadowSoftness) {
  rae_mtsdf_shader_ensure();
  float px = (float)pxRange;
  float tc[4] = { textColor.r/255.0f, textColor.g/255.0f, textColor.b/255.0f, textColor.a/255.0f };
  float oc[4] = { outlineColor.r/255.0f, outlineColor.g/255.0f, outlineColor.b/255.0f, outlineColor.a/255.0f };
  float ow = (float)outlineWidth;
  float sc[4] = { shadowColor.r/255.0f, shadowColor.g/255.0f, shadowColor.b/255.0f, shadowColor.a/255.0f };
  float so[2] = { (float)shadowOffX, (float)shadowOffY };
  float ss = (float)shadowSoftness;
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_pxrange,        &px, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_textColor,      tc,  SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_outlineColor,   oc,  SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_outlineWidth,   &ow, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_shadowColor,    sc,  SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_shadowOffset,   so,  SHADER_UNIFORM_VEC2);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_shadowSoftness, &ss, SHADER_UNIFORM_FLOAT);
  /* The fragment shader emits PREMULTIPLIED-alpha colour, so it must be
   * composited with a premultiplied blend (GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
   * Using the default straight-alpha blend here would re-introduce the
   * coverage^2 edge darkening this shader exists to avoid. */
  BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
  BeginShaderMode(g_rae_mtsdf_shader);
}

void rae_ext_msdfEnd(void) {
  EndShaderMode();
  EndBlendMode();
}

void rae_ext_drawTexturePro(Texture texture, Rectangle source, Rectangle dest, Vector2 origin, double rotation, Color tint) {
  DrawTexturePro(texture, source, dest, origin, (float)rotation, tint);
}

/* Gradient rounded-rect: shader fills the alpha-masked rounded box
 * with a linear gradient from `from` to `to` along the given angle
 * (degrees — 0=L→R, 90=T→B). Pair with `CornerRadius` in scenes
 * via the `GradientFill` ECS component. Single atomic call;
 * lazy-loads its shader on first use. */
static Shader g_rae_gradient_shader = {0};
static int g_rae_grad_loc_size = -1;
static int g_rae_grad_loc_radius = -1;
static int g_rae_grad_loc_from = -1;
static int g_rae_grad_loc_to = -1;
static int g_rae_grad_loc_angle = -1;
static int g_rae_gradient_loaded = 0;

static void rae_gradient_shader_ensure(void) {
  if (g_rae_gradient_loaded) return;
  const char* fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform vec2 uSize;\n"
    "uniform float uRadius;\n"
    "uniform vec4 uFrom;\n"
    "uniform vec4 uTo;\n"
    "uniform float uAngleRad;\n"
    "float sdRoundRect(vec2 p, vec2 b, float r) {\n"
    "  vec2 q = abs(p) - b + vec2(r);\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
    "}\n"
    "void main() {\n"
    "  vec2 dir = vec2(cos(uAngleRad), sin(uAngleRad));\n"
    "  vec2 p = fragTexCoord - vec2(0.5);\n"
    "  float t = clamp(dot(p, dir) + 0.5, 0.0, 1.0);\n"
    "  vec4 c = mix(uFrom, uTo, t);\n"
    "  vec2 pp = (fragTexCoord - vec2(0.5)) * uSize;\n"
    "  float d = sdRoundRect(pp, uSize * 0.5, uRadius);\n"
    "  float aa = clamp(0.5 - d, 0.0, 1.0);\n"
    "  c.a *= aa * fragColor.a;\n"
    "  finalColor = c;\n"
    "}\n";
  g_rae_gradient_shader = LoadShaderFromMemory(NULL, fs);
  g_rae_grad_loc_size = GetShaderLocation(g_rae_gradient_shader, "uSize");
  g_rae_grad_loc_radius = GetShaderLocation(g_rae_gradient_shader, "uRadius");
  g_rae_grad_loc_from = GetShaderLocation(g_rae_gradient_shader, "uFrom");
  g_rae_grad_loc_to = GetShaderLocation(g_rae_gradient_shader, "uTo");
  g_rae_grad_loc_angle = GetShaderLocation(g_rae_gradient_shader, "uAngleRad");
  g_rae_gradient_loaded = 1;
}

void rae_ext_drawGradientRect(
    double x, double y, double w, double h, double radius,
    int64_t r1, int64_t g1, int64_t b1, int64_t a1,
    int64_t r2, int64_t g2, int64_t b2, int64_t a2,
    double angleDeg
) {
  rae_gradient_shader_ensure();
  float size[2] = { (float)w, (float)h };
  float fr = (float)radius;
  float from[4] = { (float)r1 / 255.0f, (float)g1 / 255.0f, (float)b1 / 255.0f, (float)a1 / 255.0f };
  float to[4]   = { (float)r2 / 255.0f, (float)g2 / 255.0f, (float)b2 / 255.0f, (float)a2 / 255.0f };
  float angleRad = (float)(angleDeg * 3.14159265358979 / 180.0);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_size, size, SHADER_UNIFORM_VEC2);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_radius, &fr, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_from, from, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_to, to, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_angle, &angleRad, SHADER_UNIFORM_FLOAT);
  BeginShaderMode(g_rae_gradient_shader);
  /* Emit a textured quad with explicit 0..1 UVs. `DrawRectangleRec`
   * samples raylib's tiny `shapes` white-pixel texture so fragTexCoord
   * is effectively constant — the shader's gradient + SDF corner mask
   * both need fragTexCoord to span 0..1 across the rect. Hand-rolling
   * the quad guarantees it. */
  Texture2D white = (Texture2D){ rlGetTextureIdDefault(), 1, 1, 1, 7 };
  rlSetTexture(white.id);
  rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    rlNormal3f(0.0f, 0.0f, 1.0f);
    rlTexCoord2f(0.0f, 0.0f); rlVertex2f((float)x,            (float)y);
    rlTexCoord2f(0.0f, 1.0f); rlVertex2f((float)x,            (float)(y + h));
    rlTexCoord2f(1.0f, 1.0f); rlVertex2f((float)(x + w),      (float)(y + h));
    rlTexCoord2f(1.0f, 0.0f); rlVertex2f((float)(x + w),      (float)y);
  rlEnd();
  rlSetTexture(0);
  EndShaderMode();
}
Texture rae_ext_captureAndBlurRegion(double x, double y, double w, double h, int64_t blurSize) {
  /* Frosted-glass for a sub-region of the screen — used by the
   * mobile UI's bottom dock so only the bar area is blurred (the
   * "vibrancy" effect), not the whole window like the modal-blur
   * helper below. `x/y/w/h` are in *logical* coordinates; we scale
   * by GetWindowScaleDPI() to crop the right physical-pixel rect
   * from `LoadImageFromScreen`. */
  rlDrawRenderBatchActive();
  glFinish();
  Image full = LoadImageFromScreen();
  Vector2 scale = GetWindowScaleDPI();
  Rectangle crop = {
    (float)x * scale.x,
    (float)y * scale.y,
    (float)w * scale.x,
    (float)h * scale.y
  };
  Image region = ImageFromImage(full, crop);
  UnloadImage(full);
  ImageBlurGaussian(&region, (int)blurSize);
  Texture tex = LoadTextureFromImage(region);
  UnloadImage(region);
  return tex;
}

Texture rae_ext_captureAndBlurBackdrop(int64_t blurSize) {
  /* Frosted-glass backdrop helper for modal UI: grab the back buffer,
   * run a Gaussian blur over it on the CPU, upload as a Texture, and
   * release the temporary Image. The blur radius `blurSize` is in
   * pixels — ~10 reads as soft "vibrancy" on a typical mobile-sized
   * window. Cost is dominated by ImageBlurGaussian which is O(width *
   * height * blurSize). Designed for one-shot calls on modal open.
   *
   * IMPORTANT: raylib batches draw calls in CPU buffers and only
   * flushes them to the GPU at end-of-frame (or when the batch fills
   * up). Calling `LoadImageFromScreen` while draws are still pending
   * returns stale framebuffer contents — usually just the most
   * recent ClearBackground color, which is why the captured image
   * looks like a uniform dark block. Fix: force a flush + GPU sync
   * before the read. Same workaround `examples/98_mobile_ui/snapshot.sh`
   * documents for the related `TakeScreenshot` path on macOS+Metal. */
  rlDrawRenderBatchActive();
  glFinish();
  Image img = LoadImageFromScreen();
  ImageBlurGaussian(&img, (int)blurSize);
  Texture tex = LoadTextureFromImage(img);
  UnloadImage(img);
  return tex;
}
void rae_ext_drawTexture(Texture texture, double x, double y, Color tint) { DrawTexture(texture, (int)x, (int)y, tint); }
void rae_ext_drawTextureEx(Texture texture, Vector2 pos, double rotation, double scale, Color tint) { DrawTextureEx(texture, pos, (float)rotation, (float)scale, tint); }
int64_t rae_ext_measureText(rae_String text, int64_t fontSize) { return (int64_t)MeasureText((const char*)text.data, (int)fontSize); }

/* Custom font support.
 *
 * Raylib's `Font` struct holds internal arrays/pointers, which makes passing
 * it across the Rae ↔ C boundary awkward (especially for the live VM, where
 * structs go through RaeAny). We sidestep that with a fixed-size array of
 * "font slots" addressed by `Int`. A slot starts unloaded; `loadFontInto`
 * fills it via raylib's `LoadFontEx` with a codepoint table that covers
 * basic ASCII plus the few Unicode glyphs the HUD uses (arrows, middle dot,
 * em dash). `drawTextWithFont` falls back to the default font if the slot
 * isn't loaded yet, so the program never silently shows blank text.
 */
#define RAE_FONT_SLOTS 8
static Font g_rae_fonts[RAE_FONT_SLOTS];
static int g_rae_font_loaded[RAE_FONT_SLOTS];

static const int g_rae_font_codepoints[] = {
    /* ASCII printable */
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
    80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,
    /* Latin-1 Supplement (U+00A0..U+00FF). Covers ä ö ü ß é è ê à á â
     * ç ñ í ó ú etc. — everything in the names "Björk", "Röyksopp",
     * and the common Western European diacritics. Body fonts (Roboto)
     * have these glyphs; icon fonts don't, and bake notdef boxes that
     * never get drawn because callers don't write 0xA0-0xFF to the
     * icon slot. */
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6,         /* 0x00B7 below */
            0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
    /* HUD glyphs */
    0x00B7, /* · middle dot */
    0x2013, /* – en dash */
    0x2014, /* — em dash */
    0x2192, /* → right arrow */
    0x2190, /* ← left arrow */
    0x2191, /* ↑ up arrow */
    0x2193, /* ↓ down arrow */
    0x2026, /* … horizontal ellipsis */
    /* Material Icons Outlined codepoints used by `lib/ui/icon_codepoints.rae`.
     * Body fonts (e.g. Roboto) won't have glyphs for these — they bake as
     * `notdef` boxes, but no caller writes these codepoints to a body font
     * slot. The icon-font slot picks them up. */
    0xe145, /* add */
    0xe5c4, /* arrow_back */
    0xe5cb, /* chevron_left */
    0xe5cc, /* chevron_right */
    0xe5cd, /* close */
    0xf090, /* download */
    0xe01d, /* equalizer */
    0xe5ce, /* expand_less */
    0xe5cf, /* expand_more */
    0xe87d, /* favorite */
    0xe87e, /* favorite_border */
    0xe88a, /* home */
    0xe02e, /* library_add */
    0xe02f, /* library_books */
    0xe030, /* library_music */
    0xe5d2, /* menu */
    0xeae1, /* more_horiz */
    0xe034, /* pause */
    0xe037, /* play_arrow */
    0xe03b, /* playlist_add */
    0xe065, /* playlist_add_check */
    0xe05f, /* playlist_play */
    0xe03d, /* queue_music */
    0xe040, /* repeat */
    0xe8b6, /* search */
    0xe043, /* shuffle */
    0xe044, /* skip_next */
    0xe045, /* skip_previous */
    0xe047, /* stop */
    0xe80e, /* whatshot */
    0xe7fd, /* person */
    0xe429, /* tune */
    0xe9ba  /* logout */
};
#define RAE_FONT_CODEPOINT_COUNT ((int)(sizeof(g_rae_font_codepoints)/sizeof(g_rae_font_codepoints[0])))

void rae_ext_loadFontInto(int64_t slot, rae_String path, int64_t fontSize) {
    if (slot < 0 || slot >= RAE_FONT_SLOTS) return;
    if (g_rae_font_loaded[slot]) {
        UnloadFont(g_rae_fonts[slot]);
        g_rae_font_loaded[slot] = 0;
    }
    /* Quality strategy: bake the glyph atlas at a high resolution and let
     * the GPU bilinear-filter at draw time. Raylib's default is point
     * sampling, which is fine for retro pixel fonts but turns smooth
     * vector fonts (Roboto etc.) into a blocky mess at non-native sizes.
     * Use max(64, fontSize * 2) so a typical 18–28 px UI request gets a
     * 64–96 px atlas — sharp at the requested size, smooth when scaled. */
    int atlasSize = (int)fontSize * 2;
    if (atlasSize < 64) atlasSize = 64;
    g_rae_fonts[slot] = LoadFontEx(
        (const char*)path.data,
        atlasSize,
        (int*)g_rae_font_codepoints,
        RAE_FONT_CODEPOINT_COUNT
    );
    if (g_rae_fonts[slot].texture.id != 0) {
        SetTextureFilter(g_rae_fonts[slot].texture, TEXTURE_FILTER_BILINEAR);
        g_rae_font_loaded[slot] = 1;
    }
}

void rae_ext_unloadFontSlot(int64_t slot) {
    if (slot < 0 || slot >= RAE_FONT_SLOTS) return;
    if (g_rae_font_loaded[slot]) {
        UnloadFont(g_rae_fonts[slot]);
        g_rae_font_loaded[slot] = 0;
    }
}

rae_Bool rae_ext_isFontSlotLoaded(int64_t slot) {
    if (slot < 0 || slot >= RAE_FONT_SLOTS) return 0;
    return g_rae_font_loaded[slot] ? 1 : 0;
}

void rae_ext_drawTextWithFont(int64_t slot, rae_String text, double x, double y, double fontSize, double spacing, Color color) {
    if (slot >= 0 && slot < RAE_FONT_SLOTS && g_rae_font_loaded[slot]) {
        DrawTextEx(
            g_rae_fonts[slot],
            (const char*)text.data,
            (Vector2){(float)x, (float)y},
            (float)fontSize,
            (float)spacing,
            color
        );
    } else {
        /* Fallback: default font — keeps text on screen if the TTF is missing. */
        DrawText((const char*)text.data, (int)x, (int)y, (int)fontSize, color);
    }
}

/* Slot-aware width measurement. Companion to `drawTextWithFont` — needed
 * to center icon glyphs correctly, since the Material Icons font has
 * private-use codepoints (e.g. 0xE037 = play_arrow) that aren't in the
 * default font. Plain `MeasureText` would return the default-font's
 * notdef width instead of the actual rendered glyph width, putting the
 * icon visibly off-center inside its container.
 * Returns the rendered width in pixels at `fontSize` with `spacing`
 * between glyphs. Falls back to the default font's measurement when
 * the slot isn't loaded, matching `drawTextWithFont`'s fallback. */
int64_t rae_ext_measureTextWithFont(int64_t slot, rae_String text, double fontSize, double spacing) {
    if (slot >= 0 && slot < RAE_FONT_SLOTS && g_rae_font_loaded[slot]) {
        Vector2 sz = MeasureTextEx(
            g_rae_fonts[slot],
            (const char*)text.data,
            (float)fontSize,
            (float)spacing
        );
        return (int64_t)sz.x;
    }
    return (int64_t)MeasureText((const char*)text.data, (int)fontSize);
}
#endif
