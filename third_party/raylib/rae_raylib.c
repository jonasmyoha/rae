#include "rae_raylib.h"
#include <raylib.h>

void Rae_InitWindow(int64_t width, int64_t height, const char* title) {
    InitWindow((int)width, (int)height, title);
}

int64_t Rae_WindowShouldClose(void) {
    return WindowShouldClose() ? 1 : 0;
}

void Rae_CloseWindow(void) {
    CloseWindow();
}

void Rae_BeginDrawing(void) {
    BeginDrawing();
}

void Rae_EndDrawing(void) {
    EndDrawing();
}

void Rae_ClearBackground(int64_t r, int64_t g, int64_t b, int64_t a) {
    ClearBackground((Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void Rae_DrawRectangle(int64_t x, int64_t y, int64_t width, int64_t height, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void Rae_DrawCircle(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawCircle((int)x, (int)y, (float)radius, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void Rae_DrawText(const char* text, int64_t x, int64_t y, int64_t fontSize, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawText(text, (int)x, (int)y, (int)fontSize, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void Rae_SetTargetFPS(int64_t fps) {
    SetTargetFPS((int)fps);
}

int64_t Rae_IsKeyDown(int64_t key) {
    return IsKeyDown((int)key) ? 1 : 0;
}
