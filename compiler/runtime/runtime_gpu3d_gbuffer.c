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
#define GB_FRAME_BYTES 128  /* viewProj + prevViewProj (#390) */

/* Debug view selectors, mirrored by lib/gbuffer.rae. A G-buffer inspector
 * is permanent equipment in a deferred renderer, not scaffolding: when the
 * lit image is wrong, the first question is always which attribute is
 * wrong, and that is unanswerable without looking at the channels. */
#define GB_VIEW_LIT      0
#define GB_VIEW_ALBEDO   1
#define GB_VIEW_NORMAL   2
#define GB_VIEW_MATERIAL 3
#define GB_VIEW_DEPTH    4

static WGPUTexture     gb_a_tex = NULL;   /* rgb10a2unorm oct-normal + mode */
static WGPUTextureView gb_a_view = NULL;
static WGPUTexture     gb_b_tex = NULL;   /* rgba8unorm   albedo + roughness */
static WGPUTextureView gb_b_view = NULL;
static WGPUTexture     gb_c_tex = NULL;   /* rgba8unorm   motion + metallic + ao/emissive */
static WGPUTextureView gb_c_view = NULL;
static WGPUTexture     gb_depth_tex = NULL;    /* depth32float, sampleable */
static WGPUTextureView gb_depth_view = NULL;
static int             gb_target_w = 0, gb_target_h = 0;
/* Bumped every time the G-buffer textures are recreated. Anything holding
 * a bind group that references those views (the inspector, the pyramid,
 * lighting) compares against this and rebuilds — a bind group outliving
 * its texture is a use-after-free the validation layer catches only
 * sometimes, and a stale one silently samples the pre-resize image. */
static int             gb_targets_gen = 0;

static WGPURenderPipeline gb_pipeline = NULL;
static WGPUBuffer         gb_frame_ubuf = NULL;
static WGPUBuffer         gb_draw_sbuf = NULL;
static WGPUBindGroup      gb_bind = NULL;
static float              gb_draw_cpu[GB_MAX_DRAWS * GB_DRAW_FLOATS];
static int                gb_draw_count = 0;
static bool               gb_overflow_reported = false;

static WGPUCommandEncoder    gb_enc = NULL;
static WGPURenderPassEncoder gb_pass = NULL;

/* This frame's view-projection and clear colour, kept for the LIGHTING
 * pass. Lighting reconstructs world position from depth, which needs the
 * inverse of exactly the matrix the geometry pass rendered with — deriving
 * it from a camera the app might have moved since would put the lighting a
 * frame out of step with the depth it is reading. */
static float gb_viewproj[16];
/* Last frame's view-projection, for motion vectors (#390). First frame
 * reuses this frame's, which yields zero motion — correct, and better
 * than an uninitialised matrix projecting every pixel to the origin. */
static float gb_prev_viewproj[16];
static bool  gb_have_prev_vp = false;
static float gb_clear[3];

static WGPURenderPipeline gb_view_pipeline = NULL;
static WGPUBindGroup      gb_view_bind = NULL;
static WGPUBuffer         gb_view_ubuf = NULL;

/* Octahedral normal encoding. A unit vector has two degrees of freedom;
 * this is the area-preserving map onto two channels. The lower hemisphere
 * folds outward across the |x|+|y|=1 diamond, which is what octWrap does. */
#define GB_OCT_WGSL \
"fn octWrap(v: vec2<f32>) -> vec2<f32> {\n" \
"  let s = vec2<f32>(select(-1.0, 1.0, v.x >= 0.0), select(-1.0, 1.0, v.y >= 0.0));\n" \
"  return (vec2<f32>(1.0) - abs(v.yx)) * s;\n" \
"}\n" \
"fn octEncode(n: vec3<f32>) -> vec2<f32> {\n" \
"  var p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));\n" \
"  if (n.z < 0.0) { p = octWrap(p); }\n" \
"  return p * 0.5 + vec2<f32>(0.5);\n" \
"}\n" \
"fn octDecode(e: vec2<f32>) -> vec3<f32> {\n" \
"  let f = e * 2.0 - vec2<f32>(1.0);\n" \
"  var n = vec3<f32>(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));\n" \
"  let t = max(-n.z, 0.0);\n" \
"  n = vec3<f32>(n.x + select(t, -t, n.x >= 0.0),\n" \
"                n.y + select(t, -t, n.y >= 0.0), n.z);\n" \
"  return normalize(n);\n" \
"}\n"

