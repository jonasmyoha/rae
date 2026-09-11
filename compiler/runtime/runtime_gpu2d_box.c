/* gpu2d box/shape pipeline and draw queue. Raw pipeline calls stay C; render batching policy is a future Rae migration candidate.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* --- Box uber-shader pipeline (#110) ----------------------------------
 * Since #907/#913 the whole box pass — shader (lib/gpu2d_box.wgsl), pipeline,
 * batch, instance buffers, draw — is Rae (lib/Gpu2dCanvas.rae). */

/* What remains here is the shared viewport uniform every 2D pipeline binds at
 * @binding(0) (adopted by the canvas as an external, #907). */
static WGPUBuffer    g_g2d_uniform = NULL;
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
