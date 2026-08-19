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
static int                gb_draw_count = 0;
/* Metaball cluster slots are PER FRAME (#392). Declared here because the
 * reset belongs beside every other per-frame counter, while the buffers
 * live in runtime_gpu3d_gbuffer_sdf.c, which is included after this. */
static int                gb_sdf_group;

static WGPUCommandEncoder    gb_enc = NULL;
static WGPURenderPassEncoder gb_pass = NULL;

/* This frame's view-projection and clear colour, kept for the LIGHTING
 * pass. Lighting reconstructs world position from depth, which needs the
 * inverse of exactly the matrix the geometry pass rendered with — deriving
 * it from a camera the app might have moved since would put the lighting a
 * frame out of step with the depth it is reading. */
static float gb_viewproj[16];
/* The matrix the geometry was actually RASTERISED with, jitter included.
 * Reconstruction from depth must invert this one, not the clean matrix:
 * the depth at a pixel came from the jittered ray, and inverting the
 * unjittered projection would place every reconstructed world position
 * off by up to half a pixel — small, but it is the input to AO and to
 * lighting. */
static float gb_viewproj_jittered[16];
static int   gb_jitter_frame = 0;
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
/* TOON (#396) takes the last of the four values. Being PER-MATERIAL
 * rather than a whole-frame uniform is the point: the G-buffer already
 * stores surface attributes independently of how they are lit, so a
 * shading STYLE is a different read of the same buffer, and toon and PBR
 * objects can stand in one frame lit by one sun. A frame-wide switch
 * would have been one branch and no new bits, but could not mix — and
 * mixing is the case worth having. It costs nothing here because the 2
 * bits were already allocated and this value was spare. */
#define GB_MODE_TOON     1.0f
/* WGSL-literal forms of the same constants, so the deferred SDF pass
 * cannot drift from the raster pass's encoding. */
#define GB_MODE_LIT_WGSL       "0.0"
#define GB_MODE_EMISSIVE_WGSL  "0.33333333"
#define GB_EMISSIVE_LOG_K_WGSL "6.91"

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
"  jitter: vec4<f32>,\n"   /* xy = this frame's sub-pixel clip offset (#397) */
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
/* Jitter LAST, after clipNow is captured (#397). It is a rasterisation
 * offset, not scene motion: including it in the motion vector would make
 * every static pixel appear to move by the jitter delta, which is the
 * one thing guaranteed to defeat the filter it exists to feed. */
"  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);\n"
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


/* ----- skinned geometry into the G-buffer (#391) ---------------------
 *
 * The deferred counterpart of runtime_gpu3d_skin.c's forward pipeline.
 * Same 20-float vertex format, same joint palette, same linear blend —
 * only the fragment output differs, writing packed surface attributes
 * instead of lit colour. That is the whole point of deferred: a second
 * GEOMETRY kind needs a second geometry pipeline, not a second lighting
 * path.
 *
 * The palette is SHARED with the forward and shadow pipelines rather than
 * duplicated. One buffer, one upload, three readers: two decoders of one
 * format is how a pose ends up subtly sheared in exactly one pass.
 */
static WGPURenderPipeline gb_skin_pipeline = NULL;
static WGPUBindGroup      gb_skin_bind = NULL;

static const char* GB_SKIN_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  prevViewProj: mat4x4<f32>,\n"
"  jitter: vec4<f32>,\n"
"};\n"
"struct DrawU {\n"
"  model: mat4x4<f32>,\n"
"  prevModel: mat4x4<f32>,\n"
"  albedoMetallic: vec4<f32>,\n"
"  params: vec4<f32>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> draws: array<DrawU>;\n"
"@group(0) @binding(2) var<storage, read> palette: array<vec4<f32>>;\n"
GB_OCT_WGSL
"fn jointMat(j: u32) -> mat4x4<f32> {\n"
"  let r0 = palette[j * 3u + 0u];\n"
"  let r1 = palette[j * 3u + 1u];\n"
"  let r2 = palette[j * 3u + 2u];\n"
"  return mat4x4<f32>(\n"
"    vec4<f32>(r0.x, r1.x, r2.x, 0.0),\n"
"    vec4<f32>(r0.y, r1.y, r2.y, 0.0),\n"
"    vec4<f32>(r0.z, r1.z, r2.z, 0.0),\n"
"    vec4<f32>(r0.w, r1.w, r2.w, 1.0));\n"
"}\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) nrm: vec3<f32>,\n"
"  @location(1) @interpolate(flat) inst: u32,\n"
"  @location(2) clipNow: vec4<f32>,\n"
"  @location(3) clipPrev: vec4<f32>,\n"
"  @location(4) vcol: vec3<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32,\n"
"      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>,\n"
"      @location(3) jf: vec4<f32>, @location(4) w: vec4<f32>,\n"
"      @location(5) vc: vec4<f32>) -> VsOut {\n"
"  let d = draws[ii];\n"
"  let j = vec4<u32>(u32(jf.x), u32(jf.y), u32(jf.z), u32(jf.w));\n"
"  var skin = jointMat(j.x) * w.x;\n"
"  skin = skin + jointMat(j.y) * w.y;\n"
"  skin = skin + jointMat(j.z) * w.z;\n"
"  skin = skin + jointMat(j.w) * w.w;\n"
"  let sp = skin * vec4<f32>(p, 1.0);\n"
"  var o: VsOut;\n"
"  o.pos = F.viewProj * (d.model * vec4<f32>(sp.xyz, 1.0));\n"
"  let sn = skin * vec4<f32>(n, 0.0);\n"
"  o.nrm = normalize((d.model * vec4<f32>(sn.xyz, 0.0)).xyz);\n"
"  o.inst = ii;\n"
"  o.vcol = vc.rgb;\n"
"  o.clipNow = o.pos;\n"
/* NO PREVIOUS PALETTE EXISTS, so a skinned vertex reports only its OBJECT
 * motion, not its limb motion. Velocity is therefore right for a
 * character sliding across the screen and wrong for a swinging arm: TAA
 * will smear the latter until a previous-frame palette is kept. The
 * forward path has the identical limitation and the identical note —
 * stated in both rather than discovered in an image. */
