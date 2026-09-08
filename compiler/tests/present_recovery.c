#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <webgpu/webgpu.h>

static void* g_sdl_win = (void*)1;
static void* g_g2d_surface = (void*)1;
static int configureCount;
static int drawableWidth = 800, drawableHeight = 600;
static int configuredWidth, configuredHeight;
static void SDL_GetWindowSizeInPixels(void* window, int* width, int* height) {
    (void)window;
    *width = drawableWidth; *height = drawableHeight;
}
static void rae_g2d_configure(int width, int height) {
    if (width <= 0 || height <= 0) return;
    configureCount++;
    configuredWidth = width; configuredHeight = height;
}

/* Extracted verbatim from the production file, not a duplicate policy. */
#include "present_recovery.inc"

int main(void) {
    assert(strcmp(rae_present_status_name(196609), "Occluded") == 0);
    for (int i = 0; i < 1000; ++i) rae_present_recover("gpu3d", 196609);
    assert(configureCount == 0);
    rae_present_note_ok();
    assert(g_rae_present_skip_streak == 0);
    for (int i = 0; i < 1000; ++i)
        rae_present_recover("gpu2d", WGPUSurfaceGetCurrentTextureStatus_Timeout);
    assert(configureCount == 0);
    rae_present_note_ok();
    drawableWidth = 1600; drawableHeight = 1200;
    rae_present_recover("gpu3d", WGPUSurfaceGetCurrentTextureStatus_Outdated);
    assert(configureCount == 1 && configuredWidth == 1600 && configuredHeight == 1200);
    rae_present_note_ok();
    for (int i = 0; i < 240; ++i)
        rae_present_recover("gpu3d", WGPUSurfaceGetCurrentTextureStatus_Lost);
    assert(configureCount == 6); /* three immediate, then frames 120 and 240 */
    rae_present_note_ok();
    drawableWidth = drawableHeight = 0;
    rae_present_recover("gpu2d", WGPUSurfaceGetCurrentTextureStatus_Outdated);
    assert(configureCount == 6);
    puts("PASS: present recovery (occluded/timeout retry, success reset, resize, bounded repair)");
    return 0;
}
