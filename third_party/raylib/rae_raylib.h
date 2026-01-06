#ifndef RAE_RAYLIB_H
#define RAE_RAYLIB_H

#include <stdint.h>

void rae_raylib_init_window(int64_t width, int64_t height, const char* title);
int64_t rae_raylib_window_should_close(void);
void rae_raylib_close_window(void);
void rae_raylib_begin_drawing(void);
void rae_raylib_end_drawing(void);
void rae_raylib_clear_background(int64_t r, int64_t g, int64_t b, int64_t a);
void rae_raylib_draw_rectangle(int64_t x, int64_t y, int64_t width, int64_t height, int64_t r, int64_t g, int64_t b, int64_t a);
void rae_raylib_draw_circle(int64_t x, int64_t y, int64_t radius, int64_t r, int64_t g, int64_t b, int64_t a);
void rae_raylib_draw_text(const char* text, int64_t x, int64_t y, int64_t fontSize, int64_t r, int64_t g, int64_t b, int64_t a);
void rae_raylib_set_target_fps(int64_t fps);
int64_t rae_raylib_is_key_down(int64_t key);

#endif
