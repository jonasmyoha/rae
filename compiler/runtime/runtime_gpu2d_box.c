/* gpu2d box/shape pipeline and draw queue. Raw pipeline calls stay C; render batching policy is a future Rae migration candidate.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* --- Box uber-shader pipeline (#110) ----------------------------------
 * Since #907/#913 the whole box pass — shader (lib/gpu2d_box.wgsl), pipeline,
 * batch, instance buffers, draw — is Rae (lib/Gpu2dCanvas.rae). */

/* #915: the shared viewport uniform is a canvas buffer too (written from
 * rae_g2d_xform per flush); nothing of the box pass remains in C. */