"  o.clipPrev = F.prevViewProj * (d.prevModel * vec4<f32>(sp.xyz, 1.0));\n"
"  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);\n"
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
"  let n = normalize(in.nrm);\n"
"  let oct = octEncode(n);\n"
"  let rough = clamp(d.params.x, 0.045, 1.0);\n"
"  let now = in.clipNow.xy / in.clipNow.w;\n"
"  let prev = in.clipPrev.xy / in.clipPrev.w;\n"
"  let motion = (now - prev) * vec2<f32>(0.5, -0.5);\n"
"  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(" GB_MOTION_ZERO_WGSL ");\n"
"  var o: FsOut;\n"
"  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);\n"
/* Vertex colour MULTIPLIES the material's base colour, exactly as in the
 * forward skinned shader (#378) — white is a no-op, so a model with no
 * baked colour is unaffected. */
"  o.gbb = vec4<f32>(d.albedoMetallic.rgb * in.vcol, rough);\n"
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
/* Target creation now lives in Rae (lib/gbuffer.rae:ensureTargets, #503): it
 * builds each attachment's WGPUTextureDescriptor + view and stores the handles
 * back via rae_gb_set_target, using the C accessors for size / resize check /
 * release / commit above. */

/* The static + skinned geometry render pipelines and their WGSL shader modules
 * are now created in Rae (lib/gbuffer.rae: ensurePipelines) over the bindings.
 * C exposes the WGSL source and the entry-point names as string accessors (the
 * shaders stay as source, per #503), plus setters that store the created
 * pipelines back into the globals so the draws, the render pass and the bind
 * groups read them unchanged. */
/* Grass generation compute shader (grass epic #486). Writes one DrawU record
 * per blade into a storage buffer, entirely on the GPU: a grid of blades around
 * the player, jittered by a lattice hash, dropped onto the terrain via the SAME
 * analytic height field as the CPU terrain mesh (perlin2 ported from lib/noise —
 * so blades sit exactly on the ground), swayed by the wind model matching
 * walker_grass's grassGust. No CPU per-blade work, no readback. The DrawU layout
 * mirrors GB_WGSL's (model, prevModel, albedoMetallic, params). */