/* Shading models, in the 2 bits rgb10a2's alpha provides. Values are the
 * quantisation points of those 2 bits so a round-trip through the texture
 * lands exactly where it started. */
#define GB_MODE_LIT      0.0f
#define GB_MODE_EMISSIVE (1.0f / 3.0f)
#define GB_MODE_UNLIT    (2.0f / 3.0f)

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
#define GB_MOTION_ZERO_WGSL "0.50196078"   /* 128.0/255.0 */

/* Geometry pass. The vertex stage is deliberately close to the forward
 * one — the same mesh layout feeds both — but the fragment stage does no
 * lighting at all: it resolves the material and writes it out. That is the
 * whole point of the split. */
static const char* GB_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  prevViewProj: mat4x4<f32>,\n"
"};\n"
"struct DrawU {\n"
"  model: mat4x4<f32>,\n"
"  prevModel: mat4x4<f32>,\n"
"  albedoMetallic: vec4<f32>,\n"
"  params: vec4<f32>,\n"          /* x = roughness, y = emissive (encoded), z = mode */
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> draws: array<DrawU>;\n"
GB_OCT_WGSL
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) nrm: vec3<f32>,\n"
"  @location(1) @interpolate(flat) inst: u32,\n"
"  @location(2) clipNow: vec4<f32>,\n"
"  @location(3) clipPrev: vec4<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32,\n"
"      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {\n"
"  let d = draws[ii];\n"
"  var o: VsOut;\n"
"  o.pos = F.viewProj * (d.model * vec4<f32>(p, 1.0));\n"
/* Uniform-scale normal transform, matching the forward pass's documented
 * constraint. Non-uniform scale needs transpose(inverse(model)); when that
 * arrives it must arrive in both pipelines at once or the two frames will
 * disagree about which way a surface faces. */
"  o.nrm = normalize((d.model * vec4<f32>(n, 0.0)).xyz);\n"
"  o.inst = ii;\n"
/* Motion (#390): where this vertex is now, and where the SAME vertex was
 * last frame — its previous model through the previous view-projection.
 * Both unjittered; if a jitter is added later it is a rasterisation
 * offset, not scene motion, and including it would make every static
 * pixel appear to move. */
"  o.clipNow = o.pos;\n"
"  o.clipPrev = F.prevViewProj * (d.prevModel * vec4<f32>(p, 1.0));\n"
"  return o;\n"
"}\n"
"struct FsOut {\n"
"  @location(0) gba: vec4<f32>,\n"
"  @location(1) gbb: vec4<f32>,\n"
"  @location(2) gbc: vec4<f32>,\n"
"};\n"
"@fragment\n"
"fn fs(in: VsOut) -> FsOut {\n"
"  let d = draws[in.inst];\n"
/* Renormalise: interpolation across a triangle shortens the normal, and a
 * G-buffer normal that is not unit length quietly biases every dot product
 * the lighting pass takes. Do it BEFORE encoding — octDecode normalises on
 * the way out, which would hide the error rather than prevent it. */
"  let n = normalize(in.nrm);\n"
"  let oct = octEncode(n);\n"
/* Roughness is clamped at write time, not read time, so every consumer
 * gets the same floor without having to remember it. A zero-roughness GGX
 * lobe is a division by zero at the highlight. */
