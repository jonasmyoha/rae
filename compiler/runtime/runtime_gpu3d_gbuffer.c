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

/* #912: the static / skin / terrain pipelines and bind groups are manager IDs
 * on the Rae side (lib/Gbuffer.rae, lib/GbufferTerrain.rae); C keeps the frame
 * uniform + draws buffer (adopted there) and the WGSL sources. */
static WGPUBuffer         gb_frame_ubuf = NULL;
static WGPUBuffer         gb_draw_sbuf = NULL;
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

/* #904: the inspector pipeline / uniform / bind are manager IDs on the Rae
 * side (lib/GbufferInspector.rae); C exposes only the inspector WGSL. */

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

/* PROCEDURAL BIG-BRICK MASONRY (#62). No textures in this renderer, so the castle's
 * brick is computed from world position: big old-castle courses, staggered rows,
 * recessed mortar, per-brick shade, ROUNDED faces and an ANALYTIC normal that dips
 * into the seams (the "bumps at the seams" — no image normal map). Triplanar face
 * pick so every box/tower face is bricked. Gated per instance by the caller. */
#define GB_BRICK_WGSL \
"struct BrickOut { albedo: vec3<f32>, nrm: vec3<f32> };\n" \
"fn gbBrick(wpos: vec3<f32>, n: vec3<f32>, base: vec3<f32>) -> BrickOut {\n" \
"  let an = abs(n);\n" \
"  if (n.z > 0.6) { var top: BrickOut; top.albedo = base * 0.9; top.nrm = n; return top; }\n" \
"  var u: f32; var v: f32; var tU: vec3<f32>; var tV: vec3<f32>;\n" \
"  if (an.z >= an.x && an.z >= an.y) { u = wpos.x; v = wpos.y; tU = vec3<f32>(1.0,0.0,0.0); tV = vec3<f32>(0.0,1.0,0.0); }\n" \
"  else if (an.x >= an.y) { u = wpos.y; v = wpos.z; tU = vec3<f32>(0.0,1.0,0.0); tV = vec3<f32>(0.0,0.0,1.0); }\n" \
"  else { u = wpos.x; v = wpos.z; tU = vec3<f32>(1.0,0.0,0.0); tV = vec3<f32>(0.0,0.0,1.0); }\n" \
"  let bw = 1.35; let bh = 0.58;\n" \
"  let row = floor(v / bh);\n" \
"  let stagger = (row - 2.0 * floor(row * 0.5)) * (bw * 0.5);\n" \
"  let uu = u + stagger;\n" \
"  let cu = fract(uu / bw);\n" \
"  let cv = fract(v / bh);\n" \
"  let du = min(cu, 1.0 - cu) * bw;\n" \
"  let dv = min(cv, 1.0 - cv) * bh;\n" \
"  let dm = min(du, dv);\n" \
"  let mortarHalf = 0.05;\n" \
"  let bevel = 0.14;\n" \
"  let mortar = 1.0 - smoothstep(mortarHalf, mortarHalf + bevel, dm);\n" \
"  let bid = floor(uu / bw) * 3.0 + row * 7.0;\n" \
"  let vary = fract(sin(bid * 12.9898) * 43758.5453);\n" \
"  let brickCol = base * (0.92 + 0.14 * vary);\n" \
"  let mortarCol = base * vec3<f32>(0.70, 0.68, 0.66);\n" \
"  var o: BrickOut;\n" \
"  o.albedo = mix(brickCol, mortarCol, mortar);\n" \
"  let bevelAmt = mortar * (1.0 - mortar) * 4.0;\n" \
"  var perturb = vec3<f32>(0.0);\n" \
"  if (du <= dv) { let dir = select(1.0, -1.0, cu < 0.5); perturb = tU * dir; }\n" \
"  else { let dir = select(1.0, -1.0, cv < 0.5); perturb = tV * dir; }\n" \
"  o.nrm = normalize(n + perturb * (bevelAmt * 0.55));\n" \
"  return o;\n" \
"}\n"

