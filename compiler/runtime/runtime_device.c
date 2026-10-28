/* The device and display facts lib/Device.rae and lib/Display.rae read
 * (docs/platform-conditional-code.md §3.1 layer 4). One call each and no
 * policy: which pointer is primary, whether hover exists and what an absent
 * answer means are Rae. Platform reason: SDL3's input-device and display
 * calls natively, the browser's media queries and devicePixelRatio in the
 * web build.
 *
 * This module is included by rae_runtime.c into one translation unit, inside
 * RAE_HAS_SDL3 (it reads the SDL window, g_sdl_win). */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* Whether a CSS media query matches now ("(any-pointer: coarse)", ...) */
EM_JS(int, rae_device_media_matches, (const char* query), {
    return (typeof matchMedia === "function" && matchMedia(UTF8ToString(query)).matches) ? 1 : 0;
});

/* window.devicePixelRatio: device pixels per CSS pixel (0 without a window) */
EM_JS(double, rae_device_pixel_ratio, (), {
    return (typeof window !== "undefined" && window.devicePixelRatio) ? window.devicePixelRatio : 0;
});

rae_Bool rae_ext_Device_mediaMatches(rae_String query) {
    char text[64];
    size_t length = query.len < sizeof(text) - 1 ? (size_t)query.len : sizeof(text) - 1;
    if (query.data) memcpy(text, query.data, length);
    text[length] = 0;
    return rae_device_media_matches(text) ? 1 : 0;
}

float rae_ext_Display_browserPixelRatio(void) {
    return (float)rae_device_pixel_ratio();
}

#else

/* How many touch devices SDL knows (0 before video is initialized) */
int64_t rae_ext_Device_touchDeviceCount(void) {
    int count = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&count);
    SDL_free(devices);
    return (int64_t)count;
}

/* Whether a mouse or trackpad is attached */
rae_Bool rae_ext_Device_hasMouse(void) {
    return SDL_HasMouse() ? 1 : 0;
}

/* Pixels per point of the display the window is on (0 without a window) */
float rae_ext_Display_windowDisplayScale(void) {
    return g_sdl_win ? SDL_GetWindowDisplayScale(g_sdl_win) : 0.0f;
}

/* The display's PANEL, for lib/Display.rae physicalPixelsPerPoint: a scaled
 * mode ("looks like 3008 x 1692" on a 3840 x 2160 panel) has macOS draw the
 * window at 2 pixels per point into a larger image and shrink it to the
 * panel, so the window's drawable pixels are not the pixels the screen
 * shows. CoreGraphics names the panel's own mode; which mode is the panel's
 * and what to do with it is Rae. Platform reason: CoreGraphics display
 * calls; no other platform has scaled modes, so there they answer 0
 * (unknown). The mode list functions copy the list each call: Rae calls them
 * a few times every couple of seconds. */
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
#include <CoreGraphics/CoreGraphics.h>

/* The display under the window's center (CGGetDisplaysWithPoint), 0 when none */
int64_t rae_ext_Display_windowPanelDisplay(void) {
    if (!g_sdl_win) return 0;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(g_sdl_win, &x, &y);
    SDL_GetWindowSize(g_sdl_win, &w, &h);
    CGDirectDisplayID id = 0;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(x + w * 0.5, y + h * 0.5), 1, &id, &count) != kCGErrorSuccess || count == 0) return 0;
    return (int64_t)id;
}

/* The display's width in points (its "looks like" width) */
float rae_ext_Display_panelPointsWide(int64_t display) {
    return (float)CGDisplayBounds((CGDirectDisplayID)display).size.width;
}

static CFArrayRef rae_display_modes(int64_t display) {
    const void* keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
    const void* values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes((CGDirectDisplayID)display, options);
    CFRelease(options);
    return modes;
}

/* How many modes the display has */
int64_t rae_ext_Display_panelModeCount(int64_t display) {
    CFArrayRef modes = rae_display_modes(display);
    if (!modes) return 0;
    int64_t count = (int64_t)CFArrayGetCount(modes);
    CFRelease(modes);
    return count;
}

/* Mode `index`'s width in pixels, or -1 when macOS does not mark it as the
 * panel's native mode (kDisplayModeNativeFlag) */
int64_t rae_ext_Display_panelNativeModePixelWidth(int64_t display, int64_t index) {
    CFArrayRef modes = rae_display_modes(display);
    if (!modes) return -1;
    int64_t width = -1;
    if (index >= 0 && index < (int64_t)CFArrayGetCount(modes)) {
        CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, (CFIndex)index);
        if (CGDisplayModeGetIOFlags(mode) & 0x02000000) width = (int64_t)CGDisplayModeGetPixelWidth(mode);
    }
    CFRelease(modes);
    return width;
}
#else
int64_t rae_ext_Display_windowPanelDisplay(void) { return 0; }
float rae_ext_Display_panelPointsWide(int64_t display) { (void)display; return 0.0f; }
int64_t rae_ext_Display_panelModeCount(int64_t display) { (void)display; return 0; }
int64_t rae_ext_Display_panelNativeModePixelWidth(int64_t display, int64_t index) { (void)display; (void)index; return -1; }
#endif

#endif
