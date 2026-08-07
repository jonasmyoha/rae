/* Deferred G-buffer stubs (#356) — no-ops for builds without the GPU
 * backend (no WebGPU, or WebGPU without SDL3).
 *
 * These live in their OWN file, rather than inside runtime_gpu3d_stubs.c,
 * because they are needed under a strictly wider set of builds. The gpu2d
 * and gpu3d stubs only stand in when WebGPU is present but SDL3 is not; a
 * build with no WebGPU at all has no windowing story and therefore no
 * caller. The deferred geometry pass is different: its per-object work is
 * a real CPU path — building a model matrix from a Transform3d through
 * Mat4 value types — and that path is worth running, and asserting on,
 * with no GPU anywhere. Test 573 does exactly that: it pins #356's
 * allocation-free acceptance criterion in the plain test build, where the
 * externs below are the whole backend.
 */

/* Mirror of the Rae-side `Mat4` layout for the extern boundary; see the
 * long note in runtime_gpu3d.c. Guarded because a full GPU build compiles
 * the real definition into the same translation unit. */
#ifndef RAE_GPU3D_MAT4_FFI
#define RAE_GPU3D_MAT4_FFI
typedef struct { float v[16]; } rae_Array_float_16;
typedef struct rae_Mat4 { rae_Array_float_16 m; } rae_Mat4;
_Static_assert(sizeof(rae_Mat4) == 16 * sizeof(float),
               "Rae Mat4 must stay 16 contiguous floats for the gpu3d extern boundary");
#endif

void rae_ext_gbuffer_begin(rae_Mat4* viewProj, float clearR, float clearG, float clearB){
    (void)viewProj; (void)clearR; (void)clearG; (void)clearB;
}
void rae_ext_gbuffer_draw(int64_t mesh, rae_Mat4* model,
                          float r, float g, float b,
                          float metallic, float roughness){
    (void)mesh; (void)model; (void)r; (void)g; (void)b; (void)metallic; (void)roughness;
}
void rae_ext_gbuffer_end(void) {}
int64_t rae_ext_gbuffer_drawCount(void) { return 0; }
void rae_ext_gbuffer_debugView(int64_t mode, float zNear, float zFar){
    (void)mode; (void)zNear; (void)zFar;
}
void rae_ext_gbuffer_present(void) {}
void rae_ext_gbuffer_shutdown(void) {}