static const char* GB_GRASS_WGSL =
"struct DrawU {\n"
"  model: mat4x4<f32>,\n"
"  prevModel: mat4x4<f32>,\n"
"  albedoMetallic: vec4<f32>,\n"
"  params: vec4<f32>,\n"
"};\n"
"struct GrassU {\n"
"  a: vec4<f32>,\n"   /* playerX, playerY, groundZ, time */
"  b: vec4<f32>,\n"   /* count, extent, toon, dt */
"  c: vec4<f32>,\n"   /* camX, camY, camZ, nearKeep */
"  d: vec4<f32>,\n"   /* forwardX, forwardY, forwardZ, coneCos */
"};\n"
"struct Indirect {\n"
"  vertexCount: u32,\n"
"  instanceCount: atomic<u32>,\n"
"  firstVertex: u32,\n"
"  firstInstance: u32,\n"
"};\n"
"@group(0) @binding(0) var<uniform> G: GrassU;\n"
"@group(0) @binding(1) var<storage, read_write> outBuf: array<DrawU>;\n"
"@group(0) @binding(2) var<storage, read_write> indirect: Indirect;\n"
"fn hashWord(v0: u32) -> u32 {\n"
"  var h = v0;\n"
"  h = h ^ (h >> 16u);\n"
"  h = h * 0x7feb352du;\n"
"  h = h ^ (h >> 15u);\n"
"  h = h * 0x846ca68bu;\n"
"  h = h ^ (h >> 16u);\n"
"  return h;\n"
"}\n"
"fn hashLattice2(p: vec2<i32>, seed: u32) -> u32 {\n"
"  var h = seed ^ 0x9e3779b9u;\n"
"  h = hashWord(h ^ (u32(p.x) * 0x85ebca6bu));\n"
"  h = hashWord(h ^ (u32(p.y) * 0xc2b2ae35u));\n"
"  return h;\n"
"}\n"
"fn hash2u(p: vec2<i32>, seed: u32) -> f32 { return f32(hashLattice2(p, seed)) / 4294967295.0; }\n"
"fn fadeN(t: f32) -> f32 { return t*t*t*(t*(t*6.0-15.0)+10.0); }\n"
"fn grad2(hash: u32, p: vec2<f32>) -> f32 {\n"
"  let h = hash & 7u;\n"
"  if (h == 0u) { return p.x; }\n"
"  if (h == 1u) { return -p.x; }\n"
"  if (h == 2u) { return p.y; }\n"
"  if (h == 3u) { return -p.y; }\n"
"  if (h == 4u) { return (p.x + p.y) * 0.70710678; }\n"
"  if (h == 5u) { return (-p.x + p.y) * 0.70710678; }\n"
"  if (h == 6u) { return (p.x - p.y) * 0.70710678; }\n"
"  return (-p.x - p.y) * 0.70710678;\n"
"}\n"
"fn perlin2(p: vec2<f32>, seed: u32) -> f32 {\n"
"  let cell = vec2<i32>(floor(p));\n"
"  let f = fract(p);\n"
"  let u = vec2<f32>(fadeN(f.x), fadeN(f.y));\n"
"  let a = mix(grad2(hashLattice2(cell, seed), f), grad2(hashLattice2(cell + vec2<i32>(1,0), seed), f - vec2<f32>(1.0,0.0)), u.x);\n"
"  let b = mix(grad2(hashLattice2(cell + vec2<i32>(0,1), seed), f - vec2<f32>(0.0,1.0)), grad2(hashLattice2(cell + vec2<i32>(1,1), seed), f - vec2<f32>(1.0,1.0)), u.x);\n"
"  return mix(a, b, u.y) * 1.41421356;\n"
"}\n"
"fn terrainHeight(x: f32, y: f32, groundZ: f32) -> f32 {\n"
"  return groundZ + perlin2(vec2<f32>(x*0.020, y*0.020), 977u) * 2.8 + perlin2(vec2<f32>(x*0.075, y*0.075), 431u) * 0.55;\n"
"}\n"
"fn mTranslate(t: vec3<f32>) -> mat4x4<f32> {\n"
"  return mat4x4<f32>(vec4<f32>(1.0,0.0,0.0,0.0), vec4<f32>(0.0,1.0,0.0,0.0), vec4<f32>(0.0,0.0,1.0,0.0), vec4<f32>(t.x,t.y,t.z,1.0));\n"
"}\n"
"fn mScale3(s: vec3<f32>) -> mat4x4<f32> {\n"
"  return mat4x4<f32>(vec4<f32>(s.x,0.0,0.0,0.0), vec4<f32>(0.0,s.y,0.0,0.0), vec4<f32>(0.0,0.0,s.z,0.0), vec4<f32>(0.0,0.0,0.0,1.0));\n"
"}\n"
"fn mRotX(a: f32) -> mat4x4<f32> {\n"
"  let c = cos(a); let s = sin(a);\n"
"  return mat4x4<f32>(vec4<f32>(1.0,0.0,0.0,0.0), vec4<f32>(0.0,c,s,0.0), vec4<f32>(0.0,-s,c,0.0), vec4<f32>(0.0,0.0,0.0,1.0));\n"
"}\n"
"fn mRotY(a: f32) -> mat4x4<f32> {\n"
"  let c = cos(a); let s = sin(a);\n"
"  return mat4x4<f32>(vec4<f32>(c,0.0,-s,0.0), vec4<f32>(0.0,1.0,0.0,0.0), vec4<f32>(s,0.0,c,0.0), vec4<f32>(0.0,0.0,0.0,1.0));\n"
"}\n"
"fn mRotZ(a: f32) -> mat4x4<f32> {\n"
"  let c = cos(a); let s = sin(a);\n"
"  return mat4x4<f32>(vec4<f32>(c,s,0.0,0.0), vec4<f32>(-s,c,0.0,0.0), vec4<f32>(0.0,0.0,1.0,0.0), vec4<f32>(0.0,0.0,0.0,1.0));\n"
"}\n"
"fn swayModel(pos: vec3<f32>, yaw: f32, tiltX: f32, tiltY: f32, scale: vec3<f32>) -> mat4x4<f32> {\n"
"  return mTranslate(pos) * (mRotZ(yaw) * (mRotX(tiltX) * (mRotY(tiltY) * mScale3(scale))));\n"
"}\n"
"@compute @workgroup_size(64)\n"
"fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
"  let i = gid.x;\n"
"  let count = u32(G.b.x);\n"
"  if (i >= count) { return; }\n"
"  let side = u32(sqrt(G.b.x));\n"
"  let gx = i % side;\n"
"  let gy = i / side;\n"
"  let extent = G.b.y;\n"
"  let spacing = extent / f32(side);\n"
/* WORLD-SPACE cells (grass epic #487): a window of world cells centred on the
 * player's cell, hashed on the WORLD cell index — so each cell always holds the
 * same blade and the field stays put in the world while the player walks through
 * it (deterministic, no placement pop when the window slides). */
"  let playerCellX = i32(floor(G.a.x / spacing));\n"
"  let playerCellY = i32(floor(G.a.y / spacing));\n"
"  let worldCellX = playerCellX + i32(gx) - i32(side) / 2;\n"
"  let worldCellY = playerCellY + i32(gy) - i32(side) / 2;\n"
"  let cellX = f32(worldCellX) * spacing;\n"
"  let cellY = f32(worldCellY) * spacing;\n"
"  let cellI = vec2<i32>(worldCellX, worldCellY);\n"
"  let jx = hash2u(cellI, 7u);\n"
"  let jy = hash2u(cellI, 13u);\n"
"  let hp = hash2u(cellI, 29u);\n"
"  let hs = hash2u(cellI, 43u);\n"
"  let bx = cellX + (jx - 0.5) * spacing * 0.9;\n"
"  let by = cellY + (jy - 0.5) * spacing * 0.9;\n"
"  let bz = terrainHeight(bx, by, G.a.z);\n"
"  let yaw = hp * 6.2831853;\n"
/* Independent per-blade height + width variation (grass epic #487): the blade
 * geometry is a unit strip (x in [-0.5,0.5], z in [0,1]) generated in the render
 * shader, so a non-uniform scale here gives each blade its own height and
 * thickness. */
