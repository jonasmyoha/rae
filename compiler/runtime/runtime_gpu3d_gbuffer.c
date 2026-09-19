/* gbuffer — the deferred frame's geometry pass (#356, Track B).
 *
 * This is NOT the forward pass with lighting deleted. The forward pass
 * (runtime_gpu3d.c) shades inside the material shader, so its cost scales
 * with objects x lights and every lit pixel is shaded whether or not it
 * survives depth. This pass writes SURFACE ATTRIBUTES only; lighting later
 * reads them once per pixel, independent of how many objects contributed.
 * The two frames share meshes (an asset) and the platform present path (a
 * copy into the drawable) and nothing else — separate pipelines, separate
 * targets, separate uniforms. Mixing them would make the deferred frame's
 * cost model a fiction.
 *
 * WHAT THE G-BUFFER HOLDS, AND WHY IT IS PACKED THIS WAY (#366). Three
 * targets plus depth, 16 bytes per pixel. Channels are packed across
 * targets rather than grouped by meaning, because bandwidth is the whole
 * point and an unused channel is bandwidth spent on nothing:
 *
 *   A  rgb10a2unorm  oct(normal).xy at 10 bits each, .z reserved,
 *                    .w = material_mode (2 bits, 4 values).
 *   B  rgba8unorm    albedo.rgb, .a = roughness.
 *   C  rgba8unorm    motion.xy, .z = metallic, .w = occlusion OR emissive.
 *   D  depth32float  reverse-Z (#367).
 *
 * OCTAHEDRAL NORMALS are what make this layout fit. A unit vector has two
 * degrees of freedom, so storing three components wastes one; octahedral
 * mapping is an area-preserving projection onto two. The bit depth is not
 * incidental — 8-bit oct bands visibly across smooth surfaces, which is
 * why this needs rgb10a2 rather than rgba8. Freeing that third channel is
 * what leaves room for motion and the mode field.
 *
 * MATERIAL_MODE selects how the rest is interpreted: lit, emissive, or
 * unlit. Two bits is all rgb10a2's alpha has, and all this needs.
 *
 * EMISSIVE rides in C.w, the same channel as ambient occlusion, chosen by
 * the mode: a surface that emits is not one whose ambient light needs
 * occluding. It is stored LOGARITHMICALLY — decode is exp(e * 6.91) - 1 —
 * which fits roughly [0, 1000] of linear range into 8 bits. An earlier
 * note in this project claimed 8-bit unorm could not carry an HDR emitter
 * and proposed an extra pass; that was wrong, and this is the reason.
 *
 * WORLD POSITION is still absent, and still reconstructed from depth. It
 * is the one thing that would be pure redundancy to store.
 *
 * PER-OBJECT COST. One Mat4 by value and two vec4s of material, memcpy'd
 * into a preallocated CPU array. No allocation, per object or per frame —
 * the acceptance criterion for #356, pinned by test 573.
 */

#define GB_MAX_DRAWS   4096
#define GB_DRAW_FLOATS 40   /* mat4 model + mat4 prevModel + vec4 albedo/metallic + vec4 rough/emissive/mode */
#define GB_FRAME_BYTES 144  /* viewProj + prevViewProj + jitter (#390/#397) */

/* Debug view selectors, mirrored by lib/gbuffer.rae. A G-buffer inspector
 * is permanent equipment in a deferred renderer, not scaffolding: when the
 * lit image is wrong, the first question is always which attribute is
 * wrong, and that is unanswerable without looking at the channels. */
#define GB_VIEW_LIT      0
#define GB_VIEW_ALBEDO   1
#define GB_VIEW_NORMAL   2
#define GB_VIEW_MATERIAL 3
#define GB_VIEW_DEPTH    4

/* #906/#923: the G-buffer targets are Rae manager textures owned by the
 * DeferredRenderer's GbufferCache (lib/GbufferResources.rae) and every reader
 * binds them by ID; C keeps none of them. */
/* #912: the static / skin / terrain pipelines and bind groups are manager IDs
 * on the Rae side (lib/Gbuffer.rae, lib/GbufferTerrain.rae); #906: so are the
 * frame uniform, the draws buffer, the draw cursor and the frame's command
 * encoder / render pass (a manager Recording per frame). The geometry shaders
 * are lib/gbuffer_static.wgsl / lib/gbuffer_skinned.wgsl (+ the shared
 * lib/gbuffer_octahedral.wgsl), declared with `shader(files:)` in
 * lib/GbufferResources.rae and validated at build. C keeps the frame-derived
 * uniform MATH below and a "pass open" flag for the metaball prep, which
 * still runs here. */

