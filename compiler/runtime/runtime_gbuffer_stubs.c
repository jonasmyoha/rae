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

int64_t rae_gb_prepare(void){ return 0; }
/* #906: the frame uniform data (36 floats) — never written without a device. */
int64_t rae_gb_frame_data(rae_Mat4* viewProj, float clearR, float clearG, float clearB,
                          int64_t w, int64_t h, void* out){
    (void)viewProj; (void)clearR; (void)clearG; (void)clearB; (void)w; (void)h; (void)out; return 0;
}
int64_t rae_gb_offscreen_w(void)        { return 0; }
int64_t rae_gb_offscreen_h(void)        { return 0; }
void rae_gb_commit_targets(int64_t w, int64_t h, void* a, void* b, void* c, void* depth) {
    (void)w; (void)h; (void)a; (void)b; (void)c; (void)depth;
}
void rae_gb_forget_targets(void)        {}
int64_t rae_gb_targets_gen(void)        { return 0; }
const char* rae_gb_wgsl(void)      { return ""; }
const char* rae_gb_skin_wgsl(void) { return ""; }
void* rae_gb_view_a(void)     { return (void*)0; }
void* rae_gb_view_b(void)     { return (void*)0; }
void* rae_gb_view_c(void)     { return (void*)0; }
void* rae_gb_view_depth(void) { return (void*)0; }
float rae_gb_motion_zero(void){ return 128.0f / 255.0f; }
/* The static AND skinned single draws, plus the instanced draw, now run in Rae
 * (lib/gbuffer.rae: draw / drawSkinned / drawRecords, #502/#503); they call
 * these context accessors, so builds without the real geometry pass need no-op
 * versions. mesh_ready / skin_ready return 0, so the Rae draws early-return and
 * issue no GPU work. */
void rae_gb_submit(void* cmd)           { (void)cmd; }
const char* rae_gb_sdf_wgsl(void) { return ""; }
const char* rae_gb_view_wgsl(void)  { return ""; }
int64_t rae_g2d_format(void)        { return 0; }
void rae_ext_Gbuffer_present(void* texture, int64_t width, int64_t height) { (void)texture; (void)width; (void)height; }
void rae_ext_Gbuffer_shutdown(void) {}

/* The passes downstream of the G-buffer are pure GPU work — there is no
 * CPU-side path in them to exercise, unlike the per-object transform
 * build above — so these are no-ops with nothing to preserve. */