"  let hHeight = hash2u(cellI, 61u);\n"
"  let hWidth = hash2u(cellI, 71u);\n"
"  let baseHeight = 0.30 + hHeight * 0.40;\n"
"  let width = 0.055 + hWidth * 0.045;\n"
/* Smooth circular edge fade (grass epic #487 — no hard field boundary or rings):
 * blades shrink to nothing toward the edge of the radius, which also hides the
 * square grid's far corners. A zero-height blade is invisible (real culling that
 * skips its verts comes with the indirect-draw pass). */
"  let distToPlayer = distance(vec2<f32>(bx, by), G.a.xy);\n"
"  let radius = extent * 0.5;\n"
"  let edgeFade = 1.0 - smoothstep(radius * 0.6, radius * 0.98, distToPlayer);\n"
"  let height = baseHeight * edgeFade;\n"
"  let scale = vec3<f32>(width, width, height);\n"
/* Upright placement only. The wind bend is applied per-vertex in the render
 * shader (grass epic #487) so it can curve the blade progressively (base fixed,
 * tip leaning most) from a coherent world-space wave, which a single rigid model
 * tilt cannot do. model == prevModel; the render shader derives motion from the
 * bend at time and time-dt. */
"  let pos = vec3<f32>(bx, by, bz);\n"
/* GPU CULLING (grass epic #487 indirect / #488): skip faded blades and anything
 * outside the view cone, then append the survivors to a COMPACTED list via an
 * atomic on the indirect draw's instanceCount. Blades within nearKeep (G.c.w) of
 * the camera are always kept so the field never holes out around the player; the
 * cone (dot vs forward > G.d.w) removes off-axis and behind-camera blades. Only
 * the survivors are written + drawn. */
"  if (height < 0.02) { return; }\n"
"  let toBlade = pos - G.c.xyz;\n"
"  let dist = length(toBlade);\n"
"  let cosA = dot(toBlade / max(dist, 0.0001), normalize(G.d.xyz));\n"
"  if (dist > G.c.w && cosA < G.d.w) { return; }\n"
/* Distance LOD/density (grass epic #487): thin blades out with distance so the
 * field can extend far without far-away overdraw — full density near the player,
 * dropping to ~30% at the radius. Deterministic per world cell (no flicker). */
"  let lodT = clamp((dist - 8.0) / max(radius - 8.0, 1.0), 0.0, 1.0);\n"
"  let keepProb = 1.0 - lodT * 0.7;\n"
"  if (hash2u(cellI, 151u) > keepProb) { return; }\n"
"  let slot = atomicAdd(&indirect.instanceCount, 1u);\n"
"  let m = swayModel(pos, yaw, 0.0, 0.0, scale);\n"
"  outBuf[slot].model = m;\n"
"  outBuf[slot].prevModel = m;\n"
/* Per-blade colour: hue/brightness spread so the field is not one flat green. */
"  let cvar = hash2u(cellI, 83u);\n"
"  let cbright = 0.75 + hash2u(cellI, 97u) * 0.5;\n"
"  outBuf[slot].albedoMetallic = vec4<f32>((0.12 + cvar * 0.12) * cbright, (0.30 + cvar * 0.20) * cbright, (0.04 + cvar * 0.07) * cbright, 0.0);\n"
"  let mode = select(0.0, 1.0, G.b.z > 0.5);\n"
/* Per-blade TIP sharpness (grass epic #487): params.w carries the top width the
 * render shader tapers to. Squared hash biases MOST blades to a sharp point
 * (near 0) while leaving some flat-topped for variety. */
"  let hTip = hash2u(cellI, 113u);\n"
"  let tipWidth = hTip * hTip * 0.34;\n"
"  outBuf[slot].params = vec4<f32>(0.95, 1.0, mode, tipWidth);\n"
"}\n";

const char* rae_gb_wgsl(void)      { return GB_WGSL; }
const char* rae_gb_skin_wgsl(void) { return GB_SKIN_WGSL; }
const char* rae_gb_entry_vs(void)  { return "vs"; }
const char* rae_gb_entry_fs(void)  { return "fs"; }
/* Grass RENDER shader (grass epic #487): procedural blade geometry generated
 * in the vertex shader from @builtin(vertex_index) — NO mesh, no vertex buffer.
 * Each blade is a tapered quad (6 verts, two triangles) in unit space that the
 * compute-written model matrix scales to the blade's height/width, orients by
 * yaw, and sways. Writes the SAME G-buffer channels as GB_WGSL (octEncode +
 * motion), so the deferred lighting treats grass exactly like any other surface.
 * Reuses the "vs"/"fs" entry-point names. */