/* #923: the frame-derived matrices (viewProj, the jittered one, the previous
 * frame's, the clear colour) live on the Rae GbufferCache; nothing in C
 * reconstructs positions any more. */
/* #904: the inspector pipeline / uniform / bind are manager IDs on the Rae
 * side (lib/GbufferInspector.rae); its shader is lib/gbuffer_inspect.wgsl. */

/* Shading models, in the 2 bits rgb10a2's alpha provides. Values are the
 * quantisation points of those 2 bits so a round-trip through the texture
 * lands exactly where it started. */
#define GB_MODE_LIT      0.0f
#define GB_MODE_EMISSIVE (1.0f / 3.0f)
#define GB_MODE_UNLIT    (2.0f / 3.0f)
/* TOON (#396) takes the last of the four values. Being PER-MATERIAL
 * rather than a whole-frame uniform is the point: the G-buffer already
 * stores surface attributes independently of how they are lit, so a
 * shading STYLE is a different read of the same buffer, and toon and PBR
 * objects can stand in one frame lit by one sun. A frame-wide switch
 * would have been one branch and no new bits, but could not mix — and
 * mixing is the case worth having. It costs nothing here because the 2
 * bits were already allocated and this value was spare. */
#define GB_MODE_TOON     1.0f

/* Emissive is stored as log(1+E)/K and decoded as exp(e*K)-1, which fits
 * roughly [0, 1000] of linear radiance into one 8-bit channel. */
#define GB_EMISSIVE_LOG_K 6.91f

/* ZERO MOTION, and why it is 128/255 rather than 0.5.
 *
 * Motion is signed and target C is rgba8unorm, so the encoding is biased:
 * store m * 0.5 + BIAS, decode raw * 2 - 2*BIAS. The bias must be a value
 * the 8-bit channel can represent EXACTLY, or "did not move" does not
 * survive the round trip. 128/255 quantises to integer 128 exactly;
 * 0.5 is 127.5, which lands half a step off whichever way it rounds, and
 * decodes to a small but nonzero velocity on every static pixel. A
 * temporal pass reading that reprojects each still pixel slightly off
 * itself and softens the image — a defect that looks like "TAA is blurry"
 * rather than like an encoding bug, which is what makes it worth getting
 * right before anything consumes the channel.
 *
 * The decode constant is paired: 2 * (128/255) = 256/255. Whoever adds the
 * temporal pass must use that pairing, not 1.0, or the exactness is lost
 * at the other end.
 *
 * A raw value of EXACTLY (0,0) is left free as a sentinel meaning "this
 * pixel opted out of temporal accumulation", which is distinguishable from
 * every encoded velocity precisely because zero motion is 128/255. */
#define GB_MOTION_ZERO (128.0f / 255.0f)

/* The G-buffer is sized to the offscreen target, and reallocated on
 * resize. These are the deferred frame's OWN textures: the graph declares
 * gAlbedo/gNormal/gMaterial/gDepth as transient resources of this frame,
 * and aliasing them onto the forward frame's attachments would make that
 * declaration a lie the first time both frames ran. */
/* Target creation now lives in Rae (lib/gbuffer.rae:ensureTargets, #503): it
 * builds each attachment's WGPUTextureDescriptor + view and stores the handles
 * back via rae_gb_set_target, using the C accessors for size / resize check /
 * release / commit above. */

/* The static + skinned geometry render pipelines are created in Rae
 * (lib/GbufferResources.rae: ensurePipelines) from the declared shaders. */
/* Grass generation compute shader (grass epic #486). Writes one DrawU record
 * per blade into a storage buffer, entirely on the GPU: a grid of blades around
 * the player, jittered by a lattice hash, dropped onto the terrain via the SAME
 * analytic height field as the CPU terrain mesh (perlin2 ported from lib/noise —
 * so blades sit exactly on the ground), swayed by the wind model matching
 * walker_grass's grassGust. No CPU per-blade work, no readback. The DrawU layout
 * mirrors lib/gbuffer_static.wgsl's (model, prevModel, albedoMetallic, params). */
