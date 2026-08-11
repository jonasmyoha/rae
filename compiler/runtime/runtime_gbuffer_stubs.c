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
void rae_ext_gbuffer_draw(int64_t mesh, rae_Mat4* model, rae_Mat4* prevModel,
                          float r, float g, float b,
                          float metallic, float roughness, float emissive,
                          int64_t toon){
    (void)mesh; (void)model; (void)prevModel; (void)r; (void)g; (void)b;
    (void)metallic; (void)roughness; (void)emissive; (void)toon;
}
void rae_ext_gbuffer_drawSkinned(int64_t mesh, rae_Mat4* model, rae_Mat4* prevModel,
                                 float r, float g, float b,
                                 float metallic, float roughness, float emissive,
                                 int64_t toon){
    (void)mesh; (void)model; (void)prevModel; (void)r; (void)g; (void)b;
    (void)metallic; (void)roughness; (void)emissive; (void)toon;
}
void rae_ext_gbuffer_drawMetaballs(const float* packedBalls, int64_t count,
                                   const float* packedColors, float smoothing,
                                   float camX, float camY, float camZ,
                                   float metallic, float roughness,
                                   float emR, float emG, float emB){
    (void)packedBalls; (void)count; (void)packedColors; (void)smoothing;
    (void)camX; (void)camY; (void)camZ; (void)metallic; (void)roughness;
    (void)emR; (void)emG; (void)emB;
}
void rae_ext_gbuffer_sdfShutdown(void) {}
void rae_ext_gbuffer_end(void) {}
int64_t rae_ext_gbuffer_drawCount(void) { return 0; }
void rae_ext_gbuffer_debugView(int64_t mode, float zNear, float zFar){
    (void)mode; (void)zNear; (void)zFar;
}
void rae_ext_gbuffer_present(void) {}
void rae_ext_gbuffer_shutdown(void) {}

/* The passes downstream of the G-buffer are pure GPU work — there is no
 * CPU-side path in them to exercise, unlike the per-object transform
 * build above — so these are no-ops with nothing to preserve. */
void rae_ext_gbuffer_depthPyramid(void) {}
void rae_ext_gbuffer_lighting(float camX, float camY, float camZ, float exposure,
                              float sunX, float sunY, float sunZ,
                              float sunR, float sunG, float sunB,
                              float skyR, float skyG, float skyB,
                              float gndR, float gndG, float gndB,
                              float skyKind, float turbidity, float skyExposure,
                              float sunSizeRad, float zenR, float zenG, float zenB,
                              float bands, float horR, float horG, float horB,
                              float discI){
    (void)camX; (void)camY; (void)camZ; (void)exposure;
    (void)sunX; (void)sunY; (void)sunZ;
    (void)sunR; (void)sunG; (void)sunB;
    (void)skyR; (void)skyG; (void)skyB;
    (void)gndR; (void)gndG; (void)gndB;
    (void)skyKind; (void)turbidity; (void)skyExposure; (void)sunSizeRad;
    (void)zenR; (void)zenG; (void)zenB; (void)bands;
    (void)horR; (void)horG; (void)horB; (void)discI;
}
void rae_ext_gbuffer_ssao(float camX, float camY, float camZ){ (void)camX; (void)camY; (void)camZ; }
void rae_ext_gbuffer_taa(void) {}
void rae_ext_gbuffer_composite(float exposure) { (void)exposure; }
int64_t rae_ext_gbuffer_pyramidMips(void) { return 0; }
void rae_ext_gbuffer_deferredShutdown(void) {}

/* Shadows (#382). Stubbed for builds without the GPU backend. */
void rae_ext_gpu3d_shadowBegin(const float* cascades, int64_t count, int64_t resolution,
                               const float* splits, const float* texelWorld,
                               const float* depthRange){
    (void)cascades; (void)count; (void)resolution; (void)splits; (void)texelWorld;
    (void)depthRange;
}
void rae_ext_gpu3d_shadowDraw(int64_t mesh, rae_Mat4* model){ (void)mesh; (void)model; }
void rae_ext_gpu3d_shadowDrawSkinned(int64_t mesh, rae_Mat4* model){ (void)mesh; (void)model; }
void rae_ext_gpu3d_shadowMetaballs(const float* packedBalls, int64_t count, float smoothing){
    (void)packedBalls; (void)count; (void)smoothing;
}
void rae_ext_gpu3d_shadowEnd(void) {}
int64_t rae_ext_gpu3d_shadowDrawCount(void) { return 0; }
void rae_ext_gpu3d_shadowShutdown(void) {}

/* Skinning (#374). Stubbed for builds without the GPU backend. */
int64_t rae_ext_gpu3d_skinnedMeshCreate(const float* verts, int64_t vertCount,
                                        const int64_t* indices, int64_t indexCount){
    (void)verts; (void)vertCount; (void)indices; (void)indexCount; return 0;
}
void rae_ext_gpu3d_setPalette(const float* rows, int64_t jointCount){
    (void)rows; (void)jointCount;
}
void rae_ext_gpu3d_drawSkinned(int64_t mesh, rae_Mat4* model,
                               float r, float g, float b,
                               float metallic, float roughness){
    (void)mesh; (void)model; (void)r; (void)g; (void)b; (void)metallic; (void)roughness;
}
void rae_ext_gpu3d_skinFrameBegin(void) {}
int64_t rae_ext_gpu3d_skinDrawCount(void) { return 0; }
void rae_ext_gpu3d_skinShutdown(void) {}