/* PROCEDURAL ROOF TILES (#63), deliberately DIFFERENT from the wall brick: rounded
 * FISH-SCALE shingles in overlapping staggered courses, not a rectangular grid. The
 * surface is parameterised along/around the slope (tangent frame from the normal), so
 * courses stack up a cone and wrap around it. Each scale is a rounded dome — the albedo
 * brightens on the dome and darkens in the gaps, and the ANALYTIC normal bulges radially
 * so the scales catch light with real relief (the "bumps at the seams" for roofs). */
#define GB_ROOFTILE_WGSL \
"fn gbRoofTiles(wpos: vec3<f32>, n: vec3<f32>, base: vec3<f32>) -> BrickOut {\n" \
"  let hl = max(length(vec2<f32>(n.x, n.y)), 1e-4);\n" \
"  let horizN = vec2<f32>(n.x, n.y) / hl;\n" \
"  let tU = vec3<f32>(-horizN.y, horizN.x, 0.0);\n" \
"  let tV = normalize(cross(tU, n));\n" \
"  let hu = dot(wpos, tU);\n" \
"  let hv = dot(wpos, tV);\n" \
"  let tw = 0.72; let th = 0.55;\n" \
"  let row = floor(hv / th);\n" \
"  let stag = (row - 2.0 * floor(row * 0.5)) * 0.5;\n" \
"  let cu = fract(hu / tw + stag) - 0.5;\n" \
"  let cv = fract(hv / th) - 0.32;\n" \
"  let dxs = cu;\n" \
"  let dys = cv * 0.92;\n" \
"  let rr = sqrt(dxs * dxs + dys * dys);\n" \
"  let edge = smoothstep(0.33, 0.47, rr);\n" \
"  let dome = 1.0 - edge;\n" \
"  let bid = floor(hu / tw + stag) * 5.0 + row * 11.0;\n" \
"  let vary = fract(sin(bid * 34.11) * 4113.7);\n" \
"  var o: BrickOut;\n" \
"  o.albedo = base * (0.74 + 0.30 * dome + 0.14 * vary) * (1.0 - 0.34 * edge);\n" \
"  let slope = smoothstep(0.04, 0.40, rr) * dome;\n" \
"  var dir = vec2<f32>(dxs, dys);\n" \
"  if (rr > 1e-4) { dir = dir / rr; }\n" \
"  let perturb = tU * dir.x + tV * dir.y;\n" \
"  o.nrm = normalize(n + perturb * (slope * 0.95));\n" \
"  return o;\n" \
"}\n"

/* PROCEDURAL FLAG EMBLEM (#131). No textures in this renderer, so a unit badge is
 * computed from the mesh UV: a WHITE roundel near the TOP of the banner (with padding),
 * carrying a white symbol per unit type — archer = bow + arrow, infantry = crossed
 * swords, cavalry = horseshoe. The banner is ~4:1 tall so the u-radius is ~4x the
 * v-radius to keep the roundel circular in world space. `codeF` (from params.w) selects
 * the symbol. Returns the flag's base colour outside the badge, so it composites over
 * the team-tinted cloth. Deforms WITH the fabric since it lives in UV. */
