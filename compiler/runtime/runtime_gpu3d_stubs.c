/* gpu3d disabled-capability stubs — builds with WebGPU but without SDL3.
 * Mirrors runtime_gpu2d_stubs.c. Included by rae_runtime.c into one TU. */

int64_t rae_ext_gpu3d_meshCreate(const float* verts, int64_t vertCount,
                                 const int64_t* indices, int64_t indexCount){
    (void)verts; (void)vertCount; (void)indices; (void)indexCount; return 0;
}
void rae_ext_gpu3d_meshUpdate(int64_t mesh, const float* verts, int64_t vertCount){
    (void)mesh; (void)verts; (void)vertCount;
}
int rae_g3d_frame_prepare(const float* frame, int64_t count){ (void)frame; (void)count; return 0; }
void* rae_g3d_hdr_view(void)      { return (void*)0; }
void* rae_g3d_normal_view(void)   { return (void*)0; }
void* rae_g3d_velocity_view(void) { return (void*)0; }
void* rae_g3d_ambient_view(void)  { return (void*)0; }
void* rae_g3d_depth_view(void)    { return (void*)0; }
void* rae_g3d_pipeline(void)      { return (void*)0; }
void* rae_g3d_bind(void)          { return (void*)0; }
void  rae_g3d_set_frame(void* enc, void* pass){ (void)enc; (void)pass; }
/* Mirror of the Rae-side `Mat4` layout, for the gpu3d extern boundary.
 *
 * `lib/math3d.rae` declares `type Mat4 { m: Array(Float, cap: 16) }`, and the
 * compiler emits exactly these two declarations into the generated
 * translation unit. Repeating them here is not duplication for its own sake:
 * C compatibility across translation units (C11 6.2.7) requires the same tag
 * and member names/types, so declaring the SAME shape makes the runtime's
 * definition of rae_ext_gpu3d_draw compatible with the prototype the
 * generated code calls through. A `const float*` parameter would have the
 * same ABI but an incompatible type, which is undefined behaviour rather
 * than merely untidy.
 *
 * These live in the runtime .c files, never in rae_runtime.h — generated code
 * includes that header and emits its own copy of these types, so putting them
 * there would be a redefinition.
 *
 * The static assert is the guard: if Mat4's size ever diverges from 16 floats
 * the build fails here instead of silently reading the wrong bytes.
 */
#ifndef RAE_GPU3D_MAT4_FFI
#define RAE_GPU3D_MAT4_FFI
typedef struct { float v[16]; } rae_Array_float_16;
typedef struct rae_Mat4 { rae_Array_float_16 m; } rae_Mat4;
_Static_assert(sizeof(rae_Mat4) == 16 * sizeof(float),
               "Rae Mat4 must stay 16 contiguous floats for the gpu3d extern boundary");
#endif

void rae_ext_gpu3d_draw(int64_t mesh, rae_Mat4* model, rae_Mat4* prevModel,
                        float r, float g, float b,
                        float metallic, float roughness,
                        float emR, float emG, float emB){
    (void)mesh; (void)model; (void)prevModel; (void)r; (void)g; (void)b;
    (void)metallic; (void)roughness; (void)emR; (void)emG; (void)emB;
}
void rae_ext_gpu3d_drawMetaballs(const float* packedBalls, int64_t count,
                                 const float* packedColors, float smoothing,
                                 float metallic, float roughness,
                                 float emR, float emG, float emB){
    (void)packedBalls; (void)count; (void)packedColors; (void)smoothing;
    (void)metallic; (void)roughness; (void)emR; (void)emG; (void)emB;
}
void rae_ext_gpu3d_end(void) {}
void rae_ext_gpu3d_submit(void) {}
void rae_ext_gpu3d_tonemap(void) {}
void rae_ext_gpu3d_taa(void) {}
void rae_ext_gpu3d_ssao(void) {}
void rae_ext_gpu3d_shutdown(void) {}
void rae_ext_gpu3d_skyHosekPush(int64_t index, float value){ (void)index; (void)value; }
void rae_ext_gpu3d_skyDraw(float skyKind, float turbidity, float skyExposure, float sunSizeRad,
                           float sunX, float sunY, float sunZ,
                           float sunR, float sunG, float sunB,
                           float zenR, float zenG, float zenB, float bands,
                           float horR, float horG, float horB, float discI,
                           float clearR, float clearG, float clearB){
    (void)skyKind; (void)turbidity; (void)skyExposure; (void)sunSizeRad;
    (void)sunX; (void)sunY; (void)sunZ; (void)sunR; (void)sunG; (void)sunB;
    (void)zenR; (void)zenG; (void)zenB; (void)bands;
    (void)horR; (void)horG; (void)horB; (void)discI;
    (void)clearR; (void)clearG; (void)clearB;
}