static const char* GB_GRASS_RENDER_WGSL =
"struct Frame {\n"
"  viewProj: mat4x4<f32>,\n"
"  prevViewProj: mat4x4<f32>,\n"
"  jitter: vec4<f32>,\n"
"};\n"
"struct DrawU {\n"
"  model: mat4x4<f32>,\n"
"  prevModel: mat4x4<f32>,\n"
"  albedoMetallic: vec4<f32>,\n"
"  params: vec4<f32>,\n"
"};\n"
"struct GrassU {\n"
"  a: vec4<f32>,\n"   /* playerX, playerY, groundZ, time */
"  b: vec4<f32>,\n"   /* count, extent, toon, dt */
"};\n"
"@group(0) @binding(0) var<uniform> F: Frame;\n"
"@group(0) @binding(1) var<storage, read> draws: array<DrawU>;\n"
"@group(0) @binding(2) var<uniform> GW: GrassU;\n"
GB_OCT_WGSL
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) nrm: vec3<f32>,\n"
"  @location(1) @interpolate(flat) inst: u32,\n"
"  @location(2) clipNow: vec4<f32>,\n"
"  @location(3) clipPrev: vec4<f32>,\n"
"};\n"
/* Coherent WORLD-SPACE wind (grass epic #487): a travelling wave keyed on the
 * blade's base world position, so neighbours bend together into gusts rather
 * than each blade twitching on its own phase. Returns a signed lean magnitude. */
"fn windBend(base: vec2<f32>, time: f32) -> f32 {\n"
"  let phase = base.x * 0.5 + base.y * 0.35;\n"
"  return 0.22 * sin(time * 1.5 + phase) + 0.06 * sin(time * 3.3 + phase * 1.9) + 0.10;\n"
"}\n"
/* Unit blade: x in [-halfWidth, +halfWidth] (tapering base 0.5 -> tip),
 * z in [0,1] up. Two triangles, six vertices. */