"  let rough = clamp(d.params.x, 0.045, 1.0);\n"
/* Motion vectors, now REAL (#390). UV-space displacement since last
 * frame, biased into an unsigned 8-bit channel: 128/255 is zero, and it
 * is the one value rgba8unorm reproduces exactly, which is why the clear
 * colour uses it too. The decode is the paired
 * `raw * 2 - 256/255` — the two must change together. */
"  let now = in.clipNow.xy / in.clipNow.w;\n"
"  let prev = in.clipPrev.xy / in.clipPrev.w;\n"
"  let motion = (now - prev) * vec2<f32>(0.5, -0.5);\n"
/* Clamp before biasing: a fast object can move more than half a screen in
 * one frame, and wrapping would encode huge motion as tiny motion — the
 * worst possible failure for a temporal filter, since it looks valid. */
"  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(" GB_MOTION_ZERO_WGSL ");\n"
"  var o: FsOut;\n"
"  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);\n"
"  o.gbb = vec4<f32>(d.albedoMetallic.rgb, rough);\n"
"  o.gbc = vec4<f32>(mEnc.x, mEnc.y,\n"
"                     clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);\n"
"  return o;\n"
"}\n";

/* G-buffer inspector. Fullscreen triangle, textureLoad by pixel (1:1, so
 * no sampler), one channel selected by a uniform. */
static const char* GB_VIEW_WGSL =
"@group(0) @binding(0) var<uniform> P: vec4<f32>;\n"   /* x = mode, y = zNear, z = zFar */
"@group(0) @binding(1) var gbaTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var gbbTex: texture_2d<f32>;\n"
"@group(0) @binding(3) var gbcTex: texture_2d<f32>;\n"
"@group(0) @binding(4) var depthTex: texture_depth_2d;\n"
GB_OCT_WGSL
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n"
"  var points = array<vec2<f32>, 3>(\n"
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n"
"  return vec4<f32>(points[vi], 0.0, 1.0);\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let px = vec2<i32>(pos.xy);\n"
"  let mode = i32(P.x);\n"
"  var c = vec3<f32>(0.0);\n"
"  if (mode == 2) {\n"
/* Decode, then remap to [0,1] so the sign is visible rather than clipped
 * to black on every surface facing away from an axis. */
"    let n = octDecode(textureLoad(gbaTex, px, 0).xy);\n"
"    c = n * 0.5 + vec3<f32>(0.5);\n"
"  } else if (mode == 3) {\n"
/* Material view: metallic in red, roughness in green, the emissive/AO
 * channel in blue — the three scalars that decide how a pixel shades. */
"    let gbb = textureLoad(gbbTex, px, 0);\n"
"    let gbc = textureLoad(gbcTex, px, 0);\n"
"    c = vec3<f32>(gbc.z, gbb.a, gbc.w);\n"
"  } else if (mode == 4) {\n"
/* Reverse-Z (#367): the NEAR plane is 1 and far is 0, so a raw display is
 * inverted relative to intuition as well as bunched. Linearise, which also
 * puts near back at 0 where a reader expects it. */
"    let d = textureLoad(depthTex, px, 0);\n"
"    let zn = P.y; let zf = P.z;\n"
"    let lin = (zf * zn) / max(d * (zf - zn) + zn, 1e-6);\n"
"    c = vec3<f32>(clamp((lin - zn) / max(zf - zn, 1e-6), 0.0, 1.0));\n"
"  } else {\n"
"    c = textureLoad(gbbTex, px, 0).rgb;\n"
"  }\n"
/* The inspector writes the presentable (LDR, gamma) target, so encode.
 * Albedo and material are authored in [0,1] and displayed as authored. */
"  return vec4<f32>(pow(c, vec3<f32>(1.0 / 2.2)), 1.0);\n"
"}\n";

