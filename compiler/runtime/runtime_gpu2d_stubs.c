/* gpu2d disabled-capability stubs. Temporary bridge for builds without SDL3/WebGPU capability.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

void rae_ext_gpu2d_initWindow(int64_t w, int64_t h, rae_String t) { (void)w; (void)h; (void)t; }
rae_Bool rae_ext_gpu2d_pollClose(void) { return 1; }
void rae_ext_gpu2d_waitEvents(float timeoutSec){ (void)timeoutSec; }
int64_t rae_ext_gpu2d_loadImage(rae_String path) { (void)path; return 0; }
int64_t rae_ext_gpu2d_decodeImageProbe(rae_String path) { (void)path; return 0; }
int64_t rae_ext_gpu2d_loadImageKey(rae_String key, rae_String path) { (void)key; (void)path; return 0; }
rae_Bool rae_ext_gpu2d_hasImageKey(rae_String key) { (void)key; return 0; }
void rae_ext_gpu2d_drawImageKey(rae_String key, float x, float y, float w, float h, float radius, int64_t tint){ (void)key; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; }
void rae_ext_gpu2d_drawImageKeyScaled(rae_String key, float x, float y, float w, float h, float radius, int64_t tint, int64_t scaleMode){ (void)key; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; (void)scaleMode; }
void rae_ext_gpu2d_drawImage(float x, float y, float w, float h, float radius, int64_t handle, int64_t tint){ (void)x; (void)y; (void)w; (void)h; (void)radius; (void)handle; (void)tint; }
float rae_ext_gpu2d_pointerX(void){ return 0.0; }
float rae_ext_gpu2d_pointerY(void){ return 0.0; }
int64_t rae_ext_gpu2d_touchCount(void) { return 0; }
float rae_ext_gpu2d_touchX(int64_t i) { (void)i; return 0.0; }
float rae_ext_gpu2d_touchY(int64_t i) { (void)i; return 0.0; }
int64_t rae_ext_gpu2d_touchId(int64_t i) { (void)i; return -1; }
rae_Bool rae_ext_gpu2d_touchPressed(int64_t i) { (void)i; return 0; }
float rae_ext_gpu2d_safeTop(void){ return 0.0; }
float rae_ext_gpu2d_safeBottom(void){ return 0.0; }
float rae_ext_gpu2d_safeLeft(void){ return 0.0; }
float rae_ext_gpu2d_safeRight(void){ return 0.0; }
rae_Bool rae_ext_gpu2d_pointerDown(void) { return 0; }
rae_Bool rae_ext_gpu2d_pointerPressed(void) { return 0; }
rae_Bool rae_ext_gpu2d_pointerReleased(void) { return 0; }
float rae_ext_gpu2d_wheelMove(void){ return 0.0; }
void rae_ext_gpu2d_setMouseCursor(int64_t kind) { (void)kind; }
double rae_ext_gpu2d_nowSeconds(void){ return 0.0; }
int64_t rae_ext_gpu2d_windowWidth(void) { return 0; }
int64_t rae_ext_gpu2d_windowHeight(void) { return 0; }
void rae_ext_gpu2d_setWindowPosition(int64_t x, int64_t y) { (void)x; (void)y; }
void rae_ext_gpu2d_setWindowSize(int64_t w, int64_t h) { (void)w; (void)h; }
int64_t rae_ext_gpu2d_windowPositionX(void) { return 0; }
int64_t rae_ext_gpu2d_windowPositionY(void) { return 0; }
rae_Bool rae_ext_gpu2d_windowResized(void) { return 0; }
rae_Bool rae_ext_gpu2d_windowMoved(void) { return 0; }
void rae_ext_gpu2d_setDesignResolution(float w, float h, int64_t fit){ (void)w; (void)h; (void)fit; }
float rae_ext_gpu2d_designWidth(void){ return 0.0; }
float rae_ext_gpu2d_designHeight(void){ return 0.0; }
float rae_ext_gpu2d_dpr(void){ return 1.0; }
/* Frame lifecycle moved to Rae (#504); these back it, no-op without a GPU. */
void rae_g2d_frame_reset(void) {}
void rae_g2d_set_frame(void* enc, void* pass){ (void)enc; (void)pass; }
void* rae_g2d_pass_get(void)    { return (void*)0; }
void* rae_g2d_encoder_get(void) { return (void*)0; }
int64_t rae_g2d_frame_active(void) { return 0; }
void rae_g2d_present_and_cleanup(void) {}
void rae_g2d_tick(void) {}
rae_Bool rae_ext_gpu2d_lastPresentOk(void) { return 0; }
void rae_ext_gpu2d_flush(void) {}
void rae_ext_gpu2d_closeWindow(void) {}
void rae_ext_gpu2d_drawRect(float x, float y, float w, float h, int64_t color){ (void)x; (void)y; (void)w; (void)h; (void)color; }
void rae_ext_gpu2d_drawRoundedRect(float x, float y, float w, float h, float radius, int64_t color){ (void)x; (void)y; (void)w; (void)h; (void)radius; (void)color; }
void rae_ext_gpu2d_drawBox(float x, float y, float w, float h, float radius, int64_t fill, float borderWidth, int64_t border){ (void)x; (void)y; (void)w; (void)h; (void)radius; (void)fill; (void)borderWidth; (void)border; }
void rae_ext_gpu2d_drawGradientRect(float x, float y, float w, float h, float radius, int64_t from, int64_t to, float angleDeg){ (void)x; (void)y; (void)w; (void)h; (void)radius; (void)from; (void)to; (void)angleDeg; }
void rae_ext_gpu2d_drawLine(float x0, float y0, float x1, float y1, float thickness, int64_t color){ (void)x0; (void)y0; (void)x1; (void)y1; (void)thickness; (void)color; }
void rae_ext_gpu2d_drawGlyph(float sx0, float sy0, float sx1, float sy1, float u0, float v0, float u1, float v1, int64_t atlas, float pxRange, int64_t color){ (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)u0; (void)v0; (void)u1; (void)v1; (void)atlas; (void)pxRange; (void)color; }
void rae_ext_gpu2d_drawGlyphEx(float sx0, float sy0, float sx1, float sy1, float u0, float v0, float u1, float v1, int64_t atlas, float pxRange, int64_t color, float outlineWidth, int64_t outlineColor, float softness){ (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)u0; (void)v0; (void)u1; (void)v1; (void)atlas; (void)pxRange; (void)color; (void)outlineWidth; (void)outlineColor; (void)softness; }