"fn bladeLocal(vi: u32, tipW: f32) -> vec3<f32> {\n"
"  var corner = 0u;\n"
"  if (vi == 0u) { corner = 0u; }\n"
"  else if (vi == 1u) { corner = 1u; }\n"
"  else if (vi == 2u) { corner = 3u; }\n"
"  else if (vi == 3u) { corner = 0u; }\n"
"  else if (vi == 4u) { corner = 3u; }\n"
"  else { corner = 2u; }\n"
"  let top = (corner == 2u) || (corner == 3u);\n"
"  let right = (corner == 1u) || (corner == 3u);\n"
"  let z = select(0.0, 1.0, top);\n"
"  let w = select(0.5, tipW, top);\n"
"  let x = select(-w, w, right);\n"
"  return vec3<f32>(x, 0.0, z);\n"
"}\n"
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32, @builtin(vertex_index) vi: u32) -> VsOut {\n"
"  let d = draws[ii];\n"
"  let lp = bladeLocal(vi, d.params.w);\n"
"  var o: VsOut;\n"
/* Base world position lives in the model's translation column; the wave is keyed
 * on it so the gust is coherent across the field. The bend is applied AFTER the
 * model transform, horizontally, scaled by z*z so the base stays planted and the
 * tip leans most — a progressive curve, not a rigid tilt. */
"  let base = vec2<f32>(d.model[3].x, d.model[3].y);\n"
"  let windDir = normalize(vec2<f32>(0.82, 0.57));\n"
"  let zc = lp.z * lp.z;\n"
"  let wNow = d.model * vec4<f32>(lp, 1.0);\n"
"  let bendN = windBend(base, GW.a.w);\n"
"  let pNow = vec3<f32>(wNow.x + windDir.x * bendN * zc, wNow.y + windDir.y * bendN * zc, wNow.z);\n"
"  o.pos = F.viewProj * vec4<f32>(pNow, 1.0);\n"
/* Grass reads as an up-facing surface (lit like the ground it grows from),\n"
 * which avoids near-black blades side-on. */
"  o.nrm = normalize((d.model * vec4<f32>(0.0, 0.0, 1.0, 0.0)).xyz);\n"
"  o.inst = ii;\n"
"  o.clipNow = o.pos;\n"
"  let wPrev = d.prevModel * vec4<f32>(lp, 1.0);\n"
"  let bendP = windBend(base, GW.a.w - GW.b.w);\n"
"  let pPrev = vec3<f32>(wPrev.x + windDir.x * bendP * zc, wPrev.y + windDir.y * bendP * zc, wPrev.z);\n"
"  o.clipPrev = F.prevViewProj * vec4<f32>(pPrev, 1.0);\n"
"  o.pos = vec4<f32>(o.pos.xy + F.jitter.xy * o.pos.w, o.pos.zw);\n"
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
"  let n = normalize(in.nrm);\n"
"  let oct = octEncode(n);\n"
"  let rough = clamp(d.params.x, 0.045, 1.0);\n"
"  let now = in.clipNow.xy / in.clipNow.w;\n"
"  let prev = in.clipPrev.xy / in.clipPrev.w;\n"
"  let motion = (now - prev) * vec2<f32>(0.5, -0.5);\n"
"  let mEnc = clamp(motion, vec2<f32>(-0.5), vec2<f32>(0.5)) + vec2<f32>(" GB_MOTION_ZERO_WGSL ");\n"
"  var o: FsOut;\n"
"  o.gba = vec4<f32>(oct.x, oct.y, 0.5, d.params.z);\n"
"  o.gbb = vec4<f32>(d.albedoMetallic.rgb, rough);\n"
"  o.gbc = vec4<f32>(mEnc.x, mEnc.y, clamp(d.albedoMetallic.a, 0.0, 1.0), d.params.y);\n"
"  return o;\n"
"}\n";

const char* rae_gb_grass_wgsl(void)        { return GB_GRASS_WGSL; }
const char* rae_gb_grass_render_wgsl(void) { return GB_GRASS_RENDER_WGSL; }
const char* rae_gb_grass_entry(void)       { return "main"; }

/* Small opaque-pointer slot store for the Rae grass compute module (#486): it
 * creates its pipeline/buffers/bind-groups in Rae over the bindings but needs a
 * place to keep the handles across frames, and Rae has no module-level Ptr var.
 * Mirrors how the rest of the renderer keeps its handles in C globals. */
static void* g_grass_ptrs[8];
void  rae_gb_grass_set(int64_t index, void* ptr) { if (index >= 0 && index < 8) g_grass_ptrs[index] = ptr; }
void* rae_gb_grass_get(int64_t index) { return (index >= 0 && index < 8) ? g_grass_ptrs[index] : (void*)0; }
void rae_gb_set_pipeline(void* p)      { gb_pipeline = (WGPURenderPipeline)p; }
void rae_gb_set_skin_pipeline(void* p) { gb_skin_pipeline = (WGPURenderPipeline)p; }

/* Accessors + setters for the Rae-side buffer / bind-group creation (#503). */
void* rae_gb_frame_ubuf(void)      { return (void*)gb_frame_ubuf; }
int64_t rae_gb_frame_bytes(void)   { return (int64_t)GB_FRAME_BYTES; }
int64_t rae_gb_draws_size(void)    { return (int64_t)((uint64_t)GB_MAX_DRAWS * GB_DRAW_FLOATS * sizeof(float)); }
void rae_gb_set_static_bind(void* bind) { gb_bind = (WGPUBindGroup)bind; }
void rae_gb_set_frame_ubuf(void* buf)   { gb_frame_ubuf = (WGPUBuffer)buf; }
void rae_gb_set_draws_buffer(void* buf) { gb_draw_sbuf = (WGPUBuffer)buf; }

/* The G-buffer inspector (pipeline + uniform + bind group + the fullscreen
 * pass) is now built in Rae (lib/gbuffer.rae: ensureViewPipeline / ensureViewBind
 * / debugView, #503). C exposes the inspector WGSL, the presentable target's
 * format + view, and the handles/setters. The inspector bind references the
 * G-buffer views, so gb_release_targets nulls gb_view_bind on resize and Rae
 * re-creates it (its ensureViewBind flag is reset when the targets rebuild). */
const char* rae_gb_view_wgsl(void)  { return GB_VIEW_WGSL; }
int64_t rae_g2d_format(void)        { return (int64_t)g_g2d_fmt; }
void* rae_g2d_off_view(void)        { return (void*)g_g2d_off_view; }
int64_t rae_g2d_off_view_ready(void){ return (g_wgpu_dev && g_g2d_off_view) ? 1 : 0; }
void* rae_gb_view_pipeline(void)    { return (void*)gb_view_pipeline; }
void* rae_gb_view_ubuf(void)        { return (void*)gb_view_ubuf; }
void* rae_gb_view_bind(void)        { return (void*)gb_view_bind; }
void rae_gb_set_view_pipeline(void* p) { gb_view_pipeline = (WGPURenderPipeline)p; }
void rae_gb_set_view_ubuf(void* b)     { gb_view_ubuf = (WGPUBuffer)b; }
void rae_gb_set_view_bind(void* b)     { gb_view_bind = (WGPUBindGroup)b; }

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
int64_t rae_gb_offscreen_w(void)  { return (int64_t)rae_gb_scale_dim(g_g2d_off_w); }
int64_t rae_gb_offscreen_h(void)  { return (int64_t)rae_gb_scale_dim(g_g2d_off_h); }
int64_t rae_gb_targets_match(int64_t w, int64_t h) {
    return (gb_depth_view && (int)w == gb_target_w && (int)h == gb_target_h) ? 1 : 0;
}
int64_t rae_gb_targets_ready(void) {
    return (gb_a_view && gb_b_view && gb_c_view && gb_depth_view) ? 1 : 0;
}
void rae_gb_release_targets_ext(void) { gb_release_targets(); }
void rae_gb_set_target(int64_t idx, void* tex, void* view) {
    switch ((int)idx) {
        case 0: gb_a_tex = (WGPUTexture)tex;     gb_a_view = (WGPUTextureView)view;     break;
        case 1: gb_b_tex = (WGPUTexture)tex;     gb_b_view = (WGPUTextureView)view;     break;
        case 2: gb_c_tex = (WGPUTexture)tex;     gb_c_view = (WGPUTextureView)view;     break;
        case 3: gb_depth_tex = (WGPUTexture)tex; gb_depth_view = (WGPUTextureView)view; break;
        default: break;
    }
}
void rae_gb_commit_targets(int64_t w, int64_t h) {
    gb_target_w = (int)w; gb_target_h = (int)h; gb_targets_gen++;
}
/* Bumped whenever the targets are recreated (resize). The inspector rebuilds
 * its bind group when this changes, since it samples the G-buffer views. */
int64_t rae_gb_targets_gen(void) { return (int64_t)gb_targets_gen; }

/* Reset the per-frame counters and build+upload the TAA-jittered frame uniform
 * (stateful CPU math that stays C). Needs the Rae-created frame uniform buffer. */
int64_t rae_gb_frame_uniform(rae_Mat4* viewProj, float clearR, float clearG, float clearB) {
    if (!viewProj || !gb_frame_ubuf) return 0;
    gb_draw_count = 0;
    /* Without this the counter climbs past GB_SDF_MAX_GROUPS after a few
     * frames and every later cluster is silently dropped — metaballs that
     * render for two frames and then vanish, with no error anywhere. The
     * forward path resets its equivalent in exactly this place. */
    gb_sdf_group = 0;
    memcpy(gb_viewproj, viewProj->m.v, 16 * sizeof(float));
    gb_clear[0] = clearR; gb_clear[1] = clearG; gb_clear[2] = clearB;
    /* First frame has no previous view-projection; reusing this one gives
     * zero motion, which is right — an uninitialised matrix would project
     * every pixel to the origin and read as the whole screen streaking. */
    if (!gb_have_prev_vp) {
        memcpy(gb_prev_viewproj, viewProj->m.v, 16 * sizeof(float));
        gb_have_prev_vp = true;
    }
    /* Halton(2,3), the same low-discrepancy sequence the forward path
     * uses. Index 0 is skipped so no frame lands on a zero offset, which
     * would be a frame that contributes nothing new. */
    float jx = 0.0f, jy = 0.0f;
    if (gb_target_w > 0 && gb_target_h > 0) {
        const int hi = (gb_jitter_frame % 16) + 1;
        jx = (g3d_halton(hi, 2) - 0.5f) * 2.0f / (float)gb_target_w;
        jy = (g3d_halton(hi, 3) - 0.5f) * 2.0f / (float)gb_target_h;
    }
    gb_jitter_frame++;

    /* The rasterised matrix = jitter * viewProj. A clip-space xy offset
     * proportional to w is exactly adding jx*(row 3) to row 0 and
     * jy*(row 3) to row 1; column-major, row r of column c is m[c*4+r]. */
    memcpy(gb_viewproj_jittered, viewProj->m.v, 16 * sizeof(float));
    for (int c = 0; c < 4; c++) {
        gb_viewproj_jittered[c * 4 + 0] += jx * gb_viewproj_jittered[c * 4 + 3];
        gb_viewproj_jittered[c * 4 + 1] += jy * gb_viewproj_jittered[c * 4 + 3];
    }

    float fu[36];
    memset(fu, 0, sizeof(fu));
    memcpy(fu, viewProj->m.v, 16 * sizeof(float));
    memcpy(fu + 16, gb_prev_viewproj, 16 * sizeof(float));
    fu[32] = jx; fu[33] = jy;
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_frame_ubuf, 0, fu, GB_FRAME_BYTES);
    /* Remember for NEXT frame, after this frame's copy is on the GPU. */
    memcpy(gb_prev_viewproj, viewProj->m.v, 16 * sizeof(float));
    return 1;
}