#define GB_EMBLEM_WGSL \
"fn gbEmblem(uv: vec2<f32>, base: vec3<f32>, codeF: f32) -> vec3<f32> {\n" \
"  let vc = 0.17; let ru = 0.34; let rv = 0.085;\n" \
"  let a = (uv.x - 0.5) / ru;\n" \
"  let b = (uv.y - vc) / rv;\n" \
"  let r = length(vec2<f32>(a, b));\n" \
"  if (r > 1.18) { return base; }\n" \
"  let white = vec3<f32>(0.95, 0.95, 0.92);\n" \
"  if (abs(r - 0.92) < 0.13) { return white; }\n" \
"  if (r >= 0.80) { return base; }\n" \
"  var em = false;\n" \
"  let code = i32(round(codeF));\n" \
"  if (code >= 12) {\n" \
"    em = (abs(r - 0.58) < 0.15) && !(b > 0.28 && abs(a) < 0.44);\n" \
"  } else if (code == 11) {\n" \
"    em = (abs(a - b) < 0.16 || abs(a + b) < 0.16) && r < 0.70;\n" \
"  } else {\n" \
"    let bd = length(vec2<f32>(a + 0.70, b * 0.9));\n" \
"    let bow = (abs(bd - 0.55) < 0.12) && a < 0.05;\n" \
"    let shaft = (abs(b) < 0.11) && a > -0.55 && a < 0.55;\n" \
"    let hu = (abs(b - (0.55 - a)) < 0.13) && a > 0.12 && a < 0.55;\n" \
"    let hd = (abs(b + (0.55 - a)) < 0.13) && a > 0.12 && a < 0.55;\n" \
"    em = bow || shaft || hu || hd;\n" \
"  }\n" \
"  if (em) { return white; }\n" \
"  return base;\n" \
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
"  @location(4) wpos: vec3<f32>,\n"   /* world position, for procedural brick (#62) */
"  @location(5) uv: vec2<f32>,\n"     /* mesh UV, for the procedural flag emblem (#131) */
"};\n"
GB_BRICK_WGSL
GB_ROOFTILE_WGSL
GB_EMBLEM_WGSL
"@vertex\n"
"fn vs(@builtin(instance_index) ii: u32,\n"
"      @location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) uv: vec2<f32>) -> VsOut {\n"
"  let d = draws[ii];\n"
"  var o: VsOut;\n"
"  let world = d.model * vec4<f32>(p, 1.0);\n"
"  o.wpos = world.xyz;\n"
"  o.pos = F.viewProj * world;\n"
/* Uniform-scale normal transform, matching the forward pass's documented
 * constraint. Non-uniform scale needs transpose(inverse(model)); when that
 * arrives it must arrive in both pipelines at once or the two frames will
 * disagree about which way a surface faces. */
"  o.nrm = normalize((d.model * vec4<f32>(n, 0.0)).xyz);\n"
"  o.uv = uv;\n"
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
/* Procedural brick (#62): when the per-instance brick flag (params.w) is set,
 * replace the flat albedo + geometric normal with the masonry surface. Off (the
 * default) leaves the write byte-identical, so no other instanced draw changes. */
"  var albedoOut = d.albedoMetallic.rgb;\n"
"  var nrmOut = n;\n"
"  if (d.params.w > 9.5) {\n"
"    albedoOut = gbEmblem(in.uv, albedoOut, d.params.w);\n" /* #131 flag unit emblem (>=10) */
"  } else if (d.params.w > 1.5) {\n"
"    let rt = gbRoofTiles(in.wpos, n, albedoOut);\n"    /* #63 roof shingles (flag 2) */
"    albedoOut = rt.albedo;\n"
"    nrmOut = rt.nrm;\n"
"  } else if (d.params.w > 0.5) {\n"
"    let bk = gbBrick(in.wpos, n, albedoOut);\n"        /* #62 wall brick (flag 1) */
"    albedoOut = bk.albedo;\n"
"    nrmOut = bk.nrm;\n"
"  }\n"
"  let octB = octEncode(nrmOut);\n"
"  var o: FsOut;\n"
"  o.gba = vec4<f32>(octB.x, octB.y, 0.5, d.params.z);\n"
"  o.gbb = vec4<f32>(albedoOut, rough);\n"
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
/* `base` is this instance's palette offset in vec4 rows (paletteIndex *
 * jointCount * 3). It comes from the per-instance DrawU (params.w) so instances
 * of the same mesh can each read their own animation palette out of one shared
 * palette array — GPU animation instancing (#547). Today every instance packs
 * base = 0, so this reads palette[j*3+..] exactly as before. */
"fn jointMat(j: u32, base: u32) -> mat4x4<f32> {\n"
"  let r0 = palette[base + j * 3u + 0u];\n"
"  let r1 = palette[base + j * 3u + 1u];\n"
"  let r2 = palette[base + j * 3u + 2u];\n"
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
/* Per-instance palette offset (0 for all instances today). */
"  let pbase = u32(max(d.params.w, 0.0));\n"
"  var skin = jointMat(j.x, pbase) * w.x;\n"
"  skin = skin + jointMat(j.y, pbase) * w.y;\n"
"  skin = skin + jointMat(j.z, pbase) * w.z;\n"
"  skin = skin + jointMat(j.w, pbase) * w.w;\n"
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
const char* rae_gb_wgsl(void)      { return GB_WGSL; }
const char* rae_gb_skin_wgsl(void) { return GB_SKIN_WGSL; }
/* The grass shaders are Rae assets since #874: lib/grass_compute.wgsl (composed in
 * Rae with the biome + noise chunks) and lib/grass_render.wgsl; the grass GPU objects
 * are typed IDs in the App-owned manager, so the C slot store is gone too. */

