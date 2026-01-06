#include "rae_raylib.h"
#include <raylib.h>

void rae_raylib_init_window(int64_t width, int64_t height, const char* title) {
    InitWindow((int)width, (int)height, title);
}

int64_t rae_raylib_window_should_close(void) {
    return WindowShouldClose() ? 1 : 0;
}

void rae_raylib_close_window(void) {
    CloseWindow();
}

void rae_raylib_begin_drawing(void) {
    BeginDrawing();
}

void rae_raylib_end_drawing(void) {
    EndDrawing();
}

void rae_raylib_clear_background(int64_t r, int64_t g, int64_t b, int64_t a) {
    ClearBackground((Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void rae_raylib_draw_rectangle(int64_t x, int64_t y, int64_t width, int64_t height, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void rae_raylib_draw_circle(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawCircle((int)x, (int)y, (float)radius, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void rae_raylib_draw_text(const char* text, int64_t x, int64_t y, int64_t fontSize, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawText(text, (int)x, (int)y, (int)fontSize, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void rae_raylib_set_target_fps(int64_t fps) {
    SetTargetFPS((int)fps);
}

int64_t rae_raylib_is_key_down(int64_t key) {
    return IsKeyDown((int)key) ? 1 : 0;
}
