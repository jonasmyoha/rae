/* gpu2d box/shape pipeline and draw queue. Raw pipeline calls stay C; render batching policy is a future Rae migration candidate.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* --- Box uber-shader pipeline (#110) ----------------------------------
 * Instanced rounded-box SDF with analytic AA: one quad per primitive, the
 * fragment shader evaluates a rounded-box signed distance and antialiases
 * with screen-space derivatives (no MSAA). One pipeline → filled/rounded
 * rects, per-corner radius, borders. Primitives are accumulated CPU-side
 * each frame and drawn in one instanced draw at endFrame (painter's order).
 * Instance layout = 6×vec4 (std430): rect, radius, fill, border, params, grad. */
#define G2D_PRIM_FLOATS 24

/* The box WGSL is a Rae asset since #907: lib/gpu2d_box.wgsl. */

/* #907: the box pipeline, its per-flush instance buffers and bind groups are
 * typed IDs in a Rae-owned gpu/GpuResources manager (lib/Gpu2dBox.rae); C keeps
 * the CPU-side batch (the prims array + per-prim clip index, pushed by the draw
 * entry points below, read back through the rae_g2d_prim_* accessors) and the
 * shared viewport uniform every 2D pipeline binds at @binding(0). */
static WGPUBuffer    g_g2d_uniform = NULL;
static float* g_g2d_prims = NULL;      /* CPU accumulation (floats) */
static int   g_g2d_prim_count = 0;
static int   g_g2d_prim_capf = 0;      /* capacity in floats */

static void g2d_color(uint32_t c, float* out) {
    /* 0xAARRGGBB -> premultiplied RGBA in 0..1 */
    float a = (float)((c >> 24) & 0xFF) / 255.0f;
    float r = (float)((c >> 16) & 0xFF) / 255.0f;
    float g = (float)((c >> 8)  & 0xFF) / 255.0f;
    float b = (float)( c        & 0xFF) / 255.0f;
    out[0] = r * a; out[1] = g * a; out[2] = b * a; out[3] = a;
}