/* The grass shaders are Rae assets since #874: lib/grass_compute.wgsl (composed in
 * Rae with the biome + noise chunks) and lib/grass_render.wgsl; the grass GPU objects
 * are typed IDs in the App-owned manager, so the C slot store is gone too. */

/* #906: the frame uniform + draws buffer are manager buffers on the Rae
 * GbufferCache (lib/GbufferResources.rae ensureGbBuffers); rae_gb_frame_ubuf /
 * draws_buffer / frame_bytes / draws_size and their setters are gone. */

/* The G-buffer inspector (pipeline + uniform + bind group + the fullscreen
 * pass) is built in Rae (lib/GbufferInspector.rae, #503) and, since #904, its
 * objects are manager IDs there. C exposes the
 * presentable target's format + view; Rae rebuilds the bind (which samples the
 * G-buffer views) when gb_targets_gen changes. */
int64_t rae_g2d_format(void)        { return (int64_t)g_g2d_fmt; }

/* Mirror of the Rae-side `Mat4` layout for the extern boundary; see the
 * long note in runtime_gpu3d.c. The include guard makes this a no-op when
 * that file was compiled into the same TU first. */
#ifndef RAE_GPU3D_MAT4_FFI
#define RAE_GPU3D_MAT4_FFI
typedef struct { float v[16]; } rae_Array_float_16;
typedef struct rae_Mat4 { rae_Array_float_16 m; } rae_Mat4;
_Static_assert(sizeof(rae_Mat4) == 16 * sizeof(float),
               "Rae Mat4 must stay 16 contiguous floats for the gpu3d extern boundary");
#endif

/* Begin the geometry pass. Takes the view-projection BY VALUE as a Mat4
 * (#354) rather than a packed Float list: the caller builds it from value
 * types on the stack, so starting a frame allocates nothing either. */
/* Per-frame PREP for the geometry pass (#503): lazily create the pipeline and
 * targets, reset the per-frame counters, and build+upload the frame uniform
 * (TAA jitter + prev-viewProj — stateful CPU math that stays C for now).
 * Returns 1 when the pass is ready to encode, 0 otherwise. The command encoder,
 * render pass and attachment descriptors are built in Rae (lib/gbuffer.rae:
 * begin) over the bindings once this returns 1. */
/* Ensure the pipelines exist. Returns 1 when a device + pipeline are ready.
 * Targets, buffers and bind groups are created in Rae (#503); the frame uniform
 * is uploaded by rae_gb_frame_uniform once Rae has created its buffer. */
int64_t rae_gb_prepare(void) {
    /* Pipelines are created in Rae now (ensurePipelines); just report the
     * device is up so begin() can proceed to build them. */
    return g_wgpu_dev ? 1 : 0;
}

/* G-buffer target creation moved to Rae (#503). C keeps the offscreen size
 * source, the resize check, the release, and the commit of the size + gen
 * counter; Rae builds the WGPUTextureDescriptor / view for each attachment and
 * stores the handles back here so every downstream pass reads them unchanged. */
/* Dynamic resolution (#530): the deferred targets (g-buffer, depth, pyramid,
 * lit, ao, taa) are sized to a FRACTION of the drawable, then the composite pass
 * upscales the reduced lit/taa source into the full-size presentable offscreen
 * (which must stay drawable-size for the same-size present copy). The scale is a
 * pure per-frame knob; changing it re-fits every target via the usual generation
 * check. Fewer shaded pixels = less GPU load, the main thermal lever on mobile. */
static double gb_render_scale = -1.0;
static double rae_gb_get_scale(void) {
    if (gb_render_scale < 0.0) {          /* lazy default; env for headless tests */
        gb_render_scale = 1.0;
        const char* e = getenv("RAE_RENDER_SCALE");
        if (e) { double v = atof(e); if (v >= 0.25 && v <= 1.0) gb_render_scale = v; }
    }
    return gb_render_scale;
}
void   rae_gb_set_render_scale(double s) {
    if (s < 0.25) s = 0.25; if (s > 1.0) s = 1.0; gb_render_scale = s;
}
double rae_gb_render_scale(void) { return rae_gb_get_scale(); }
static int rae_gb_scale_dim(int full) {
    int v = (int)((double)full * rae_gb_get_scale() + 0.5);
    return v < 1 ? 1 : v;
}
/* #920: the presentable target is the canvas's, built to the surface size. */
int64_t rae_gb_offscreen_w(void)  { return (int64_t)rae_gb_scale_dim(g_sdl_w); }
int64_t rae_gb_offscreen_h(void)  { return (int64_t)rae_gb_scale_dim(g_sdl_h); }
/* #923: the frame uniform data (jitter, previous view-projection) is Rae math
 * on the GbufferCache; rae_gb_frame_data is gone. */

