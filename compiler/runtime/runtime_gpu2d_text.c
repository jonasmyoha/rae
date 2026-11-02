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
/* How many atlas handles exist (1..count); the table grows, so this is the
 * current count, not a fixed maximum. */
int64_t rae_g2d_text_atlas_max(void) { return rae_sdf_atlas_count(); }
/* The CPU MSDF atlas `handle` (1-based, from SdfText.loadAtlas): its RGBA8
 * pixels + size, for the Rae upload into a manager texture; NULL if unloaded. */
void* rae_sdf_atlas_pixels(int64_t handle) {
    const unsigned char* px = NULL; int w = 0, h = 0;
    return rae_sdf_atlas_get(handle, &px, &w, &h) ? (void*)px : NULL;
}
int64_t rae_sdf_atlas_width(int64_t handle) {
    const unsigned char* px = NULL; int w = 0, h = 0;
    return rae_sdf_atlas_get(handle, &px, &w, &h) ? (int64_t)w : 0;
}
int64_t rae_sdf_atlas_height(int64_t handle) {
    const unsigned char* px = NULL; int w = 0, h = 0;
    return rae_sdf_atlas_get(handle, &px, &w, &h) ? (int64_t)h : 0;
}
