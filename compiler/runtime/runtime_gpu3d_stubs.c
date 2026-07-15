/* gpu3d disabled-capability stubs — builds with WebGPU but without SDL3.
 * Mirrors runtime_gpu2d_stubs.c. Included by rae_runtime.c into one TU. */

int64_t rae_ext_gpu3d_meshCreate(const double* verts, int64_t vertCount,
                                 const int64_t* indices, int64_t indexCount) {
    (void)verts; (void)vertCount; (void)indices; (void)indexCount; return 0;
}
void rae_ext_gpu3d_begin(const double* frame, int64_t count) { (void)frame; (void)count; }
void rae_ext_gpu3d_draw(int64_t mesh, const double* model,
                        double r, double g, double b,
                        double metallic, double roughness,
                        double emR, double emG, double emB) {
    (void)mesh; (void)model; (void)r; (void)g; (void)b;
    (void)metallic; (void)roughness; (void)emR; (void)emG; (void)emB;
}
void rae_ext_gpu3d_end(void) {}
void rae_ext_gpu3d_shutdown(void) {}