int64_t rae_gb_pyramid_ready(void)         { return 0; }
void* rae_gb_pyr_from_depth_pipeline(void) { return (void*)0; }
void* rae_gb_pyr_reduce_pipeline(void)     { return (void*)0; }
void* rae_gb_pyr_src_view(int64_t i)       { (void)i; return (void*)0; }
void* rae_gb_pyr_rt_view(int64_t i)        { (void)i; return (void*)0; }
void rae_gb_light_upload(float camX, float camY, float camZ, float exposure,
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
void* rae_gb_light_pipeline(void)      { return (void*)0; }
void* rae_gb_lit_view(void)            { return (void*)0; }
void* rae_gb_shadow_frame_ubuf(void)   { return (void*)0; }
int64_t rae_gb_shadow_frame_bytes(void){ return 320; }
void* rae_gb_shadow_array_view(void)   { return (void*)0; }
void* rae_gb_shadow_sampler(void)      { return (void*)0; }
void rae_gb_ssao_upload(float camX, float camY, float camZ){ (void)camX; (void)camY; (void)camZ; }
const char* rae_gb_ao_wgsl(void)   { return ""; }
void* rae_gb_ao_view(void)         { return (void*)0; }
void* rae_gb_light_ubuf(void)      { return (void*)0; }
int64_t rae_gb_light_bytes(void)   { return 0; }
int64_t rae_gb_taa_ready(void)       { return 0; }
int64_t rae_gb_taa_begin(void)       { return 0; }
void rae_gb_taa_end(void)            {}
void* rae_gb_taa_pipeline(void)      { return (void*)0; }
void* rae_gb_taa_ubuf(void)          { return (void*)0; }
void* rae_gb_taa_target_view(void)   { return (void*)0; }
void* rae_gb_taa_history_view(void)  { return (void*)0; }
int64_t rae_gb_taa_is_enabled(void)  { return 0; }
void rae_gb_set_taa_enabled(int64_t e) { (void)e; }
int64_t rae_gb_deferred_prepare(void)        { return 0; }
const char* rae_gb_composite_wgsl(void)      { return ""; }
void* rae_gb_composite_source_view(void)     { return (void*)0; }
int64_t rae_gb_composite_source_index(void)  { return 2; }
int64_t rae_ext_Gbuffer_pyramidMips(void) { return 0; }
void rae_ext_Gbuffer_deferredShutdown(void) {}

/* Shadows (#382). Stubbed for builds without the GPU backend. */
void rae_sm_begin(const float* cascades, int64_t count, int64_t resolution,
                               const float* splits, const float* texelWorld,
                               const float* depthRange){
    (void)cascades; (void)count; (void)resolution; (void)splits; (void)texelWorld;
    (void)depthRange;
}
void rae_sm_queue_mesh(int64_t mesh, void* vbuf, void* ibuf, int64_t icount, rae_Mat4* model){ (void)mesh; (void)vbuf; (void)ibuf; (void)icount; (void)model; }
void rae_sm_queue_skinned(int64_t mesh, void* vbuf, void* ibuf, int64_t icount, void* palette, rae_Mat4* model, int64_t paletteBase){
    (void)mesh; (void)vbuf; (void)ibuf; (void)icount; (void)palette; (void)model; (void)paletteBase;
}
void rae_sm_record_metaballs(const float* packedBalls, int64_t count, float smoothing){
    (void)packedBalls; (void)count; (void)smoothing;
}
/* draw count exposed via rae_sm_draw_count (#514). */
/* Shadow cascade render moved to Rae (#504); accessors no-op without a GPU. */
int64_t rae_sm_ready(void)               { return 0; }
void rae_sm_upload_models(void)          {}
int64_t rae_sm_cascade_count(void)       { return 0; }
int64_t rae_sm_draw_count(void)          { return 0; }
void* rae_sm_layer_view(int64_t c)       { (void)c; return (void*)0; }
int64_t rae_sm_caster_ready(int64_t i, int64_t c) { (void)i; (void)c; return 0; }
void* rae_sm_caster_pipeline(int64_t i)  { (void)i; return (void*)0; }
void* rae_sm_caster_bind(int64_t i, int64_t c) { (void)i; (void)c; return (void*)0; }
void* rae_sm_caster_vbuf(int64_t i)      { (void)i; return (void*)0; }
void* rae_sm_caster_ibuf(int64_t i)      { (void)i; return (void*)0; }
int64_t rae_sm_caster_icount(int64_t i)  { (void)i; return 0; }
int64_t rae_sm_caster_key(int64_t i)     { (void)i; return -1; }
void rae_sm_draw_metaballs(int64_t c, void* passptr) { (void)c; (void)passptr; }
void rae_sm_shutdown(void) {}

/* Skinning (#374). Stubbed for builds without the GPU backend. */
int rae_g3d_push_skinned_draw(int64_t mesh, rae_Mat4* model,
                              float r, float g, float b,
                              float metallic, float roughness, void* palette){
    (void)mesh; (void)model; (void)r; (void)g; (void)b; (void)metallic; (void)roughness; (void)palette;
    return -1;
}
void* rae_g3d_skin_pipeline(void) { return (void*)0; }
void* rae_g3d_skin_bind(void)     { return (void*)0; }
void rae_ext_Gpu3d_skinFrameBegin(void) {}
int64_t rae_ext_Gpu3d_skinDrawCount(void) { return 0; }
void rae_ext_Gpu3d_skinShutdown(void) {}

void rae_ext_Gbuffer_skyHosekPush(int64_t index, float value){ (void)index; (void)value; }
