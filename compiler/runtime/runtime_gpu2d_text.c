/* gpu2d MSDF text: the CPU atlas pixel accessors. Pipeline, batch and layout are Rae.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 */

/* The text WGSL is a Rae asset since #909: lib/gpu2d_text.wgsl. */

/* #909/#915: the text pipeline, sampler, atlas textures + views, per-atlas
 * instance buffers, bind groups AND the per-atlas CPU glyph batch are Rae
 * (lib/Gpu2dCanvasText.rae); C keeps only the CPU atlas pixels
 * (runtime_image_sdl3.c, read back through rae_sdf_atlas_*). */
int64_t rae_g2d_text_atlas_max(void) { return (int64_t)RAE_SDF_MAX_ATLAS; }
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