/* #906/#922: the frame's encoder + pass are the Rae GbufferCache's; nothing
 * in C needs to know whether a geometry pass is open any more. */

/* Queue one mesh into the G-buffer. `model` arrives as a Mat4 by value —
 * 16 floats the caller already had on the stack — and is memcpy'd into a
 * preallocated slot. Nothing on this path touches the allocator. */
/* Queue a SKINNED mesh into the G-buffer (#391). Shares the draw record
 * and the storage buffer with the static path — the layouts are identical
 * — and switches pipeline for the duration of the draw, then hands the
 * pass back so a following static draw is unaffected. */
/* #921: the skinned meshes + joint palette are the Rae SkinStore's; the
 * rae_gb_skin_* accessors are gone. */

/* ---- Instanced draw: context accessors for the Rae port (#502) -------------
 *
 * The instanced G-buffer draw itself now lives in Rae (lib/gbuffer.rae:
 * drawRecords): Rae uploads its records straight into the draws storage buffer
 * via wgpuQueueWriteBuffer and issues one instanced wgpuRenderPassEncoder
 * DrawIndexed over the generated bindings — no C shim does the draw. These
 * accessors hand Rae the per-frame handles it needs (the pass encoder, the
 * static pipeline/bind group, the mesh's vertex/index buffers) and the shared
 * instance cursor, all as opaque Ptr / plain ints. The draws buffer is the
 * same shared per-frame instance array the single draw() path fills; Rae writes
 * its slice at [base .. base+count) and advances the cursor, exactly as the old
 * C drawRecords did. The static vertex shader indexes draws[instance_index], so
 * instanceCount=N / firstInstance=base gives each instance its own record. */
/* #906: rae_gb_pass is gone — the open pass is GbufferCache.pass in Rae. */
/* #533/#9/#14 textured terrain: the terrain-splat pipeline, its bind group, the
 * repeat sampler and the blend amount are manager objects / cache fields on the
 * Rae side since #912 (lib/GbufferTerrain.rae). C keeps the material ARRAY as an
 * asset-upload ABI (rae_gb_terrain_array_*) and exposes its 2d-array view, which
 * Rae adopts into the bind group; gen bumps whenever the array is (re)created so
 * Rae rebuilds the bind group. */
static WGPUTextureView gb_terrain_tex_view = NULL;
static int64_t gb_terrain_tex_gen = 0;
void* rae_gb_terrain_array_view(void)      { return (void*)gb_terrain_tex_view; }
void  rae_gb_bump_terrain_tex_gen(void)    { gb_terrain_tex_gen++; }
int64_t rae_gb_terrain_tex_gen(void)       { return gb_terrain_tex_gen; }

/* #14 textured-terrain material ARRAY. One texture_2d_array, one layer per
 * material (grass/sand/road/water); the terrain fragment blends each layer over
 * the procedural colour by its biome weight. Replaces #9's single texture_2d.
 * Create once (init), then write each layer's RGBA — same 64->32 repack as
 * registerImageRgba, since decodePng hands over one packed int per pixel. The
 * 2D-array VIEW is stored in the same slot the bind group reads, and the gen
 * bumps so Rae rebinds. */
