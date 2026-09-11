/* gpu2d MSDF text pipeline and glyph queue. Raw atlas upload/pipeline calls stay C; text layout and style policy belongs in Rae.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* --- Text pipeline (#111): MSDF glyph quads --------------------------
 * A second instanced pipeline that samples the MSDF atlas (the same raw
 * RGBA the CPU blit path holds in g_sdf_atlas[]) and antialiases with the
 * median-of-3 + screen-px-range trick. Shares the viewport uniform and
 * premultiplied blend with the box pipeline, so glyphs composite in the
 * same pass after boxes. Instance = 4*vec4: rect, uv (normalised), colour,
 * params(pxRange). One atlas/font per frame (the common UI case). */
#define G2D_TEXT_FLOATS 20

/* The text WGSL is a Rae asset since #909: lib/gpu2d_text.wgsl. */

/* #909: the text pipeline, sampler, atlas textures + views, per-atlas instance
 * buffers and bind groups are manager IDs on the Rae side (lib/Gpu2dCanvas.rae);
 * C keeps the per-atlas CPU glyph batch (accumulated by the draw entry points
 * below, read back through the rae_g2d_text_* accessors) and the CPU atlas
 * pixels (runtime_image_sdl3.c, read back through rae_sdf_atlas_*). */
static float* g_g2d_text_prims[RAE_SDF_MAX_ATLAS]; /* CPU float accumulation */
static int   g_g2d_text_count[RAE_SDF_MAX_ATLAS];  /* glyphs this frame */
static int   g_g2d_text_capf[RAE_SDF_MAX_ATLAS];   /* float capacity */

int64_t rae_g2d_text_atlas_max(void) { return (int64_t)RAE_SDF_MAX_ATLAS; }
int64_t rae_g2d_text_floats(void)    { return (int64_t)G2D_TEXT_FLOATS; }
int64_t rae_g2d_text_count(int64_t ai) {
    return (ai >= 0 && ai < RAE_SDF_MAX_ATLAS) ? (int64_t)g_g2d_text_count[(int)ai] : 0;
}
void* rae_g2d_text_data(int64_t ai) {
    return (ai >= 0 && ai < RAE_SDF_MAX_ATLAS) ? (void*)g_g2d_text_prims[(int)ai] : NULL;
}
int64_t rae_g2d_text_clip_at(int64_t ai, int64_t i) {
    if (ai < 0 || ai >= RAE_SDF_MAX_ATLAS) return 0;
    int* tclip = g_g2d_text_clip[(int)ai];
    return (tclip && i >= 0 && i < g_g2d_text_clip_cap[(int)ai]) ? (int64_t)tclip[(int)i] : 0;
}
void rae_g2d_text_reset(int64_t ai) {
    if (ai >= 0 && ai < RAE_SDF_MAX_ATLAS) g_g2d_text_count[(int)ai] = 0;
}
/* The CPU MSDF atlas `handle` (1-based, from SdfText.loadAtlas): its RGBA8
 * pixels + size, for the Rae upload into a manager texture; NULL if unloaded. */
void* rae_sdf_atlas_pixels(int64_t handle) {
    int i = (int)handle - 1;
    return (i >= 0 && i < RAE_SDF_MAX_ATLAS) ? (void*)g_sdf_atlas[i] : NULL;
}
int64_t rae_sdf_atlas_width(int64_t handle) {
    int i = (int)handle - 1;
    return (i >= 0 && i < RAE_SDF_MAX_ATLAS && g_sdf_atlas[i]) ? (int64_t)g_sdf_atlas_w[i] : 0;
}
int64_t rae_sdf_atlas_height(int64_t handle) {
    int i = (int)handle - 1;
    return (i >= 0 && i < RAE_SDF_MAX_ATLAS && g_sdf_atlas[i]) ? (int64_t)g_sdf_atlas_h[i] : 0;
}

/* Full glyph submit with an optional outline (outlineWidth px + colour) and
 * softness (edge-falloff width in px; 1 = crisp, larger = soft/blurred). */
void rae_ext_Gpu2d_drawGlyphEx(float sx0, float sy0, float sx1, float sy1,
                               float u0, float v0, float u1, float v1,
                               int64_t atlas, float pxRange, int64_t color,
                               float outlineWidth, int64_t outlineColor, float softness){
    int ai = (int)atlas - 1;
    if (ai < 0 || ai >= RAE_SDF_MAX_ATLAS) return;
    int need = (g_g2d_text_count[ai] + 1) * G2D_TEXT_FLOATS;
    if (need > g_g2d_text_capf[ai]) {
        int cap = g_g2d_text_capf[ai] ? g_g2d_text_capf[ai] : (256 * G2D_TEXT_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_text_prims[ai] = (float*)realloc(g_g2d_text_prims[ai], (size_t)cap * sizeof(float));
        g_g2d_text_capf[ai] = cap;
    }
    float* p = g_g2d_text_prims[ai] + g_g2d_text_count[ai] * G2D_TEXT_FLOATS;
    p[0]=(float)sx0; p[1]=(float)sy0; p[2]=(float)(sx1-sx0); p[3]=(float)(sy1-sy0);
    p[4]=(float)u0; p[5]=(float)v0; p[6]=(float)(u1-u0); p[7]=(float)(v1-v0);
    /* straight (non-premultiplied) colour 0xAARRGGBB; the shader premultiplies */
    uint32_t c = (uint32_t)color;
    p[8]  = (float)((c >> 16) & 0xFF) / 255.0f;
    p[9]  = (float)((c >> 8)  & 0xFF) / 255.0f;
    p[10] = (float)( c        & 0xFF) / 255.0f;
    p[11] = (float)((c >> 24) & 0xFF) / 255.0f;
    p[12]=(float)pxRange; p[13]=(float)outlineWidth; p[14]=(float)softness; p[15]=0.0f;
    uint32_t oc = (uint32_t)outlineColor;
    p[16] = (float)((oc >> 16) & 0xFF) / 255.0f;
    p[17] = (float)((oc >> 8)  & 0xFF) / 255.0f;
    p[18] = (float)( oc        & 0xFF) / 255.0f;
    p[19] = (float)((oc >> 24) & 0xFF) / 255.0f;
    rae_g2d_text_clip_ensure(ai, g_g2d_text_count[ai] + 1);
    g_g2d_text_clip[ai][g_g2d_text_count[ai]] = g_g2d_cur_clip;
    g_g2d_text_count[ai]++;
}

/* Back-compat: glyph with no outline. */
void rae_ext_Gpu2d_drawGlyph(float sx0, float sy0, float sx1, float sy1,
                             float u0, float v0, float u1, float v1,
                             int64_t atlas, float pxRange, int64_t color){
    rae_ext_Gpu2d_drawGlyphEx(sx0, sy0, sx1, sy1, u0, v0, u1, v1, atlas, pxRange, color, 0.0, 0, 1.0);
}

