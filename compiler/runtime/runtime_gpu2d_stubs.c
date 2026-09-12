/* gpu2d disabled-capability stubs. Temporary bridge for builds without SDL3/WebGPU capability.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

void rae_ext_Gpu2d_initWindow(int64_t w, int64_t h, rae_String t) { (void)w; (void)h; (void)t; }
rae_Bool rae_ext_Gpu2d_pollClose(void) { return 1; }
void rae_ext_Gpu2d_waitEvents(float timeoutSec){ (void)timeoutSec; }
int64_t rae_ext_Gpu2d_decodeImageProbe(rae_String path) { (void)path; return 0; }
float rae_ext_Gpu2d_pointerX(void){ return 0.0; }
float rae_ext_Gpu2d_pointerY(void){ return 0.0; }
int64_t rae_ext_Gpu2d_touchCount(void) { return 0; }
float rae_ext_Gpu2d_touchX(int64_t i) { (void)i; return 0.0; }
float rae_ext_Gpu2d_touchY(int64_t i) { (void)i; return 0.0; }
int64_t rae_ext_Gpu2d_touchId(int64_t i) { (void)i; return -1; }
rae_Bool rae_ext_Gpu2d_touchPressed(int64_t i) { (void)i; return 0; }
float rae_ext_Gpu2d_safeTop(void){ return 0.0; }
float rae_ext_Gpu2d_safeBottom(void){ return 0.0; }
float rae_ext_Gpu2d_safeLeft(void){ return 0.0; }
float rae_ext_Gpu2d_safeRight(void){ return 0.0; }
rae_Bool rae_ext_Gpu2d_pointerDown(void) { return 0; }
rae_Bool rae_ext_Gpu2d_pointerPressed(void) { return 0; }
rae_Bool rae_ext_Gpu2d_pointerReleased(void) { return 0; }
float rae_ext_Gpu2d_wheelMove(void){ return 0.0; }
void rae_ext_Gpu2d_setMouseCursor(int64_t kind) { (void)kind; }
double rae_ext_Gpu2d_nowSeconds(void){ return 0.0; }
int64_t rae_ext_Gpu2d_windowWidth(void) { return 0; }
int64_t rae_ext_Gpu2d_windowHeight(void) { return 0; }
void rae_ext_Gpu2d_setWindowPosition(int64_t x, int64_t y) { (void)x; (void)y; }
void rae_ext_Gpu2d_setWindowSize(int64_t w, int64_t h) { (void)w; (void)h; }
int64_t rae_ext_Gpu2d_windowPositionX(void) { return 0; }
int64_t rae_ext_Gpu2d_windowPositionY(void) { return 0; }
rae_Bool rae_ext_Gpu2d_windowResized(void) { return 0; }
rae_Bool rae_ext_Gpu2d_windowMoved(void) { return 0; }
void rae_ext_Gpu2d_setDesignResolution(float w, float h, int64_t fit){ (void)w; (void)h; (void)fit; }
float rae_ext_Gpu2d_designWidth(void){ return 0.0; }
float rae_ext_Gpu2d_designHeight(void){ return 0.0; }
float rae_ext_Gpu2d_dpr(void){ return 1.0; }
/* Frame lifecycle moved to Rae (#504); these back it, no-op without a GPU. */
void rae_g2d_frame_reset(void) {}
void rae_g2d_present(void* texture, int64_t width, int64_t height) { (void)texture; (void)width; (void)height; }
int64_t rae_g2d_surface_ready(void) { return 0; }
int64_t rae_g2d_surface_width(void) { return 0; }
int64_t rae_g2d_surface_height(void) { return 0; }
void rae_g2d_tick(void) {}
void rae_g2d_xform(float* out) { if (out) { for (int i = 0; i < 8; i++) out[i] = (i == 2 || i == 3) ? 1.0f : 0.0f; } }
void* rae_g2d_decode_image(rae_String path) { (void)path; return (void*)0; }
int64_t rae_g2d_decoded_width(void) { return 0; }
int64_t rae_g2d_decoded_height(void) { return 0; }
void rae_g2d_decode_free(void* rgba) { (void)rgba; }
rae_Bool rae_ext_Gpu2d_lastPresentOk(void) { return 0; }
int64_t rae_g2d_text_atlas_max(void) { return 8; }
void* rae_sdf_atlas_pixels(int64_t handle) { (void)handle; return (void*)0; }
int64_t rae_sdf_atlas_width(int64_t handle) { (void)handle; return 0; }
int64_t rae_sdf_atlas_height(int64_t handle) { (void)handle; return 0; }
void rae_ext_Gpu2d_closeWindow(void) {}