static WGPUTexture gb_terrain_array_tex = NULL;
void rae_gb_terrain_array_init(int64_t w, int64_t h, int64_t layers) {
    WGPUDevice dev = (WGPUDevice)rae_wgpu_ctx_device();
    if (!dev || w <= 0 || h <= 0 || layers <= 0) return;
    if (gb_terrain_array_tex) { wgpuTextureRelease(gb_terrain_array_tex); gb_terrain_array_tex = NULL; }
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h;
    td.size.depthOrArrayLayers = (uint32_t)layers;
    td.format = WGPUTextureFormat_RGBA8Unorm; td.mipLevelCount = 1; td.sampleCount = 1;
    gb_terrain_array_tex = wgpuDeviceCreateTexture(dev, &td);
    WGPUTextureViewDescriptor vd; memset(&vd, 0, sizeof(vd));
    vd.format = WGPUTextureFormat_RGBA8Unorm;
    vd.dimension = WGPUTextureViewDimension_2DArray;
    vd.baseMipLevel = 0; vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0; vd.arrayLayerCount = (uint32_t)layers;
    vd.aspect = WGPUTextureAspect_All;
    if (gb_terrain_tex_view) { wgpuTextureViewRelease(gb_terrain_tex_view); gb_terrain_tex_view = NULL; }
    gb_terrain_tex_view = wgpuTextureCreateView(gb_terrain_array_tex, &vd);
    gb_terrain_tex_gen++;
}
void rae_gb_terrain_array_write(int64_t layer, const int64_t* pixels, int64_t w, int64_t h) {
    WGPUQueue q = (WGPUQueue)rae_wgpu_ctx_queue();
    if (!q || !gb_terrain_array_tex || !pixels || w <= 0 || h <= 0) return;
    size_t n = (size_t)w * (size_t)h;
    unsigned char* rgba = (unsigned char*)malloc(n * 4);
    if (!rgba) return;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = (uint32_t)pixels[i];
        rgba[i * 4 + 0] = (unsigned char)(p & 0xff);
        rgba[i * 4 + 1] = (unsigned char)((p >> 8) & 0xff);
        rgba[i * 4 + 2] = (unsigned char)((p >> 16) & 0xff);
        rgba[i * 4 + 3] = (unsigned char)((p >> 24) & 0xff);
    }
    WGPUTexelCopyTextureInfo dst; memset(&dst, 0, sizeof(dst));
    dst.texture = gb_terrain_array_tex; dst.aspect = WGPUTextureAspect_All;
    dst.origin.z = (uint32_t)layer;
    WGPUTexelCopyBufferLayout layout; memset(&layout, 0, sizeof(layout));
    layout.bytesPerRow = (uint32_t)w * 4; layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext; ext.width = (uint32_t)w; ext.height = (uint32_t)h; ext.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(q, &dst, rgba, n * 4, &layout, &ext);
    free(rgba);
    /* No gen bump: writing a layer updates the texture the existing view already
     * points at, so the bind group stays valid — only init (new view) rebinds. */
}

/* #15 BILLBOARD SPRITE pass. A textured, alpha-tested, cylindrical-quad pipeline
 * that draws standing props (trees, ...) from the same DrawU instance records the
 * mesh path uses, generalizing the grass render pass (procedural quad, no vertex
 * buffer). Handles live in one opaque slot array like grass; the sprite images
 * are a texture_2d_array (one layer per prop kind), created + written exactly
 * like the terrain array above. */
/* #874: the sprite pipeline/bind/uniform/sampler are manager IDs on the Rae side; C keeps
 * only the asset-upload ABI (the array texture + its view, read through rae_gb_sprite_array_view). */
static WGPUTextureView gb_sprite_array_view = NULL;
void* rae_gb_sprite_array_view(void) { return (void*)gb_sprite_array_view; }
static WGPUTexture gb_sprite_array_tex = NULL;
static int64_t gb_sprite_tex_gen = 0;
int64_t rae_gb_sprite_tex_gen(void) { return gb_sprite_tex_gen; }
void rae_gb_sprite_array_init(int64_t w, int64_t h, int64_t layers) {
    WGPUDevice dev = (WGPUDevice)rae_wgpu_ctx_device();
    if (!dev || w <= 0 || h <= 0 || layers <= 0) return;
    if (gb_sprite_array_tex) { wgpuTextureRelease(gb_sprite_array_tex); gb_sprite_array_tex = NULL; }
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h;
    td.size.depthOrArrayLayers = (uint32_t)layers;
    td.format = WGPUTextureFormat_RGBA8Unorm; td.mipLevelCount = 1; td.sampleCount = 1;
    gb_sprite_array_tex = wgpuDeviceCreateTexture(dev, &td);
    WGPUTextureViewDescriptor vd; memset(&vd, 0, sizeof(vd));
    vd.format = WGPUTextureFormat_RGBA8Unorm;
    vd.dimension = WGPUTextureViewDimension_2DArray;
    vd.baseMipLevel = 0; vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0; vd.arrayLayerCount = (uint32_t)layers;
    vd.aspect = WGPUTextureAspect_All;
    if (gb_sprite_array_view) { wgpuTextureViewRelease(gb_sprite_array_view); gb_sprite_array_view = NULL; }
    gb_sprite_array_view = wgpuTextureCreateView(gb_sprite_array_tex, &vd);
    gb_sprite_tex_gen++;
}
void rae_gb_sprite_array_write(int64_t layer, const int64_t* pixels, int64_t w, int64_t h) {
    WGPUQueue q = (WGPUQueue)rae_wgpu_ctx_queue();
    if (!q || !gb_sprite_array_tex || !pixels || w <= 0 || h <= 0) return;
    size_t n = (size_t)w * (size_t)h;
    unsigned char* rgba = (unsigned char*)malloc(n * 4);
    if (!rgba) return;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = (uint32_t)pixels[i];
        rgba[i * 4 + 0] = (unsigned char)(p & 0xff);
        rgba[i * 4 + 1] = (unsigned char)((p >> 8) & 0xff);
        rgba[i * 4 + 2] = (unsigned char)((p >> 16) & 0xff);
        rgba[i * 4 + 3] = (unsigned char)((p >> 24) & 0xff);
    }
    WGPUTexelCopyTextureInfo dst; memset(&dst, 0, sizeof(dst));
    dst.texture = gb_sprite_array_tex; dst.aspect = WGPUTextureAspect_All;
    dst.origin.z = (uint32_t)layer;
    WGPUTexelCopyBufferLayout layout; memset(&layout, 0, sizeof(layout));
    layout.bytesPerRow = (uint32_t)w * 4; layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext; ext.width = (uint32_t)w; ext.height = (uint32_t)h; ext.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(q, &dst, rgba, n * 4, &layout, &ext);
    free(rgba);
}

