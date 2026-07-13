/* gpu2d disabled-capability stubs. Temporary bridge for builds without SDL3/WebGPU capability.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

void rae_ext_gpu2d_initWindow(int64_t w, int64_t h, rae_String t) { (void)w; (void)h; (void)t; }
rae_Bool rae_ext_gpu2d_pollClose(void) { return 1; }
void rae_ext_gpu2d_waitEvents(double timeoutSec) { (void)timeoutSec; }
int64_t rae_ext_gpu2d_loadImage(rae_String path) { (void)path; return 0; }
int64_t rae_ext_gpu2d_decodeImageProbe(rae_String path) { (void)path; return 0; }
int64_t rae_ext_gpu2d_loadImageKey(rae_String key, rae_String path) { (void)key; (void)path; return 0; }
rae_Bool rae_ext_gpu2d_hasImageKey(rae_String key) { (void)key; return 0; }
void rae_ext_gpu2d_drawImageKey(rae_String key, double x, double y, double w, double h, double radius, int64_t tint) { (void)key; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; }
void rae_ext_gpu2d_drawImageKeyScaled(rae_String key, double x, double y, double w, double h, double radius, int64_t tint, int64_t scaleMode) { (void)key; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; (void)scaleMode; }
void rae_ext_gpu2d_drawImage(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)handle; (void)tint; }
double rae_ext_gpu2d_pointerX(void) { return 0.0; }
double rae_ext_gpu2d_pointerY(void) { return 0.0; }
rae_Bool rae_ext_gpu2d_pointerDown(void) { return 0; }
rae_Bool rae_ext_gpu2d_pointerPressed(void) { return 0; }
rae_Bool rae_ext_gpu2d_pointerReleased(void) { return 0; }
double rae_ext_gpu2d_wheelMove(void) { return 0.0; }
void rae_ext_gpu2d_setMouseCursor(int64_t kind) { (void)kind; }
double rae_ext_gpu2d_nowSeconds(void) { return 0.0; }
int64_t rae_ext_gpu2d_windowWidth(void) { return 0; }
int64_t rae_ext_gpu2d_windowHeight(void) { return 0; }
void rae_ext_gpu2d_setWindowPosition(int64_t x, int64_t y) { (void)x; (void)y; }
int64_t rae_ext_gpu2d_windowPositionX(void) { return 0; }
int64_t rae_ext_gpu2d_windowPositionY(void) { return 0; }
rae_Bool rae_ext_gpu2d_windowResized(void) { return 0; }
void rae_ext_gpu2d_setDesignResolution(double w, double h, int64_t fit) { (void)w; (void)h; (void)fit; }
double rae_ext_gpu2d_designWidth(void) { return 0.0; }
double rae_ext_gpu2d_designHeight(void) { return 0.0; }
double rae_ext_gpu2d_dpr(void) { return 1.0; }
void rae_ext_gpu2d_beginFrame(double r, double g, double b, double a) { (void)r; (void)g; (void)b; (void)a; }
void rae_ext_gpu2d_endFrame(void) {}
rae_Bool rae_ext_gpu2d_lastPresentOk(void) { return 0; }
void rae_ext_gpu2d_flush(void) {}
void rae_ext_gpu2d_closeWindow(void) {}
void rae_ext_gpu2d_drawRect(double x, double y, double w, double h, int64_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
void rae_ext_gpu2d_drawRoundedRect(double x, double y, double w, double h, double radius, int64_t color) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)color; }
void rae_ext_gpu2d_drawBox(double x, double y, double w, double h, double radius, int64_t fill, double borderWidth, int64_t border) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)fill; (void)borderWidth; (void)border; }
void rae_ext_gpu2d_drawGradientRect(double x, double y, double w, double h, double radius, int64_t from, int64_t to, double angleDeg) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)from; (void)to; (void)angleDeg; }
void rae_ext_gpu2d_drawLine(double x0, double y0, double x1, double y1, double thickness, int64_t color) { (void)x0; (void)y0; (void)x1; (void)y1; (void)thickness; (void)color; }
void rae_ext_gpu2d_drawGlyph(double sx0, double sy0, double sx1, double sy1, double u0, double v0, double u1, double v1, int64_t atlas, double pxRange, int64_t color) { (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)u0; (void)v0; (void)u1; (void)v1; (void)atlas; (void)pxRange; (void)color; }
void rae_ext_gpu2d_drawGlyphEx(double sx0, double sy0, double sx1, double sy1, double u0, double v0, double u1, double v1, int64_t atlas, double pxRange, int64_t color, double outlineWidth, int64_t outlineColor, double softness) { (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)u0; (void)v0; (void)u1; (void)v1; (void)atlas; (void)pxRange; (void)color; (void)outlineWidth; (void)outlineColor; (void)softness; }