/* G-buffer target views for the Rae-built render pass (#503). Valid after a
 * frame prep returned 1. The clear values / attachment layout live in Rae now;
 * these just hand over the opaque view handles. */
void* rae_gb_view_a(void)     { return (void*)gb_a_view; }
void* rae_gb_view_b(void)     { return (void*)gb_b_view; }
void* rae_gb_view_c(void)     { return (void*)gb_c_view; }
void* rae_gb_view_depth(void) { return (void*)gb_depth_view; }
/* The biased zero the motion channel clears to (128/255); Rae uses it for the
 * C-target clear so a static background reads as "not moving". */
float rae_gb_motion_zero(void) { return GB_MOTION_ZERO; }
/* Store the Rae-created command encoder + render pass so the draws and the C
 * metaball path see the live frame, and end() can finish it. */
void rae_gb_set_frame(void* enc, void* pass) {
    gb_enc = (WGPUCommandEncoder)enc;
    gb_pass = (WGPURenderPassEncoder)pass;
}

/* Queue one mesh into the G-buffer. `model` arrives as a Mat4 by value —
 * 16 floats the caller already had on the stack — and is memcpy'd into a
 * preallocated slot. Nothing on this path touches the allocator. */
/* Queue a SKINNED mesh into the G-buffer (#391). Shares the draw record
 * and the storage buffer with the static path — the layouts are identical
 * — and switches pipeline for the duration of the draw, then hands the
 * pass back so a following static draw is unaffected. */
/* ---- Skinned draw: context accessors for the Rae port (#503) ---------------
 *
 * The single static AND skinned draws now run in Rae (lib/gbuffer.rae: draw /
 * drawSkinned), each building its one DrawU record with packInstanceRecord and
 * going through the same Rae upload+draw path as the instanced drawRecords.
 * These accessors hand Rae the skin pipeline / bind group / mesh buffers.
 * The skin bind group is still created here (lazily): it needs the pipeline's
 * layout plus the palette storage buffer, genuine resource creation that stays
 * C for now (its Rae migration is later in #503/#504). */
void* rae_gb_skin_pipeline(void) { return (void*)gb_skin_pipeline; }
/* The skin bind group (frame uniform + draws buffer + joint palette) is created
 * in Rae now (lib/gbuffer.rae:ensureSkinBind, #503) and stored back here. */
void* rae_gb_skin_bind(void) { return (void*)gb_skin_bind; }
void rae_gb_set_skin_bind(void* bind) { gb_skin_bind = (WGPUBindGroup)bind; }
/* The joint palette storage buffer + its byte size, for the Rae-built skin bind
 * group. It comes up asynchronously with the first skinned upload, so a
 * readiness check lets Rae defer creating the bind until it exists. */