static void rae_g2d_push(double x, double y, double w, double h,
                         double rtl, double rtr, double rbr, double rbl,
                         uint32_t fill, uint32_t border, double bw, double angle) {
    int need = (g_g2d_prim_count + 1) * G2D_PRIM_FLOATS;
    if (need > g_g2d_prim_capf) {
        int cap = g_g2d_prim_capf ? g_g2d_prim_capf : (64 * G2D_PRIM_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_prims = (float*)realloc(g_g2d_prims, (size_t)cap * sizeof(float));
        g_g2d_prim_capf = cap;
    }
    float* p = g_g2d_prims + g_g2d_prim_count * G2D_PRIM_FLOATS;
    p[0]=(float)x; p[1]=(float)y; p[2]=(float)w; p[3]=(float)h;
    p[4]=(float)rtl; p[5]=(float)rtr; p[6]=(float)rbr; p[7]=(float)rbl;
    g2d_color(fill, &p[8]);
    g2d_color(border, &p[12]);
    p[16]=(float)bw; p[17]=(float)angle; p[18]=0.0f; p[19]=0.0f;
    g2d_color(fill, &p[20]);
    rae_g2d_prim_clip_ensure(g_g2d_prim_count + 1);
    g_g2d_prim_clip[g_g2d_prim_count] = g_g2d_cur_clip;
    g_g2d_prim_count++;
}

static void rae_g2d_push_gradient(double x, double y, double w, double h,
                                  double radius, uint32_t from, uint32_t to,
                                  double angle_deg) {
    int need = (g_g2d_prim_count + 1) * G2D_PRIM_FLOATS;
    if (need > g_g2d_prim_capf) {
        int cap = g_g2d_prim_capf ? g_g2d_prim_capf : (64 * G2D_PRIM_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_prims = (float*)realloc(g_g2d_prims, (size_t)cap * sizeof(float));
        g_g2d_prim_capf = cap;
    }
    float* p = g_g2d_prims + g_g2d_prim_count * G2D_PRIM_FLOATS;
    p[0]=(float)x; p[1]=(float)y; p[2]=(float)w; p[3]=(float)h;
    p[4]=(float)radius; p[5]=(float)radius; p[6]=(float)radius; p[7]=(float)radius;
    g2d_color(from, &p[8]);
    g2d_color(0, &p[12]);
    p[16]=0.0f;
    p[17]=0.0f;
    p[18]=1.0f;
    p[19]=(float)(angle_deg * 0.017453292519943295);
    g2d_color(to, &p[20]);
    rae_g2d_prim_clip_ensure(g_g2d_prim_count + 1);
    g_g2d_prim_clip[g_g2d_prim_count] = g_g2d_cur_clip;
    g_g2d_prim_count++;
}

/* The shared viewport uniform (2 x vec4: physW, physH, scaleX, scaleY /
 * offsetX, offsetY, ..). Created on first use; the image and text bind groups
 * reference it at binding 0 and the Rae box pass adopts it as an external. */
static void rae_g2d_ensure_viewport_uniform(void) {
    if (g_g2d_uniform) return;
    WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
    ud.size = 32; ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;  /* 2*vec4 xform */
    g_g2d_uniform = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
}
void* rae_g2d_viewport_uniform(void) { rae_g2d_ensure_viewport_uniform(); return (void*)g_g2d_uniform; }

/* Batch accessors for the Rae box pass (#907): the packed prims, their count
 * and stride, each prim's clip index, and the reset after a flush. */
int64_t rae_g2d_prim_count(void)  { return (int64_t)g_g2d_prim_count; }
int64_t rae_g2d_prim_floats(void) { return (int64_t)G2D_PRIM_FLOATS; }
void*   rae_g2d_prim_data(void)   { return (void*)g_g2d_prims; }
int64_t rae_g2d_prim_clip_at(int64_t i) {
    return (g_g2d_prim_clip && i >= 0 && i < g_g2d_prim_clip_cap) ? (int64_t)g_g2d_prim_clip[(int)i] : 0;
}
void    rae_g2d_prim_reset(void)  { g_g2d_prim_count = 0; }

void rae_ext_Gpu2d_drawRect(float x, float y, float w, float h, int64_t color){
    rae_g2d_push(x, y, w, h, 0, 0, 0, 0, (uint32_t)color, 0, 0.0, 0.0);
}
void rae_ext_Gpu2d_drawRoundedRect(float x, float y, float w, float h, float radius, int64_t color){
    rae_g2d_push(x, y, w, h, radius, radius, radius, radius, (uint32_t)color, 0, 0.0, 0.0);
}
void rae_ext_Gpu2d_drawBox(float x, float y, float w, float h, float radius,
                           int64_t fill, float borderWidth, int64_t border){
    rae_g2d_push(x, y, w, h, radius, radius, radius, radius,
                 (uint32_t)fill, (uint32_t)border, borderWidth, 0.0);
}
void rae_ext_Gpu2d_drawGradientRect(float x, float y, float w, float h,
                                    float radius, int64_t from, int64_t to,
                                    float angleDeg){
    rae_g2d_push_gradient(x, y, w, h, radius, (uint32_t)from, (uint32_t)to, angleDeg);
}
/* A line from (x0,y0) to (x1,y1), `thickness` px wide, with rounded caps —
 * a rotated capsule (rounded rect of length x thickness, radius thickness/2). */
void rae_ext_Gpu2d_drawLine(float x0, float y0, float x1, float y1,
                            float thickness, int64_t color){
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6 || thickness <= 0.0) return;
    double angle = atan2(dy, dx);
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double r = thickness * 0.5;
    if (r > len * 0.5) r = len * 0.5;
    rae_g2d_push(cx - len * 0.5, cy - thickness * 0.5, len, thickness,
                 r, r, r, r, (uint32_t)color, 0, 0.0, angle);
}

