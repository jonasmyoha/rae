/* Deferred G-buffer stubs (#356) — no-ops for builds without the GPU
 * backend (no WebGPU, or WebGPU without SDL3).
 *
 * These live in their OWN file because they are needed under a strictly
 * wider set of builds. The gpu2d stubs only stand in when WebGPU is present
 * but SDL3 is not; a
 * build with no WebGPU at all has no windowing story and therefore no
 * caller. The deferred geometry pass is different: its per-object work is
 * a real CPU path — building a model matrix from a Transform3d through
 * Mat4 value types — and that path is worth running, and asserting on,
 * with no GPU anywhere. Test 573 does exactly that: it pins #356's
 * allocation-free acceptance criterion in the plain test build, where the
 * externs below are the whole backend.
 */

int64_t rae_gb_prepare(void){ return 0; }
void rae_gb_submit(void* cmd)           { (void)cmd; }
int64_t rae_g2d_format(void)        { return 0; }
void rae_ext_Gbuffer_present(void* texture, int64_t width, int64_t height) { (void)texture; (void)width; (void)height; }
void rae_ext_Gbuffer_shutdown(void) {}

/* The post passes are Rae manager objects (#923); C keeps the WGSL sources
 * and the deferred prepare. */
int64_t rae_gb_deferred_prepare(void)        { return 0; }


