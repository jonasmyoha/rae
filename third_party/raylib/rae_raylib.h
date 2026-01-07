#ifndef RAE_RAYLIB_H
#define RAE_RAYLIB_H

#include <stdint.h>

void Rae_InitWindow(int64_t width, int64_t height, const char* title);
int64_t Rae_WindowShouldClose(void);
void Rae_CloseWindow(void);
void Rae_BeginDrawing(void);
void Rae_EndDrawing(void);
void Rae_ClearBackground(int64_t r, int64_t g, int64_t b, int64_t a);
void Rae_DrawRectangle(int64_t x, int64_t y, int64_t width, int64_t height, int64_t r, int64_t g, int64_t b, int64_t a);
void Rae_DrawCircle(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a);
void Rae_DrawText(const char* text, int64_t x, int64_t y, int64_t fontSize, int64_t r, int64_t g, int64_t b, int64_t a);
void Rae_SetTargetFPS(int64_t fps);
int64_t Rae_IsKeyDown(int64_t key);

#endif
