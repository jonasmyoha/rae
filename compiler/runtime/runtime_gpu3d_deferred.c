#include "runtime_sky_state.h"

/* Deferred frame — depth pyramid, lighting, composite (#356, Track B).
 *
 * The passes that consume what runtime_gpu3d_gbuffer.c produces. Split
 * into its own file because the geometry pass and the passes that read it
 * are separate concerns that change for different reasons, and because
 * runtime_gpu3d.c is already the cautionary tale for letting a renderer
 * family accumulate in one file (queue #364).
 *
 * DEPTH PYRAMID. A mip chain of depth reduced to the NEAREST value, not an
 * average. The consumer is occlusion: "is anything in this screen region
 * closer than my bounding box" is answered by the nearest depth, and an
 * averaged pyramid would report a plausible-looking value that is
 * conservative in neither direction — it would cull visible geometry.
 * Under reverse-Z (#367) nearest is the MAXIMUM, so the reduction is max;
 * it was min before the depth convention flipped, and a pyramid reducing
 * the wrong way is invisible until something culls with it.
 * Built with fragment passes rather than compute for the same reason the
 * SSAO pass is: they run everywhere WebGPU runs, with no workgroup-size
 * guess. Hi-Z culling and a deferred SSAO are the intended readers; the
 * chain exists now so those arrive as consumers of a real resource rather
 * than as a reshaping of the frame.
 *
 * LIGHTING. One fullscreen pass, Cook-Torrance GGX + Smith + Schlick over
 * the G-buffer, writing LINEAR HDR radiance into litColor. This is the
 * payoff of the whole split: its cost is one evaluation per pixel and does
 * not scale with how many objects wrote that pixel. World position is
 * reconstructed from depth and the inverse view-projection rather than
 * read from a stored position buffer — the reason the G-buffer has no
 * position channel.
 *
 * COMPOSITE. Exposure + ACES + gamma, litColor -> the presentable
 * offscreen. Identical tone curve to the forward frame's tonemap on
 * purpose: the two frames should differ in how radiance is computed, not
 * in how it is displayed, or comparing them proves nothing.
 */

#define GB_PYRAMID_MAX_MIPS 16
/* mat4 invViewProj + 6 vec4 + mat4 viewProj (#387 march) + 3 vec4 of sky
 * (#400/#404). The sky rides the LIGHTING uniform rather than getting its
 * own buffer because the background and the ambient are the same
 * environment: two buffers is two chances for the sky you see and the
 * sky you are lit by to be a frame apart. */
#define GB_LIGHT_BYTES 416

/* #923: the lit / litCopy / AO / TAA / pyramid targets, the light + TAA
 * uniforms and the lighting / TAA / pyramid pipelines are Rae manager objects
 * on the DeferredRenderer (lib/GbufferTargets.rae). The pass shaders are Rae
 * assets declared with `shader(files:)` and validated at build:
 * lib/deferred_light.wgsl (+ lib/gbuffer_octahedral.wgsl, lib/sky_hosek.wgsl),
 * lib/deferred_ao.wgsl, lib/deferred_taa.wgsl, lib/deferred_composite.wgsl,
 * lib/deferred_pyramid_from_depth.wgsl, lib/deferred_pyramid_reduce.wgsl.
 * C keeps the deferred prepare (shadow defaults) and the shadow inputs.
 *
 * The lighting BRDF is deliberately the same one the forward pass uses.
 * Anywhere the two disagree is a bug in one of them. */

/* #923/#925: the pass objects and the shadow cascades are Rae's. What is
 * left of the deferred prep is the device + surface check. */
int64_t rae_gb_deferred_prepare(void) {
    if (!g_wgpu_dev) return 0;
    return g_g2d_surface ? 1 : 0;
}