static void gb_release_targets(void) {
    if (gb_view_bind)    { wgpuBindGroupRelease(gb_view_bind); gb_view_bind = NULL; }
    if (gb_a_view) { wgpuTextureViewRelease(gb_a_view); gb_a_view = NULL; }
    if (gb_a_tex)  { wgpuTextureRelease(gb_a_tex); gb_a_tex = NULL; }
    if (gb_b_view) { wgpuTextureViewRelease(gb_b_view); gb_b_view = NULL; }
    if (gb_b_tex)  { wgpuTextureRelease(gb_b_tex); gb_b_tex = NULL; }
    if (gb_c_view) { wgpuTextureViewRelease(gb_c_view); gb_c_view = NULL; }
    if (gb_c_tex)  { wgpuTextureRelease(gb_c_tex); gb_c_tex = NULL; }
    if (gb_depth_view)   { wgpuTextureViewRelease(gb_depth_view); gb_depth_view = NULL; }
    if (gb_depth_tex)    { wgpuTextureRelease(gb_depth_tex); gb_depth_tex = NULL; }
}

/* The G-buffer is sized to the offscreen target, and reallocated on
 * resize. These are the deferred frame's OWN textures: the graph declares
 * gAlbedo/gNormal/gMaterial/gDepth as transient resources of this frame,
 * and aliasing them onto the forward frame's attachments would make that
 * declaration a lie the first time both frames ran. */
static void gb_ensure_targets(void) {
    int w = g_g2d_off_w, h = g_g2d_off_h;
    if (w <= 0 || h <= 0) return;
    if (gb_depth_view && w == gb_target_w && h == gb_target_h) return;
    gb_release_targets();

    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h; td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = 1; td.sampleCount = 1;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;

    td.format = WGPUTextureFormat_RGB10A2Unorm;
    gb_a_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    gb_a_view = wgpuTextureCreateView(gb_a_tex, NULL);
    td.format = WGPUTextureFormat_RGBA8Unorm;
    gb_b_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    gb_b_view = wgpuTextureCreateView(gb_b_tex, NULL);
    td.format = WGPUTextureFormat_RGBA8Unorm;
    gb_c_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    gb_c_view = wgpuTextureCreateView(gb_c_tex, NULL);
    td.format = WGPUTextureFormat_Depth32Float;
    gb_depth_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    gb_depth_view = wgpuTextureCreateView(gb_depth_tex, NULL);

    gb_target_w = w; gb_target_h = h;
    gb_targets_gen++;
}