/* #905: rae_gb_mesh_* are gone — the Rae MeshStore owns the meshes.
 * #906: rae_gb_draws_buffer / max_draws / draw_count / advance_draws are gone —
 * the draw cursor is GbufferCache.drawCount in Rae (gbufferMaxDraws). */

/* Finish and submit the geometry pass. Uniform data uploads once here, not
 * per draw. */
/* The geometry pass's finish/submit (end()) now runs in Rae over the bindings
 * (lib/gbuffer.rae:endPass, #503). These accessors hand Rae the frame's command
 * encoder and let it clear the encoder/pass globals once the pass is submitted,
 * so the C metaball path and the draws see a live pass during the frame and a
 * cleared one after. */
/* Submit exactly one command buffer. wgpuQueueSubmit takes a POINTER to an
 * array of command buffers; Rae has no way yet to take the address of a single
 * handle (and List(Ptr) — Ptr being Buffer(void) — nests wrongly in the
 * container codegen), so this thin call stays C. Not renderer logic; a general
 * FFI gap to close later (array-of-handles across the boundary). */
void rae_gb_submit(void* cmd) {
    WGPUCommandBuffer c = (WGPUCommandBuffer)cmd;
    wgpuQueueSubmit(g_wgpu_queue, 1, &c);
}

/* Write one G-buffer channel into the presentable target. */
/* The debug-view pass moved to Rae (lib/gbuffer_inspector.rae debugView, #503). */

/* Present the composed frame. Shares the platform copy-to-drawable with
 * the forward frame — see rae_g3d_present_offscreen. Reached from Rae as
 * gbuffer.present() -> presentFrame() -> renderDeferredPass (no-UI present). */
void rae_ext_Gbuffer_present(void* texture, int64_t width, int64_t height) {
    rae_g2d_tick_virtual_clock();
    rae_g3d_present_offscreen((WGPUTexture)texture, (int)width, (int)height);
}

/* #906: C owns nothing of the geometry frame any more; this forgets the
 * borrowed target views and resets the frame-derived math so a renderer
 * created afterwards starts from a clean first frame. */
/* #923: C owns nothing of the deferred frame; this releases the asset-upload
 * arrays (terrain materials, sprites) the still-C upload ABI created. */
void rae_ext_Gbuffer_shutdown(void) {
    if (gb_terrain_tex_view) { wgpuTextureViewRelease(gb_terrain_tex_view); gb_terrain_tex_view = NULL; }
    if (gb_terrain_array_tex) { wgpuTextureRelease(gb_terrain_array_tex); gb_terrain_array_tex = NULL; }
    if (gb_sprite_array_view) { wgpuTextureViewRelease(gb_sprite_array_view); gb_sprite_array_view = NULL; }
    if (gb_sprite_array_tex) { wgpuTextureRelease(gb_sprite_array_tex); gb_sprite_array_tex = NULL; }
}
