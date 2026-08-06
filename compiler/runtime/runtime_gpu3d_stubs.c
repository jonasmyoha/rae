/* gpu3d disabled-capability stubs — builds with WebGPU but without SDL3.
 * Mirrors runtime_gpu2d_stubs.c. Included by rae_runtime.c into one TU. */

int64_t rae_ext_gpu3d_meshCreate(const float* verts, int64_t vertCount,
                                 const int64_t* indices, int64_t indexCount){
    (void)verts; (void)vertCount; (void)indices; (void)indexCount; return 0;
}
void rae_ext_gpu3d_begin(const float* frame, int64_t count){ (void)frame; (void)count; }
void rae_ext_gpu3d_draw(int64_t mesh, const float* model, const float* prevModel,
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