static void gb_init_pipeline(void) {
    if (gb_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(GB_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    /* Same interleaved pos3/nrm3/uv2 vertex layout as the forward pass:
     * meshes are assets shared by both frames, not per-frame resources. */
    WGPUVertexAttribute attrs[3]; memset(attrs, 0, sizeof(attrs));
    attrs[0].format = WGPUVertexFormat_Float32x3; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Float32x3; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
    attrs[2].format = WGPUVertexFormat_Float32x2; attrs[2].offset = 24; attrs[2].shaderLocation = 2;
    WGPUVertexBufferLayout vbl; memset(&vbl, 0, sizeof(vbl));
    vbl.arrayStride = 32; vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.attributeCount = 3; vbl.attributes = attrs;

    WGPUColorTargetState cts[3]; memset(cts, 0, sizeof(cts));
    cts[0].format = WGPUTextureFormat_RGB10A2Unorm; cts[0].writeMask = WGPUColorWriteMask_All;
    cts[1].format = WGPUTextureFormat_RGBA8Unorm;   cts[1].writeMask = WGPUColorWriteMask_All;
    cts[2].format = WGPUTextureFormat_RGBA8Unorm;   cts[2].writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 3; fs.targets = cts;

    WGPUDepthStencilState ds; memset(&ds, 0, sizeof(ds));
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.depthCompare = WGPUCompareFunction_Greater;   /* reverse-Z (#367) */
    ds.stencilFront.compare = WGPUCompareFunction_Always;
    ds.stencilFront.failOp = WGPUStencilOperation_Keep;
    ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    ds.stencilFront.passOp = WGPUStencilOperation_Keep;
    ds.stencilBack = ds.stencilFront;
    ds.stencilReadMask = 0xFFFFFFFFu; ds.stencilWriteMask = 0xFFFFFFFFu;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;   /* auto layout from shader bindings */
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_Back;
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    gb_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!gb_pipeline) { fprintf(stderr, "[gbuffer] pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
    ud.size = GB_FRAME_BYTES;
    ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    gb_frame_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);

    WGPUBufferDescriptor sd; memset(&sd, 0, sizeof(sd));
    sd.size = (uint64_t)GB_MAX_DRAWS * GB_DRAW_FLOATS * sizeof(float);
    sd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    gb_draw_sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &sd);

    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_pipeline, 0);
    WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = gb_frame_ubuf; e[0].size = GB_FRAME_BYTES;
    e[1].binding = 1; e[1].buffer = gb_draw_sbuf;  e[1].size = sd.size;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
    gb_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

static void gb_init_view_pipeline(void) {
    if (gb_view_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(GB_VIEW_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;
    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    gb_view_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!gb_view_pipeline) { fprintf(stderr, "[gbuffer] inspector pipeline creation FAILED\n"); return; }

    WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
    ud.size = 16; ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    gb_view_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
}

/* The inspector's bind group references the G-buffer views, so it is
 * rebuilt whenever gb_ensure_targets recreates them (it nulls this). */
static void gb_ensure_view_bind(void) {
    if (gb_view_bind || !gb_view_pipeline) return;
    if (!gb_a_view || !gb_b_view || !gb_c_view || !gb_depth_view) return;
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_view_pipeline, 0);
    WGPUBindGroupEntry e[5]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = gb_view_ubuf; e[0].size = 16;
    e[1].binding = 1; e[1].textureView = gb_a_view;
    e[2].binding = 2; e[2].textureView = gb_b_view;
    e[3].binding = 3; e[3].textureView = gb_c_view;
    e[4].binding = 4; e[4].textureView = gb_depth_view;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 5; bgd.entries = e;
    gb_view_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

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
void rae_ext_gbuffer_begin(rae_Mat4* viewProj, float clearR, float clearG, float clearB) {
    if (!g_wgpu_dev || !viewProj) return;
    gb_init_pipeline();
    gb_ensure_targets();
    if (!gb_pipeline || !gb_a_view || !gb_b_view || !gb_c_view || !gb_depth_view) return;

    gb_draw_count = 0;
    memcpy(gb_viewproj, viewProj->m.v, 16 * sizeof(float));
    gb_clear[0] = clearR; gb_clear[1] = clearG; gb_clear[2] = clearB;
    /* First frame has no previous view-projection; reusing this one gives
     * zero motion, which is right — an uninitialised matrix would project
     * every pixel to the origin and read as the whole screen streaking. */
    if (!gb_have_prev_vp) {
        memcpy(gb_prev_viewproj, viewProj->m.v, 16 * sizeof(float));
        gb_have_prev_vp = true;
    }
    float fu[32];
    memcpy(fu, viewProj->m.v, 16 * sizeof(float));
    memcpy(fu + 16, gb_prev_viewproj, 16 * sizeof(float));
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_frame_ubuf, 0, fu, GB_FRAME_BYTES);
    /* Remember for NEXT frame, after this frame's copy is on the GPU. */
    memcpy(gb_prev_viewproj, viewProj->m.v, 16 * sizeof(float));

    gb_enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca[3]; memset(ca, 0, sizeof(ca));
    ca[0].view = gb_a_view;
    ca[0].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca[0].loadOp = WGPULoadOp_Clear;
    ca[0].storeOp = WGPUStoreOp_Store;
    /* (0.5, 0.5) is the octahedral encoding of +Z, so an unwritten pixel
     * decodes to a valid up-facing unit normal rather than to whatever
     * oct(0,0) happens to produce — which is a real direction too, just an
     * arbitrary one, and it showed up as a wrongly-coloured background in
     * the normal inspector. Mode 0 = lit. */
    ca[0].clearValue.r = 0.5; ca[0].clearValue.g = 0.5; ca[0].clearValue.b = 0.0; ca[0].clearValue.a = 0.0;
    /* A is the oct-normal/mode target now, so the clear colour belongs on
     * B (albedo) instead. Clearing albedo to the sky colour is not a
     * shortcut for a sky pass — it is what the inspector shows where no
     * geometry wrote, and it keeps a background-coloured frame
     * distinguishable from a black failure. */
    ca[1].view = gb_b_view;
    ca[1].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca[1].loadOp = WGPULoadOp_Clear;
    ca[1].storeOp = WGPUStoreOp_Store;
    ca[1].clearValue.r = clearR; ca[1].clearValue.g = clearG; ca[1].clearValue.b = clearB; ca[1].clearValue.a = 1.0;
    ca[2].view = gb_c_view;
    ca[2].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca[2].loadOp = WGPULoadOp_Clear;
    ca[2].storeOp = WGPUStoreOp_Store;
    /* Background: no motion (the exactly-representable biased zero), not
     * metallic, and unoccluded. Clearing this to zeros would tell a
     * temporal pass the background is moving fast and an ambient pass that
     * it is fully occluded. */
    ca[2].clearValue.r = GB_MOTION_ZERO; ca[2].clearValue.g = GB_MOTION_ZERO;
    ca[2].clearValue.b = 0.0; ca[2].clearValue.a = 1.0;
    WGPURenderPassDepthStencilAttachment da; memset(&da, 0, sizeof(da));
    da.view = gb_depth_view;
    da.depthLoadOp = WGPULoadOp_Clear;
    da.depthStoreOp = WGPUStoreOp_Store;   /* the pyramid and lighting both sample it */
    da.depthClearValue = 0.0f;   /* reverse-Z: 0 is the far plane (#367) */
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 3;
    rp.colorAttachments = ca;
    rp.depthStencilAttachment = &da;
    gb_pass = wgpuCommandEncoderBeginRenderPass(gb_enc, &rp);
    wgpuRenderPassEncoderSetPipeline(gb_pass, gb_pipeline);
    wgpuRenderPassEncoderSetBindGroup(gb_pass, 0, gb_bind, 0, NULL);
}

/* Queue one mesh into the G-buffer. `model` arrives as a Mat4 by value —
 * 16 floats the caller already had on the stack — and is memcpy'd into a
 * preallocated slot. Nothing on this path touches the allocator. */
void rae_ext_gbuffer_draw(int64_t mesh, rae_Mat4* model, rae_Mat4* prevModel,
                          float r, float g, float b,
                          float metallic, float roughness, float emissive) {
    if (!gb_pass || !model) return;
    int slot = (int)mesh - 1;
    if (slot < 0 || slot >= g3d_mesh_n) return;
    if (gb_draw_count >= GB_MAX_DRAWS) {
        if (!gb_overflow_reported) {
            fprintf(stderr, "[gbuffer] ERROR: draw limit exceeded (max=%d); discarding additional draws\n",
                    GB_MAX_DRAWS);
            gb_overflow_reported = true;
        }
        return;
    }
    float* d = gb_draw_cpu + gb_draw_count * GB_DRAW_FLOATS;
    memcpy(d, model->m.v, 16 * sizeof(float));
    /* No previous transform means "did not move": reusing the current
     * model yields zero motion, where zeros would project the object to
     * the origin and streak the whole screen toward it. */
    memcpy(d + 16, prevModel ? prevModel->m.v : model->m.v, 16 * sizeof(float));
    d[32] = r; d[33] = g; d[34] = b; d[35] = metallic;
    d[36] = roughness;
    /* Log-encode emissive into the 8-bit channel it shares with occlusion,
     * and flip the shading mode so lighting knows which it is. A
     * non-emitter stores 1.0 there, which reads as "unoccluded" — the
     * correct default until an AO pass writes something better. */
    if (emissive > 0.0f) {
        float e = logf(1.0f + emissive) / GB_EMISSIVE_LOG_K;
        d[37] = e > 1.0f ? 1.0f : e;
        d[38] = GB_MODE_EMISSIVE;
    } else {
        d[37] = 1.0f;
        d[38] = GB_MODE_LIT;
    }
    d[39] = 0.0f;
    wgpuRenderPassEncoderSetVertexBuffer(gb_pass, 0, g3d_mesh_vbuf[slot], 0,
                                         wgpuBufferGetSize(g3d_mesh_vbuf[slot]));
    wgpuRenderPassEncoderSetIndexBuffer(gb_pass, g3d_mesh_ibuf[slot],
                                        WGPUIndexFormat_Uint32, 0,
                                        wgpuBufferGetSize(g3d_mesh_ibuf[slot]));
    wgpuRenderPassEncoderDrawIndexed(gb_pass, g3d_mesh_icount[slot], 1, 0, 0,
                                     (uint32_t)gb_draw_count);
    gb_draw_count++;
}

/* Finish and submit the geometry pass. Uniform data uploads once here, not
 * per draw. */
void rae_ext_gbuffer_end(void) {
    if (!gb_pass) return;
    if (gb_draw_count > 0) {
        wgpuQueueWriteBuffer(g_wgpu_queue, gb_draw_sbuf, 0, gb_draw_cpu,
                             (size_t)gb_draw_count * GB_DRAW_FLOATS * sizeof(float));
    }
    if (getenv("RAE_GBUFFER_DEBUG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[gbuffer] geometry pass: draws=%d target=%dx%d\n",
                    gb_draw_count, gb_target_w, gb_target_h);
            logged = 1;
        }
    }
    wgpuRenderPassEncoderEnd(gb_pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(gb_enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(gb_pass); gb_pass = NULL;
    wgpuCommandEncoderRelease(gb_enc); gb_enc = NULL;
}

int64_t rae_ext_gbuffer_drawCount(void) { return (int64_t)gb_draw_count; }

/* Write one G-buffer channel into the presentable target. */
void rae_ext_gbuffer_debugView(int64_t mode, float zNear, float zFar) {
    if (!g_wgpu_dev || !g_g2d_off_view) return;
    gb_init_view_pipeline();
    gb_ensure_view_bind();
    if (!gb_view_pipeline || !gb_view_bind) return;

    float p[4] = { (float)mode, zNear, zFar, 0.0f };
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_view_ubuf, 0, p, sizeof(p));

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = g_g2d_off_view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, gb_view_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, gb_view_bind, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
}

/* Present the composed frame. Shares the platform copy-to-drawable with
 * the forward frame — see rae_g3d_present_offscreen. */
void rae_ext_gbuffer_present(void) {
    rae_g2d_tick_virtual_clock();
    rae_g3d_present_offscreen();
}

void rae_ext_gbuffer_shutdown(void) {
    gb_release_targets();
    if (gb_view_pipeline) { wgpuRenderPipelineRelease(gb_view_pipeline); gb_view_pipeline = NULL; }
    if (gb_view_ubuf)     { wgpuBufferRelease(gb_view_ubuf); gb_view_ubuf = NULL; }
    if (gb_bind)          { wgpuBindGroupRelease(gb_bind); gb_bind = NULL; }
    if (gb_draw_sbuf)     { wgpuBufferRelease(gb_draw_sbuf); gb_draw_sbuf = NULL; }
    if (gb_frame_ubuf)    { wgpuBufferRelease(gb_frame_ubuf); gb_frame_ubuf = NULL; }
    if (gb_pipeline)      { wgpuRenderPipelineRelease(gb_pipeline); gb_pipeline = NULL; }
    gb_target_w = 0; gb_target_h = 0;
}