void* rae_gb_skin_palette(void)      { return (void*)g3d_skin_palette_sbuf; }
int64_t rae_gb_skin_palette_size(void){ return (int64_t)((uint64_t)G3D_SKIN_MAX_JOINTS * 12 * sizeof(float)); }
int64_t rae_gb_skin_palette_ready(void){ return g3d_skin_palette_sbuf ? 1 : 0; }
/* 1 if a skinned draw of `mesh` can proceed (pass open, skin pipeline + bind
 * available, mesh slot valid) else 0. The bind is created by Rae's
 * ensureSkinBind before this is checked. */
int64_t rae_gb_skin_ready(int64_t mesh) {
    int slot = (int)mesh - 1;
    if (!gb_pass || !gb_skin_pipeline || !gb_skin_bind) return 0;
    if (slot < 0 || slot >= g3d_skin_mesh_n) return 0;
    if (!g3d_skin_vbuf[slot] || !g3d_skin_ibuf[slot]) return 0;
    return 1;
}
void* rae_gb_skin_vbuf(int64_t mesh)  { int s=(int)mesh-1; return (s>=0 && s<g3d_skin_mesh_n)?(void*)g3d_skin_vbuf[s]:NULL; }
void* rae_gb_skin_ibuf(int64_t mesh)  { int s=(int)mesh-1; return (s>=0 && s<g3d_skin_mesh_n)?(void*)g3d_skin_ibuf[s]:NULL; }
int64_t rae_gb_skin_icount(int64_t mesh){ int s=(int)mesh-1; return (s>=0 && s<g3d_skin_mesh_n)?(int64_t)g3d_skin_icount[s]:0; }

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
void* rae_gb_pass(void)          { return (void*)gb_pass; }
void* rae_gb_static_pipeline(void){ return (void*)gb_pipeline; }
void* rae_gb_static_bind(void)   { return (void*)gb_bind; }
void* rae_gb_draws_buffer(void)  { return (void*)gb_draw_sbuf; }
int64_t rae_gb_max_draws(void)   { return (int64_t)GB_MAX_DRAWS; }
int64_t rae_gb_draw_count(void)  { return (int64_t)gb_draw_count; }
void rae_gb_advance_draws(int64_t count) {
    if (count <= 0) return;
    if (gb_draw_count + (int)count > GB_MAX_DRAWS) gb_draw_count = GB_MAX_DRAWS;
    else gb_draw_count += (int)count;
}
/* 1 if an instanced draw of `mesh` can proceed this frame (pass open, mesh slot
 * valid and its buffers uploaded), else 0 — Rae checks this rather than testing
 * opaque Ptr handles for null. */
int64_t rae_gb_mesh_ready(int64_t mesh) {
    int slot = (int)mesh - 1;
    if (!gb_pass) return 0;
    if (slot < 0 || slot >= g3d_mesh_n) return 0;
    if (!g3d_mesh_vbuf[slot] || !g3d_mesh_ibuf[slot]) return 0;
    return 1;
}
void* rae_gb_mesh_vbuf(int64_t mesh)  { int s=(int)mesh-1; return (s>=0 && s<g3d_mesh_n)?(void*)g3d_mesh_vbuf[s]:NULL; }
void* rae_gb_mesh_ibuf(int64_t mesh)  { int s=(int)mesh-1; return (s>=0 && s<g3d_mesh_n)?(void*)g3d_mesh_ibuf[s]:NULL; }
int64_t rae_gb_mesh_icount(int64_t mesh){ int s=(int)mesh-1; return (s>=0 && s<g3d_mesh_n)?(int64_t)g3d_mesh_icount[s]:0; }

/* Finish and submit the geometry pass. Uniform data uploads once here, not
 * per draw. */
/* The geometry pass's finish/submit (end()) now runs in Rae over the bindings
 * (lib/gbuffer.rae:endPass, #503). These accessors hand Rae the frame's command
 * encoder and let it clear the encoder/pass globals once the pass is submitted,
 * so the C metaball path and the draws see a live pass during the frame and a
 * cleared one after. */
void* rae_gb_encoder(void) { return (void*)gb_enc; }
void rae_gb_clear_frame(void) { gb_pass = NULL; gb_enc = NULL; }
/* 1 while a geometry pass is open (between begin and end), so Rae can guard
 * end() without null-testing an opaque Ptr. */
int64_t rae_gb_frame_active(void) { return gb_pass ? 1 : 0; }
/* Submit exactly one command buffer. wgpuQueueSubmit takes a POINTER to an
 * array of command buffers; Rae has no way yet to take the address of a single
 * handle (and List(Ptr) — Ptr being Buffer(void) — nests wrongly in the
 * container codegen), so this thin call stays C. Not renderer logic; a general
 * FFI gap to close later (array-of-handles across the boundary). */
void rae_gb_submit(void* cmd) {
    WGPUCommandBuffer c = (WGPUCommandBuffer)cmd;
    wgpuQueueSubmit(g_wgpu_queue, 1, &c);
}

int64_t rae_ext_gbuffer_drawCount(void) { return (int64_t)gb_draw_count; }

/* Write one G-buffer channel into the presentable target. */
/* rae_ext_gbuffer_debugView moved to Rae (lib/gbuffer.rae:debugView, #503). */

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
    if (gb_skin_pipeline) { wgpuRenderPipelineRelease(gb_skin_pipeline); gb_skin_pipeline = NULL; }
    if (gb_skin_bind)     { wgpuBindGroupRelease(gb_skin_bind); gb_skin_bind = NULL; }
    gb_target_w = 0; gb_target_h = 0;
}
