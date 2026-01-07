#ifndef RAE_RAYLIB_H
#define RAE_RAYLIB_H

#include <stdint.h>

void initWindow(int64_t width, int64_t height, const char* title);
int64_t windowShouldClose(void);
void closeWindow(void);
void beginDrawing(void);
void endDrawing(void);
void clearBackground(int64_t r, int64_t g, int64_t b, int64_t a);
void drawRectangle(int64_t x, int64_t y, int64_t width, int64_t height, int64_t r, int64_t g, int64_t b, int64_t a);
void drawCircle(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a);
void drawText(const char* text, int64_t x, int64_t y, int64_t fontSize, int64_t r, int64_t g, int64_t b, int64_t a);
void setTargetFPS(int64_t fps);
int64_t isKeyDown(int64_t key);

#endif
