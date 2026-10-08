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

/* How many touch devices SDL knows (0 before video is initialised) */
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

#endif