/* Accessors + setters for the Rae-side buffer / bind-group creation (#503). */
void* rae_gb_frame_ubuf(void)      { return (void*)gb_frame_ubuf; }
int64_t rae_gb_frame_bytes(void)   { return (int64_t)GB_FRAME_BYTES; }
int64_t rae_gb_draws_size(void)    { return (int64_t)((uint64_t)GB_MAX_DRAWS * GB_DRAW_FLOATS * sizeof(float)); }
void rae_gb_set_frame_ubuf(void* buf)   { gb_frame_ubuf = (WGPUBuffer)buf; }
void rae_gb_set_draws_buffer(void* buf) { gb_draw_sbuf = (WGPUBuffer)buf; }

/* The G-buffer inspector (pipeline + uniform + bind group + the fullscreen
 * pass) is built in Rae (lib/GbufferInspector.rae, #503) and, since #904, its
 * objects are manager IDs there. C exposes the inspector WGSL and the
 * presentable target's format + view; Rae rebuilds the bind (which samples the
 * G-buffer views) when gb_targets_gen changes. */
const char* rae_gb_view_wgsl(void)  { return GB_VIEW_WGSL; }
int64_t rae_g2d_format(void)        { return (int64_t)g_g2d_fmt; }
void* rae_g2d_off_view(void)        { return (void*)g_g2d_off_view; }
int64_t rae_g2d_off_view_ready(void){ return (g_wgpu_dev && g_g2d_off_view) ? 1 : 0; }

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
    /* Jitter only feeds TAA. With TAA off (#20) it would just shimmer the
     * un-resolved image, so suppress it — matching the forward path, which also
     * zeroes jitter when its TAA is disabled. */
    float jx = 0.0f, jy = 0.0f;
    if (gb_target_w > 0 && gb_target_h > 0 && rae_gb_taa_is_enabled()) {
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
/* The joint palette storage buffer + its byte size, for the Rae-built skin bind
 * group. It comes up asynchronously with the first skinned upload, so a
 * readiness check lets Rae defer creating the bind until it exists. */
void* rae_gb_skin_palette(void)      { return (void*)g3d_skin_palette_sbuf; }
int64_t rae_gb_skin_palette_size(void){ return (int64_t)((uint64_t)G3D_SKIN_MAX_JOINTS * 12 * G3D_SKIN_MAX_PALETTES * sizeof(float)); }
int64_t rae_gb_skin_palette_ready(void){ return g3d_skin_palette_sbuf ? 1 : 0; }
/* 1 if a skinned draw of `mesh` can proceed (pass open, skin pipeline + bind
 * available, mesh slot valid) else 0. The bind is created by Rae's
 * ensureSkinBind before this is checked. */
int64_t rae_gb_skin_ready(int64_t mesh) {
    int slot = (int)mesh - 1;
    /* #912: the skin pipeline / bind are manager objects checked on the Rae side. */
    if (!gb_pass) return 0;
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

int64_t rae_ext_Gbuffer_drawCount(void) { return (int64_t)gb_draw_count; }

/* Write one G-buffer channel into the presentable target. */
/* The debug-view pass moved to Rae (lib/gbuffer_inspector.rae debugView, #503). */

/* Present the composed frame. Shares the platform copy-to-drawable with
 * the forward frame — see rae_g3d_present_offscreen. Reached from Rae as
 * gbuffer.present() -> presentFrame() -> renderDeferredPass (no-UI present). */
void rae_ext_Gbuffer_present(void) {
    rae_g2d_tick_virtual_clock();
    rae_g3d_present_offscreen();
}

void rae_ext_Gbuffer_shutdown(void) {
    gb_release_targets();
    if (gb_draw_sbuf)     { wgpuBufferRelease(gb_draw_sbuf); gb_draw_sbuf = NULL; }
    if (gb_frame_ubuf)    { wgpuBufferRelease(gb_frame_ubuf); gb_frame_ubuf = NULL; }
    gb_target_w = 0; gb_target_h = 0;
}
