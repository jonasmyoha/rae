#include "rae_raylib.h"
#include <raylib.h>

void initWindow(int64_t width, int64_t height, const char* title) {
    InitWindow((int)width, (int)height, title);
}

int64_t windowShouldClose(void) {
    return WindowShouldClose() ? 1 : 0;
}

void closeWindow(void) {
    CloseWindow();
}

void beginDrawing(void) {
    BeginDrawing();
}

void endDrawing(void) {
    EndDrawing();
}

void clearBackground(int64_t r, int64_t g, int64_t b, int64_t a) {
    ClearBackground((Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void drawRectangle(int64_t x, int64_t y, int64_t width, int64_t height, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void drawCircle(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawCircle((int)x, (int)y, (float)radius, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void drawText(const char* text, int64_t x, int64_t y, int64_t fontSize, int64_t r, int64_t g, int64_t b, int64_t a) {
    DrawText(text, (int)x, (int)y, (int)fontSize, (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a});
}

void setTargetFPS(int64_t fps) {
    SetTargetFPS((int)fps);
}

int64_t isKeyDown(int64_t key) {
    return IsKeyDown((int)key) ? 1 : 0;
}
