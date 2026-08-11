/* Deferred frame — depth pyramid, lighting, composite (#356, Track B).
 *
 * The passes that consume what runtime_gpu3d_gbuffer.c produces. Split
 * into its own file because the geometry pass and the passes that read it
 * are separate concerns that change for different reasons, and because
 * runtime_gpu3d.c is already the cautionary tale for letting a renderer
 * family accumulate in one file (queue #364).
 *
 * DEPTH PYRAMID. A mip chain of depth reduced to the NEAREST value, not an
 * average. The consumer is occlusion: "is anything in this screen region
 * closer than my bounding box" is answered by the nearest depth, and an
 * averaged pyramid would report a plausible-looking value that is
 * conservative in neither direction — it would cull visible geometry.
 * Under reverse-Z (#367) nearest is the MAXIMUM, so the reduction is max;
 * it was min before the depth convention flipped, and a pyramid reducing
 * the wrong way is invisible until something culls with it.
 * Built with fragment passes rather than compute for the same reason the
 * SSAO pass is: they run everywhere WebGPU runs, with no workgroup-size
 * guess. Hi-Z culling and a deferred SSAO are the intended readers; the
 * chain exists now so those arrive as consumers of a real resource rather
 * than as a reshaping of the frame.
 *
 * LIGHTING. One fullscreen pass, Cook-Torrance GGX + Smith + Schlick over
 * the G-buffer, writing LINEAR HDR radiance into litColor. This is the
 * payoff of the whole split: its cost is one evaluation per pixel and does
 * not scale with how many objects wrote that pixel. World position is
 * reconstructed from depth and the inverse view-projection rather than
 * read from a stored position buffer — the reason the G-buffer has no
 * position channel.
 *
 * COMPOSITE. Exposure + ACES + gamma, litColor -> the presentable
 * offscreen. Identical tone curve to the forward frame's tonemap on
 * purpose: the two frames should differ in how radiance is computed, not
 * in how it is displayed, or comparing them proves nothing.
 */

#define GB_PYRAMID_MAX_MIPS 16
/* mat4 invViewProj + 6 vec4 + mat4 viewProj (#387 march) + 3 vec4 of sky
 * (#400/#404). The sky rides the LIGHTING uniform rather than getting its
 * own buffer because the background and the ambient are the same
 * environment: two buffers is two chances for the sky you see and the
 * sky you are lit by to be a frame apart. */
#define GB_LIGHT_BYTES 416

static WGPUTexture     gb_pyramid_tex = NULL;    /* r32float, half res, mip chain */
static WGPUTextureView gb_pyramid_rt[GB_PYRAMID_MAX_MIPS];   /* one per mip, as target */
static WGPUTextureView gb_pyramid_src[GB_PYRAMID_MAX_MIPS];  /* one per mip, as source */
static int             gb_pyramid_mips = 0;
static int             gb_pyramid_w = 0, gb_pyramid_h = 0;

static WGPUTexture     gb_lit_tex = NULL;        /* linear HDR radiance */
static WGPUTextureView gb_lit_view = NULL;
/* HDR radiance format (#370). rg11b10ufloat halves the bandwidth of the
 * largest full-res float target in the frame, but it is an optional
 * WebGPU feature; when the adapter does not offer it we fall back to
 * rgba16float, which is guaranteed. The pipeline is created from this
 * same variable, so the two cannot drift apart. */
static WGPUTextureFormat gb_lit_format = WGPUTextureFormat_RGBA16Float;

static WGPURenderPipeline gb_pyr_from_depth_pipeline = NULL;  /* depth32f -> r32f */
static WGPURenderPipeline gb_pyr_reduce_pipeline = NULL;      /* r32f -> r32f */
static WGPUBindGroup      gb_pyr_bind[GB_PYRAMID_MAX_MIPS];

static WGPURenderPipeline gb_light_pipeline = NULL;
static WGPUBindGroup      gb_light_bind = NULL;
static WGPUBuffer         gb_light_ubuf = NULL;

static WGPURenderPipeline gb_composite_pipeline = NULL;
/* One per history slot: the composite's source alternates with the TAA
 * ping-pong, and a single cached bind group would pin frame one's
 * choice forever. */
static WGPUBindGroup      gb_composite_bind[2] = {NULL, NULL};
static WGPUBuffer         gb_composite_ubuf = NULL;
static WGPUBuffer         gb_taa_ubuf = NULL;

static int gb_deferred_gen = -1;   /* gb_targets_gen this file's binds were built for */

/* Fullscreen triangle, shared by every pass here. Three vertices covering
 * the viewport beats two triangles: no shared edge, so no risk of a seam
 * from inconsistent interpolation along the diagonal. */
#define GB_FULLSCREEN_VS \
"@vertex\n" \
"fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {\n" \
"  var points = array<vec2<f32>, 3>(\n" \
"    vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));\n" \
"  return vec4<f32>(points[vi], 0.0, 1.0);\n" \
"}\n"

/* Mip 0 of the pyramid: reduce full-res depth into the half-res chain. */
static const char* GB_PYR_FROM_DEPTH_WGSL =
"@group(0) @binding(0) var srcTex: texture_depth_2d;\n"
GB_FULLSCREEN_VS
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) f32 {\n"
"  let px = vec2<i32>(pos.xy) * 2;\n"
"  let dim = vec2<i32>(textureDimensions(srcTex, 0)) - vec2<i32>(1);\n"
/* Clamp rather than skip: on an odd-sized level the 2x2 footprint runs
 * off the edge, and sampling out of bounds returns zero — which under
 * reverse-Z is the FAR plane, so an unclamped border would report nothing
 * occluding rather than something wrongly close. Clamping keeps the border
 * honest either way. */
"  let a = textureLoad(srcTex, min(px,                  dim), 0);\n"
"  let b = textureLoad(srcTex, min(px + vec2<i32>(1,0), dim), 0);\n"
"  let c = textureLoad(srcTex, min(px + vec2<i32>(0,1), dim), 0);\n"
"  let d = textureLoad(srcTex, min(px + vec2<i32>(1,1), dim), 0);\n"
"  return max(max(a, b), max(c, d));\n"
"}\n";

/* Mip n from mip n-1. Same reduction, non-depth source format. */
static const char* GB_PYR_REDUCE_WGSL =
"@group(0) @binding(0) var srcTex: texture_2d<f32>;\n"
GB_FULLSCREEN_VS
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) f32 {\n"
"  let px = vec2<i32>(pos.xy) * 2;\n"
"  let dim = vec2<i32>(textureDimensions(srcTex, 0)) - vec2<i32>(1);\n"
"  let a = textureLoad(srcTex, min(px,                  dim), 0).r;\n"
"  let b = textureLoad(srcTex, min(px + vec2<i32>(1,0), dim), 0).r;\n"
"  let c = textureLoad(srcTex, min(px + vec2<i32>(0,1), dim), 0).r;\n"
"  let d = textureLoad(srcTex, min(px + vec2<i32>(1,1), dim), 0).r;\n"
"  return max(max(a, b), max(c, d));\n"
"}\n";

/* Deferred lighting. Reads the G-buffer, writes linear HDR radiance.
 *
 * The BRDF is deliberately the same one the forward pass uses. Anywhere
 * the two disagree is a bug in one of them, and keeping them identical is
 * what makes the forward frame a usable reference image while the deferred
 * frame is being built. */


/* ----- TAA on the deferred frame (#390) ------------------------------
 *
 * Two history buffers, ping-ponged: the resolve reads the one it did not
 * write last frame and writes the other, so a frame never reads and
 * writes the same texture. Persistent, not transient — the whole point is
 * that it survives to next frame, which is the lifetime class #331 put in
 * the graph v1 precisely so temporal techniques could be declared
 * truthfully.
 *
 * REPROJECTION USES THE G-BUFFER'S MOTION CHANNEL (#390 part 1), decoded
 * with the paired `raw * 2 - 256/255` — the encode and decode constants
 * must change together or every pixel reprojects to the wrong place.
 *
 * NEIGHBOURHOOD CLAMP is what makes it usable rather than a smear. The
 * history is clamped to the min/max of the 3x3 neighbourhood around the
 * current pixel, so a disoccluded pixel — one whose history belongs to
 * geometry no longer there — is pulled back to something plausible
 * instead of ghosting. Without it TAA trades aliasing for trailing, which
 * is a worse artefact.
 */
static WGPUTexture     gb_taa_tex[2] = {NULL, NULL};
static WGPUTextureView gb_taa_view[2] = {NULL, NULL};
static WGPURenderPipeline gb_taa_pipeline = NULL;
static WGPUBindGroup   gb_taa_bind[2] = {NULL, NULL};
static int  gb_taa_cur = 0;
static bool gb_taa_have_history = false;
/* Sub-pixel jitter (#390). WITHOUT IT TAA IS NOT ANTIALIASING: every
 * frame samples the same point inside each pixel, so accumulating them
 * adds nothing and only risks ghosting. The jitter is what makes
 * successive frames carry different information. */
static float gb_jitter_x = 0.0f, gb_jitter_y = 0.0f;
static int   gb_taa_frame = 0;
static bool  gb_taa_enabled = true;

static const char* GB_TAA_WGSL =
"@group(0) @binding(0) var litTex: texture_2d<f32>;\n"
"@group(0) @binding(1) var histTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var gbcTex: texture_2d<f32>;\n"
"@group(0) @binding(3) var<uniform> P: vec4<f32>;\n"   /* x = have history */
GB_FULLSCREEN_VS
"@fragment\n"
"fn fs(@builtin(position) fc: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let dim = vec2<i32>(textureDimensions(litTex));\n"
"  let px = vec2<i32>(fc.xy);\n"
"  let cur = textureLoad(litTex, px, 0).rgb;\n"
"  if (P.x < 0.5) { return vec4<f32>(cur, 1.0); }\n"
/* Paired with the G-buffer's encode: bias 128/255, so the inverse is
 * raw*2 - 256/255. */
"  let mRaw = textureLoad(gbcTex, px, 0).xy;\n"
"  let motion = mRaw * 2.0 - vec2<f32>(256.0 / 255.0);\n"
"  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(dim);\n"
"  let prevUv = uv - motion;\n"
/* Off-screen history is no history. Using the clamped edge would smear
 * the border inward, which reads as a dirty frame edge. */
"  if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {\n"
"    return vec4<f32>(cur, 1.0);\n"
"  }\n"
"  let hp = vec2<i32>(prevUv * vec2<f32>(dim));\n"
"  var hist = textureLoad(histTex, hp, 0).rgb;\n"
/* Neighbourhood clamp — see the note above on why this is not optional. */
"  var lo = cur;\n"
"  var hi = cur;\n"
"  for (var dy = -1; dy <= 1; dy = dy + 1) {\n"
"    for (var dx = -1; dx <= 1; dx = dx + 1) {\n"
"      let q = clamp(px + vec2<i32>(dx, dy), vec2<i32>(0), dim - vec2<i32>(1));\n"
"      let c = textureLoad(litTex, q, 0).rgb;\n"
"      lo = min(lo, c);\n"
"      hi = max(hi, c);\n"
"    }\n"
"  }\n"
"  hist = clamp(hist, lo, hi);\n"
/* 0.9 history is the usual compromise: enough frames to converge the
 * jitter, few enough that a clamped-but-wrong sample washes out fast. */
"  return vec4<f32>(mix(cur, hist, 0.9), 1.0);\n"
"}\n";

/* ----- SSAO on the deferred frame (#387) -----------------------------
 *
 * The forward frame computed AO in runtime_gpu3d_ssao.c against ITS depth
 * and normal targets and wrote into its separate ambient attachment. The
 * G-buffer has no ambient attachment — lighting computes the indirect term
 * itself — so the deferred version produces an AO texture that the
 * lighting pass samples and applies to the INDIRECT term only.
 *
 * That invariant (docs/shadow-system-design.md §5) is why AO is a separate
 * texture rather than something folded into radiance: once it is mixed
 * into a single lit value there is no way to keep it off direct light, and
 * no way to shrink its influence when GI lands.
 *
 * HORIZON SEARCH, not hemisphere sampling — the same choice #337 made and
 * for the same reason: a horizon search integrates a whole angular sector
 * per sample instead of testing one direction, so few samples still
 * produce smooth occlusion.
 *
 * FULL RESOLUTION for now. AO is low-frequency and belongs at half res
 * with a depth-aware upsample (the forward path does this), but that is a
 * second pass and a sampler; correctness first, cost second. Half-res is a
 * tier knob and is tracked rather than pretended at.
 */
static WGPUTexture     gb_ao_tex = NULL;
static WGPUTextureView gb_ao_view = NULL;
static WGPURenderPipeline gb_ao_pipeline = NULL;
static WGPUBindGroup   gb_ao_bind = NULL;

static const char* GB_AO_WGSL =
"struct LightU {\n"
"  invViewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"
"  sunDir: vec4<f32>,\n"
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"  clearColor: vec4<f32>,\n"
"  viewProj: mat4x4<f32>,\n"
"  skyParams: vec4<f32>,\n"   /* x kind, y turbidity, z exposure, w sun angular size */
"  skyZenith: vec4<f32>,\n"   /* stylised zenith colour, w = band sharpness */
"  skyHorizon: vec4<f32>,\n"
/* Cooked Hosek-Wilkie state: 3 channels x (9 coefficients + radiance)
 * = 30 floats in 9 vec4 (three per channel, last one half used). Packed by
 * gb_pack_hosek on the CPU from the SAME memoized cook lib/sky.rae
 * calls, so the CPU irradiance and the GPU background cannot disagree. */
"  hosek: array<vec4<f32>, 9>,\n"  /* stylised horizon colour, w = sun disc intensity */
"};\n"
"@group(0) @binding(0) var<uniform> L: LightU;\n"
"@group(0) @binding(1) var gbaTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var depthTex: texture_depth_2d;\n"
GB_OCT_WGSL
GB_FULLSCREEN_VS
/* Contact scale, matching #337: a large radius would be obsoleted by real
 * world-space GI and would double-darken against it. */
"const AO_RADIUS: f32 = 1.2;\n"
"const AO_SLICES: i32 = 3;\n"
"const AO_STEPS: i32 = 6;\n"
/* A SPHERE IS FACETED IN THE DEPTH BUFFER BUT SMOOTH IN THE NORMAL
 * BUFFER. Normals are interpolated across each triangle, while neighbour
 * positions are reconstructed from depth and so lie on flat facets. Points
 * on the next facet therefore sit fractionally above the tangent plane and
 * register as occluders, drawing a dark seam along every triangle edge —
 * the square grid visible on the spheres. Ignoring horizons this shallow
 * costs nothing real: a true occluder at grazing elevation contributes
 * almost no occlusion anyway. */
"const AO_BIAS: f32 = 0.06;\n"
"fn ign(pix: vec2<f32>) -> f32 {\n"
"  return fract(52.9829189 * fract(dot(pix, vec2<f32>(0.06711056, 0.00583715))));\n"
"}\n"
/* World position from reverse-Z depth, the same reconstruction the
 * lighting pass uses — two different reconstructions of one quantity is a
 * bug waiting to happen. */
"fn worldAt(px: vec2<i32>, dim: vec2<i32>) -> vec3<f32> {\n"
"  let d = textureLoad(depthTex, px, 0);\n"
"  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(dim);\n"
"  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
"  let w = L.invViewProj * ndc;\n"
"  return w.xyz / w.w;\n"
"}\n"
/* World position to screen UV, for measuring the radius in pixels. */
"fn Pclip(w: vec3<f32>) -> vec2<f32> {\n"
"  let c = L.viewProj * vec4<f32>(w, 1.0);\n"
"  let n = c.xy / max(c.w, 1e-4);\n"
"  return vec2<f32>(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) fc: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let dim = vec2<i32>(textureDimensions(depthTex));\n"
"  let px = vec2<i32>(fc.xy);\n"
"  let d = textureLoad(depthTex, px, 0);\n"
/* Reverse-Z: 0 is the far plane, so an untouched pixel is sky. Shading it
 * would darken the background, which AO must never do. */
"  if (d <= 0.0) { return vec4<f32>(1.0, 1.0, 1.0, 1.0); }\n"
"  let P = worldAt(px, dim);\n"
"  let gba = textureLoad(gbaTex, px, 0);\n"
"  let N = octDecode(gba.xy);\n"
"  let noise = ign(fc.xy);\n"
/* SCREEN-SPACE MARCH LENGTH FROM THE WORLD RADIUS, not a fixed pixel
 * count. A fixed count ignores distance and field of view, so the
 * darkened region stays the same size on screen however close the
 * surface is — which reads as a small hard patch rather than contact
 * shading. Project a point one radius to the side and measure how far
 * it moved in pixels; that is the same derivation the forward pass
 * uses. */
"  let side = P + vec3<f32>(AO_RADIUS, 0.0, 0.0);\n"
"  let sideClip = Pclip(side);\n"
"  let pClip = Pclip(P);\n"
"  var marchPx = length((sideClip - pClip) * vec2<f32>(dim)) * 0.5;\n"
"  marchPx = clamp(marchPx, 4.0, 96.0);\n"
"  let stepPxLen = marchPx / f32(AO_STEPS);\n"
"  var occl = 0.0;\n"
"  for (var s = 0; s < AO_SLICES; s = s + 1) {\n"
"    let ang = (f32(s) + noise) * 3.14159265 / f32(AO_SLICES);\n"
"    let dir = vec2<f32>(cos(ang), sin(ang));\n"
"    var maxSin = 0.0;\n"
/* JITTER THE MARCH RADIALLY, not just the slice angle. Without this every
 * pixel samples the same six radii, so the pixels whose occluder first
 * falls in step 3 form a contour — which is exactly the ring banding seen
 * on spheres, the torus and metaballs. Offsetting each pixel's whole
 * march by a sub-step amount turns that contour into noise the filter
 * below averages away. The offset is per-slice so the three slices do not
 * band together. */
"    let radialJitter = fract(noise + f32(s) * 0.6180339887);\n"
"    for (var t = 1; t <= AO_STEPS; t = t + 1) {\n"
"      let stepPx = dir * ((f32(t) - 1.0 + radialJitter) * stepPxLen);\n"
"      let sp = px + vec2<i32>(stepPx);\n"
"      if (sp.x < 0 || sp.y < 0 || sp.x >= dim.x || sp.y >= dim.y) { break; }\n"
"      let sd = textureLoad(depthTex, sp, 0);\n"
"      if (sd <= 0.0) { continue; }\n"
"      let S = worldAt(sp, dim);\n"
"      let v = S - P;\n"
"      let dist = length(v);\n"
"      if (dist < 0.001 || dist > AO_RADIUS) { continue; }\n"
/* The horizon angle above the tangent plane. sin(elevation) = N.v/|v|,
 * and tracking its MAXIMUM along the march is the horizon search: one
 * value summarises every occluder in this direction. */
"      let sinE = dot(N, v) / dist - AO_BIAS;\n"
/* Attenuate with distance so a far occluder does not count as much as a
 * touching one; without it AO reads as a hard disc at the radius. */
"      let falloff = 1.0 - (dist / AO_RADIUS) * (dist / AO_RADIUS);\n"
"      maxSin = max(maxSin, sinE * falloff);\n"
"    }\n"
"    occl = occl + clamp(maxSin, 0.0, 1.0);\n"
"  }\n"
"  let ao = clamp(1.0 - occl / f32(AO_SLICES), 0.0, 1.0);\n"
"  return vec4<f32>(ao, ao, ao, 1.0);\n"
"}\n";

static const char* GB_LIGHT_WGSL =
"struct LightU {\n"
"  invViewProj: mat4x4<f32>,\n"
"  camPos: vec4<f32>,\n"      /* xyz cam, w exposure (used by composite) */
"  sunDir: vec4<f32>,\n"      /* xyz direction TOWARD the scene */
"  sunColor: vec4<f32>,\n"
"  ambSky: vec4<f32>,\n"
"  ambGround: vec4<f32>,\n"
"  clearColor: vec4<f32>,\n"
/* viewProj is not read here, but it IS in the buffer between clearColor
 * and the sky block, so it must be declared or every sky field below
 * would be read from the wrong offset — a silent desync that shows up as
 * a sky coloured by whatever happened to be in the matrix. */
"  viewProj: mat4x4<f32>,\n"
"  skyParams: vec4<f32>,\n"
"  skyZenith: vec4<f32>,\n"
"  skyHorizon: vec4<f32>,\n"
/* Cooked Hosek-Wilkie state: 3 channels x (9 coefficients + radiance)
 * = 30 floats in 9 vec4 (three per channel, last one half used). Packed by
 * gb_pack_hosek on the CPU from the SAME memoized cook lib/sky.rae
 * calls, so the CPU irradiance and the GPU background cannot disagree. */
"  hosek: array<vec4<f32>, 9>,\n"
"};\n"
"@group(0) @binding(0) var<uniform> L: LightU;\n"
"@group(0) @binding(1) var gbaTex: texture_2d<f32>;\n"
"@group(0) @binding(2) var gbbTex: texture_2d<f32>;\n"
"@group(0) @binding(3) var gbcTex: texture_2d<f32>;\n"
"@group(0) @binding(4) var depthTex: texture_depth_2d;\n"
"@group(0) @binding(5) var aoTex: texture_2d<f32>;\n"
/* Sun shadows (#382/#384) reach the deferred frame through the SAME
 * shared lookup the forward and skinned shaders use — one definition,
 * three callers, so the three cannot disagree about where a shadow
 * falls. Only the binding numbers differ. */
"struct ShadowU {\n"
"  lightViewProj: array<mat4x4<f32>, 4>,\n"
"  splitFar: vec4<f32>,\n"
"  texelWorld: vec4<f32>,\n"
"  depthRange: vec4<f32>,\n"
"  shadowCfg: vec4<f32>,\n"
"};\n"
"@group(0) @binding(6) var<uniform> SH: ShadowU;\n"
"@group(0) @binding(7) var shadowTex: texture_depth_2d_array;\n"
"@group(0) @binding(8) var shadowSamp: sampler_comparison;\n"
GB_OCT_WGSL
GB_FULLSCREEN_VS
G3D_SHADOW_FN_WGSL
"const PI: f32 = 3.14159265;\n"
/* ---- sky (#400 procedural, #404 stylised) --------------------------
 * The SAME model as lib/sky.rae, evaluated per pixel. Two copies of a
 * formula is a real cost, and it is paid deliberately: the CPU one
 * answers "what ambient does this scene get" once per frame, this one
 * answers "what colour is this pixel of sky", and a round trip through a
 * texture to share them would cost more than the duplication. Test 584
 * pins the CPU one; the relationships it asserts are what this must also
 * show, and a divergence is visible as a background that does not match
 * the lighting.
 */
"fn preethamF(A: f32, B: f32, C: f32, D: f32, E: f32, ct: f32, g: f32) -> f32 {\n"
/* Clamped away from zero: the model diverges at the horizon, and an
 * infinity here reaches the tonemapper and takes the frame with it. */
"  let c = max(ct, 0.01);\n"
"  let cg = cos(g);\n"
"  return (1.0 + A * exp(B / c)) * (1.0 + C * exp(D * g) + E * cg * cg);\n"
"}\n"
"fn skyProcedural(dir: vec3<f32>, toSun: vec3<f32>, t: f32) -> vec3<f32> {\n"
"  let g = acos(clamp(dot(dir, toSun), -1.0, 1.0));\n"
"  let ts = acos(clamp(max(toSun.z, 0.01), -1.0, 1.0));\n"
"  let Ay = 0.1787 * t - 1.4630; let By = -0.3554 * t + 0.4275;\n"
"  let Cy = -0.0227 * t + 5.3251; let Dy = 0.1206 * t - 2.5771;\n"
"  let Ey = -0.0670 * t + 0.3703;\n"
"  let Ax = -0.0193 * t - 0.2592; let Bx = -0.0665 * t + 0.0008;\n"
"  let Cx = -0.0004 * t + 0.2125; let Dx = -0.0641 * t - 0.8989;\n"
"  let Ex = -0.0033 * t + 0.0452;\n"
"  let Az = -0.0167 * t - 0.2608; let Bz = -0.0950 * t + 0.0092;\n"
"  let Cz = -0.0079 * t + 0.2102; let Dz = -0.0441 * t - 1.6537;\n"
"  let Ez = -0.0109 * t + 0.0529;\n"
"  let fy = preethamF(Ay,By,Cy,Dy,Ey, dir.z, g) / preethamF(Ay,By,Cy,Dy,Ey, 1.0, ts);\n"
"  let fx = preethamF(Ax,Bx,Cx,Dx,Ex, dir.z, g) / preethamF(Ax,Bx,Cx,Dx,Ex, 1.0, ts);\n"
"  let fz = preethamF(Az,Bz,Cz,Dz,Ez, dir.z, g) / preethamF(Az,Bz,Cz,Dz,Ez, 1.0, ts);\n"
"  let ts2 = ts * ts; let ts3 = ts2 * ts;\n"
"  let chi = (4.0 / 9.0 - t / 120.0) * (PI - 2.0 * ts);\n"
"  let zY = max((4.0453 * t - 4.9710) * tan(chi) - 0.2155 * t + 2.4192, 0.0);\n"
"  let zx = (0.00166*ts3 - 0.00375*ts2 + 0.00209*ts) * t * t\n"
"         + (-0.02903*ts3 + 0.06377*ts2 - 0.03202*ts + 0.00394) * t\n"
"         + (0.11693*ts3 - 0.21196*ts2 + 0.06052*ts + 0.25886);\n"
"  let zy = (0.00275*ts3 - 0.00610*ts2 + 0.00317*ts) * t * t\n"
"         + (-0.04214*ts3 + 0.08970*ts2 - 0.04153*ts + 0.00516) * t\n"
"         + (0.15346*ts3 - 0.26756*ts2 + 0.06670*ts + 0.26688);\n"
"  let xx = zx * fx; let yy = max(zy * fz, 0.0001); let YY = zY * fy * 0.05;\n"
"  let X = xx * YY / yy; let Z = (1.0 - xx - yy) * YY / yy;\n"
"  let rgb = vec3<f32>( 3.2406*X - 1.5372*YY - 0.4986*Z,\n"
"                      -0.9689*X + 1.8758*YY + 0.0415*Z,\n"
"                       0.0557*X - 0.2040*YY + 1.0570*Z);\n"
"  return max(rgb, vec3<f32>(0.0));\n"
"}\n"
/* STYLISED: a vertical ramp between two authored colours, quantised the
 * same way the toon terminator is. Bands rather than a smooth gradient is
 * the point — a cel scene under a photographic gradient reads as models
 * pasted onto a photograph, which is the failure §3C of the design doc
 * exists to prevent. The colours come from the scene's palette, so the
 * sky and the character's shadow tint agree by construction. */
"fn skyStylised(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {\n"
"  let h = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);\n"
"  let bands = max(L.skyZenith.w, 1.0);\n"
"  let s = h * bands;\n"
"  let i = floor(s);\n"
"  let band = (i + smoothstep(0.35, 0.65, s - i)) / bands;\n"
"  var c = mix(L.skyHorizon.rgb, L.skyZenith.rgb, band);\n"
/* A warm glow toward the sun, kept broad and soft so it reads as painted
 * light rather than as a lens artefact. */
"  let g = clamp(dot(dir, toSun), 0.0, 1.0);\n"
"  c = c + L.sunColor.rgb * pow(g, 6.0) * 0.35;\n"
"  return c;\n"
"}\n"
/* HOSEK-WILKIE: the analytic formula, nine coefficients per channel, cooked
 * on the CPU from the fitted dataset (third_party/hosek_wilkie). Preetham
 * above is kept as the table-free fallback; this is the better fit near the
 * horizon and at low sun, and it is the only one of the two that responds to
 * ground albedo. */
"fn hosekChannel(base: i32, cosTheta: f32, cosGamma: f32, gamma: f32) -> f32 {\n"
"  let c0 = L.hosek[base];\n"
"  let c1 = L.hosek[base + 1];\n"
"  let c2 = L.hosek[base + 2];\n"
"  let expM = exp(c1.x * gamma);\n"
"  let rayM = cosGamma * cosGamma;\n"
"  let mieDen = 1.0 + c2.x * c2.x - 2.0 * c2.x * cosGamma;\n"
"  var mieM = 0.0;\n"
"  if (mieDen > 0.0) { mieM = (1.0 + rayM) / (mieDen * sqrt(mieDen)); }\n"
"  let zen = sqrt(max(cosTheta, 0.0));\n"
/* The +0.01 is the model's own horizon guard: cos(theta) reaches 0 there
 * and the widening term would divide by zero. */
"  let widening = 1.0 + c0.x * exp(c0.y / (cosTheta + 0.01));\n"
"  let body = c0.z + c0.w * expM + c1.y * rayM + c1.z * mieM + c1.w * zen;\n"
"  return widening * body;\n"
"}\n"
"fn skyHosek(dir: vec3<f32>, toSun: vec3<f32>) -> vec3<f32> {\n"
"  let cosTheta = clamp(dir.z, -1.0, 1.0);\n"
"  let cosGamma = clamp(dot(dir, toSun), -1.0, 1.0);\n"
"  let gamma = acos(cosGamma);\n"
"  let r = hosekChannel(0, cosTheta, cosGamma, gamma) * L.hosek[2].y;\n"
"  let g = hosekChannel(3, cosTheta, cosGamma, gamma) * L.hosek[5].y;\n"
"  let b = hosekChannel(6, cosTheta, cosGamma, gamma) * L.hosek[8].y;\n"
/* Below the horizon the fit is not defined; fade to the ground-ish horizon
 * colour rather than letting it diverge into the lower hemisphere. */
"  let below = smoothstep(0.0, 0.06, dir.z);\n"
"  return mix(L.skyHorizon.rgb, max(vec3<f32>(r, g, b), vec3<f32>(0.0)), below);\n"
"}\n"
"fn skyColor(dir: vec3<f32>) -> vec3<f32> {\n"
"  let kind = L.skyParams.x;\n"
"  let toSun = normalize(-L.sunDir.xyz);\n"
"  var c = L.clearColor.rgb;\n"
"  if (kind > 1.5 && kind < 2.5) { c = skyProcedural(dir, toSun, L.skyParams.y); }\n"
"  if (kind > 2.5 && kind < 3.5) { c = skyStylised(dir, toSun); }\n"
"  if (kind > 3.5) { c = skyHosek(dir, toSun); }\n"
/* The sun DISC, added on top for every kind that has a sky. Separate
 * from the models above because it is ~1e5 times their radiance: folding
 * it in would make the whole expression's dynamic range about the disc.
 * A hard edge with one smoothstep of falloff, so it survives TAA. */
"  if (kind > 1.5) {\n"
"    let ca = dot(dir, toSun);\n"
"    let cutoff = cos(max(L.skyParams.w, 0.001));\n"
"    let disc = smoothstep(cutoff, mix(cutoff, 1.0, 0.35), ca);\n"
"    c = c + L.sunColor.rgb * disc * L.skyHorizon.w;\n"
"  }\n"
"  return c * L.skyParams.z;\n"
"}\n"
/* Quantise [0,1] into `bands` steps with a SOFTENED edge (#396). A bare
 * floor() aliases along the terminator, and that edge moves whenever the
 * light or the object does, so it crawls — the one artefact a stylised
 * look cannot hide. Smoothstepping across the step keeps the band reading
 * flat while giving the boundary a pixel or two to resolve in. */
"fn toonBand(x: f32, bands: f32) -> f32 {\n"
"  let s = clamp(x, 0.0, 1.0) * bands;\n"
"  let i = floor(s);\n"
"  let f = s - i;\n"
"  return (i + smoothstep(0.4, 0.6, f)) / bands;\n"
"}\n"
"fn dGGX(NoH: f32, rough: f32) -> f32 {\n"
"  let a = rough * rough;\n"
"  let a2 = a * a;\n"
"  let d = NoH * NoH * (a2 - 1.0) + 1.0;\n"
"  return a2 / (PI * d * d + 1e-5);\n"
"}\n"
"fn gSmith(NoV: f32, NoL: f32, rough: f32) -> f32 {\n"
"  let k = (rough + 1.0) * (rough + 1.0) / 8.0;\n"
"  let gv = NoV / (NoV * (1.0 - k) + k);\n"
"  let gl = NoL / (NoL * (1.0 - k) + k);\n"
"  return gv * gl;\n"
"}\n"
"fn fresnel(VoH: f32, f0: vec3<f32>) -> vec3<f32> {\n"
"  return f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - VoH, 5.0);\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let px = vec2<i32>(pos.xy);\n"
"  let depth = textureLoad(depthTex, px, 0);\n"
/* REVERSE-Z (#367): the far plane is 0, not 1. Depth still at 0 means the
 * geometry pass wrote nothing here. Reconstructing a position from it
 * would place the pixel at infinity and light it as if it were a surface;
 * the background is not a surface, so it passes through as the clear
 * colour. Getting this comparison the wrong way round after the reverse-Z
 * switch would light the sky and discard the scene. */
"  if (depth <= 0.0) {\n"
/* THE BACKGROUND IS THE SKY (#400). Reconstructed from the same
 * invViewProj the rest of this shader uses, at the far plane — reverse-Z,
 * so far is z = 0. Deriving the ray from a separately-uploaded camera
 * basis would let the background drift a frame away from the geometry
 * during camera motion, which reads as the world sliding against the sky.
 */
"    let uvF = (vec2<f32>(px) + vec2<f32>(0.5)) / vec2<f32>(textureDimensions(gbbTex, 0));\n"
"    let ndcF = vec4<f32>(uvF.x * 2.0 - 1.0, 1.0 - uvF.y * 2.0, 0.0, 1.0);\n"
"    let farW = L.invViewProj * ndcF;\n"
"    let dirF = normalize(farW.xyz / farW.w - L.camPos.xyz);\n"
"    return vec4<f32>(skyColor(dirF), 1.0);\n"
"  }\n"
"  let gbb = textureLoad(gbbTex, px, 0);\n"
"  let albedo = gbb.rgb;\n"
"  let gba = textureLoad(gbaTex, px, 0);\n"
"  let mode = gba.w;\n"
/* The four modes are the quantisation points of 2 bits — 0, 1/3, 2/3, 1 —
 * so each is tested as a BAND rather than with an open-ended comparison.
 * Adding toon at 1.0 (#396) is exactly why: the old `mode > 0.5` for
 * unlit and `mode > 0.0` for emissive would both have swallowed it, and a
 * toon surface would have rendered as flat albedo with its occlusion
 * channel decoded as an emissive intensity. */
"  let isEmissive = mode > 0.16 && mode < 0.5;\n"
"  let isUnlit = mode > 0.5 && mode < 0.83;\n"
"  let isToon = mode > 0.83;\n"
/* Unlit surfaces skip every calculation below and emit albedo directly.
 * This early-out is a real part of why the mode field earns its 2 bits. */
"  if (isUnlit) {\n"
"    return vec4<f32>(albedo, 1.0);\n"
"  }\n"
"  let dim = vec2<f32>(textureDimensions(gbbTex, 0));\n"
"  let uv = (vec2<f32>(px) + vec2<f32>(0.5)) / dim;\n"
/* WebGPU clip space: xy in [-1,1] with +y UP, z in [0,1]. UV runs +y DOWN,
 * hence the flip. Getting this backwards mirrors the lighting vertically,
 * which reads as "the sun is in the wrong place" rather than as a
 * reconstruction bug. The depth value goes in as-is: it is inverted by the
 * projection matrix, and invViewProj is that same matrix's inverse. */
"  let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);\n"
"  let wpos4 = L.invViewProj * ndc;\n"
"  let wpos = wpos4.xyz / wpos4.w;\n"
"  let N = octDecode(gba.xy);\n"
"  let gbc = textureLoad(gbcTex, px, 0);\n"
"  let metallic = clamp(gbc.z, 0.0, 1.0);\n"
"  let rough = clamp(gbb.a, 0.045, 1.0);\n"
/* C.w is occlusion for a lit surface and log-encoded emissive for an
 * emitter — the mode decides which, so an emitter is never also occluded
 * and an occluded surface never also glows. */
"  var ao = gbc.w;\n"
"  var emissive = 0.0;\n"
"  if (isEmissive) {\n"
"    emissive = exp(ao * 6.91) - 1.0;\n"
"    ao = 1.0;\n"
"  }\n"
"  let V = normalize(L.camPos.xyz - wpos);\n"
"  let Ldir = normalize(-L.sunDir.xyz);\n"
"  let H = normalize(V + Ldir);\n"
"  let NoV = max(dot(N, V), 1e-4);\n"
"  let NoL = max(dot(N, Ldir), 0.0);\n"
"  let NoH = max(dot(N, H), 0.0);\n"
"  let VoH = max(dot(V, H), 0.0);\n"
"  let f0 = mix(vec3<f32>(0.04), albedo, metallic);\n"
"  let Fs = fresnel(VoH, f0);\n"
"  let spec = dGGX(NoH, rough) * gSmith(NoV, NoL, rough) * Fs\n"
"           / max(4.0 * NoV * NoL, 1e-4);\n"
"  let kd = (vec3<f32>(1.0) - Fs) * (1.0 - metallic);\n"
/* Shadow multiplies DIRECT only — never the ambient below. Same §5
 * invariant the forward path enforces, and the deferred frame keeps the
 * two terms separate anyway, so it is structural here too. */
"  let sunVis = sunVisibility(wpos, N, length(L.camPos.xyz - wpos), vec2<f32>(px));\n"
"  let direct = (kd * albedo / PI + spec) * L.sunColor.rgb * NoL * sunVis;\n"

/* Z-up world (docs/coordinate-system.md), so the hemisphere blends on N.z.
 * The forward pass blends on N.y because it predates the convention — that
 * disagreement is real and tracked, and this is the correct one. */
"  let hemi = mix(L.ambGround.rgb, L.ambSky.rgb, N.z * 0.5 + 0.5);\n"
"  let ambF = fresnel(NoV, f0);\n"
/* TOON (#396), rebuilt to match how stylised shaders actually work.
 *
 * The first attempt quantised the PBR result — floor(N.L * bands) times
 * albedo, plus a thresholded GGX highlight and a rim. That produces
 * "banded PBR", not cel shading, and it reads as wrong for reasons worth
 * writing down, because they are the whole technique:
 *
 *   1. A CEL SHADOW IS A COLOUR, NOT A DARKER ALBEDO. The defining move
 *      is lerping between authored light and shadow TONES. Multiplying
 *      albedo by a fraction keeps the same hue at lower value, which is
 *      exactly the muddy look this replaces. Real cel shadows shift hue —
 *      here toward the sky ambient, so shadows read cool against warm
 *      light, which is what makes them look deliberate.
 *   2. ONE OR TWO BOUNDARIES, NOT N EVEN BANDS. Even quantisation gives
 *      a smooth gradient's staircase. Stylised shaders place a small
 *      number of thresholds explicitly, and the placement is the art.
 *   3. THE THRESHOLD RUNS ON HALF-LAMBERT, not raw N.L. Remapping to
 *      N.L * 0.5 + 0.5 spreads the terminator across the whole sphere,
 *      so a threshold has resolution to sit in and can be moved past the
 *      geometric terminator. On raw N.L everything below zero is one
 *      flat clamp and half the control range does nothing.
 *   4. NO SPECULAR OR RIM BY DEFAULT. Both are opt-in extras in the
 *      shaders this follows, and the reference material this was checked
 *      against had both switched OFF. Adding them unasked is most of why
 *      the first attempt looked busy.
 */
"  let hl = dot(N, Ldir) * 0.5 + 0.5;\n"
/* Two thresholds: a hard terminator, and an earlier softer one that adds
 * a mid-tone so the result is three tones rather than a hard two. */
"  let toonT1 = smoothstep(0.0, 0.03, clamp(hl - 0.50, 0.0, 1.0));\n"
"  let toonT2 = smoothstep(0.0, 0.07, clamp(hl - 0.62, 0.0, 1.0));\n"
/* The shadow tint. Pushing toward the sky ambient rather than to grey is
 * what stops the shadow reading as "the same colour with the brightness
 * turned down". */
"  let shadeTint = mix(vec3<f32>(1.0), normalize(max(L.ambSky.rgb, vec3<f32>(0.001))) * 1.732, 0.5);\n"
"  let toonLit = albedo;\n"
"  let toonMid = albedo * 0.62 * shadeTint;\n"
"  let toonDeep = albedo * 0.34 * shadeTint;\n"
/* Applied mid FIRST and deep second, so all three tones survive. Doing it
 * the other way round lets the second lerp overwrite the first in deep
 * shadow and collapses the result back to two tones. */
"  var toonCol = mix(toonMid, toonLit, toonT2);\n"
"  toonCol = mix(toonDeep, toonCol, toonT1);\n"
/* The cast shadow stays a CONTINUOUS multiplier (§5): it pushes toward
 * the deep tone rather than being quantised into the bands, so the soft
 * penumbra survives instead of being staircased. Not all the way to the
 * deep tone, or a shadowed object goes featureless. */
"  toonCol = mix(toonDeep, toonCol, mix(1.0, sunVis, 0.85));\n"
/* Sit in the same exposure range as the PBR path, so toggling the style
 * does not also change the apparent brightness of the scene. */
"  let toonLight = L.sunColor.rgb * 0.34 + hemi * 0.75;\n"
"  let toonShaded = toonCol * toonLight;\n"
/* Screen-space AO multiplies the baked occlusion, and the product
   touches INDIRECT only — never `direct` above. */
/* DEPTH-AWARE 3x3 AVERAGE. The AO pass rotates its sampling per pixel
 * with interleaved gradient noise, which is what stops banding — but raw,
 * that noise reads as a halftone screen. The forward path dissolves it in
 * its bilateral upsample; this frame renders AO at full resolution and so
 * has no upsample to hide behind, and needs the filter explicitly.
 *
 * Weighted by DEPTH similarity, not distance alone: a flat blur would
 * pull occlusion across silhouettes and darken the background behind an
 * object's edge. */
"  var aoSum = 0.0;\n"
"  var aoW = 0.0;\n"
"  let dims = vec2<i32>(textureDimensions(aoTex));\n"
"  for (var dy = -1; dy <= 1; dy = dy + 1) {\n"
"    for (var dx = -1; dx <= 1; dx = dx + 1) {\n"
"      let q = clamp(px + vec2<i32>(dx, dy), vec2<i32>(0), dims - vec2<i32>(1));\n"
"      let qd = textureLoad(depthTex, q, 0);\n"
"      if (qd <= 0.0) { continue; }\n"
"      let w = 1.0 / (1.0 + abs(qd - depth) * 800.0);\n"
"      aoSum = aoSum + textureLoad(aoTex, q, 0).r * w;\n"
"      aoW = aoW + w;\n"
"    }\n"
"  }\n"
"  var ssao = 1.0;\n"
"  if (aoW > 0.0) { ssao = aoSum / aoW; }\n"
"  let ambient = (hemi * albedo * (1.0 - metallic) + hemi * ambF * (1.0 - rough * 0.7)) * ao * ssao;\n"
/* Emissive tints by albedo, matching how it is authored: an emitter's base
 * colour IS the colour it emits, which is why one scalar suffices. */
"  let emit = albedo * emissive;\n"
/* LINEAR HDR out. Exposure, ACES and gamma belong to the composite, so a
 * later SSAO/GI pass can attenuate radiance rather than tonemapped values. */
/* Clamp before writing. rg11b10ufloat (#370) is UNSIGNED with a limited
 * exponent range, so a negative value from a stray term or an unbounded
 * emitter does not merely lose precision — it becomes a NaN or a huge
 * positive, and one such pixel survives every subsequent blur and bloom.
 * The bound is the format's, applied even on the rgba16float fallback so
 * both paths produce the same image. */
"  var lit = direct + ambient + emit;\n"
/* Occlusion still applies to the stylised result — a cel character with
 * no contact darkening floats. */
"  if (isToon) { lit = toonShaded * mix(1.0, ao * ssao, 0.6); }\n"
"  let radiance = clamp(lit, vec3<f32>(0.0), vec3<f32>(64000.0));\n"
"  return vec4<f32>(radiance, 1.0);\n"
"}\n";

/* Composite: linear HDR radiance -> the presentable LDR offscreen. */
static const char* GB_COMPOSITE_WGSL =
"@group(0) @binding(0) var<uniform> P: vec4<f32>;\n"   /* x = exposure */
"@group(0) @binding(1) var litTex: texture_2d<f32>;\n"
GB_FULLSCREEN_VS
"fn aces(x: vec3<f32>) -> vec3<f32> {\n"
"  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),\n"
"               vec3<f32>(0.0), vec3<f32>(1.0));\n"
"}\n"
"@fragment\n"
"fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
"  let hdr = textureLoad(litTex, vec2<i32>(pos.xy), 0).rgb;\n"
"  var c = aces(hdr * P.x);\n"
"  c = pow(c, vec3<f32>(1.0 / 2.2));\n"
"  return vec4<f32>(c, 1.0);\n"
"}\n";

/* Build a fullscreen pipeline: no vertex buffers, no depth, one target. */
static WGPURenderPipeline gb_make_fullscreen_pipeline(const char* wgsl, WGPUTextureFormat fmt,
                                                      const char* label) {
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(wgsl);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = fmt; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;
    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    if (!p) fprintf(stderr, "[deferred] %s pipeline creation FAILED\n", label);
    return p;
}

static void gb_deferred_release_targets(void) {
    for (int i = 0; i < GB_PYRAMID_MAX_MIPS; i++) {
        if (gb_pyr_bind[i])    { wgpuBindGroupRelease(gb_pyr_bind[i]); gb_pyr_bind[i] = NULL; }
        if (gb_pyramid_rt[i])  { wgpuTextureViewRelease(gb_pyramid_rt[i]); gb_pyramid_rt[i] = NULL; }
        if (gb_pyramid_src[i]) { wgpuTextureViewRelease(gb_pyramid_src[i]); gb_pyramid_src[i] = NULL; }
    }
    if (gb_pyramid_tex)    { wgpuTextureRelease(gb_pyramid_tex); gb_pyramid_tex = NULL; }
    if (gb_light_bind)     { wgpuBindGroupRelease(gb_light_bind); gb_light_bind = NULL; }
    for (int i = 0; i < 2; i++) {
        if (gb_composite_bind[i]) { wgpuBindGroupRelease(gb_composite_bind[i]); gb_composite_bind[i] = NULL; }
    }
    for (int i = 0; i < 2; i++) {
        if (gb_taa_view[i]) { wgpuTextureViewRelease(gb_taa_view[i]); gb_taa_view[i] = NULL; }
        if (gb_taa_tex[i])  { wgpuTextureRelease(gb_taa_tex[i]); gb_taa_tex[i] = NULL; }
        if (gb_taa_bind[i]) { wgpuBindGroupRelease(gb_taa_bind[i]); gb_taa_bind[i] = NULL; }
    }
    gb_taa_have_history = false;
    if (gb_ao_view)        { wgpuTextureViewRelease(gb_ao_view); gb_ao_view = NULL; }
    if (gb_ao_tex)         { wgpuTextureRelease(gb_ao_tex); gb_ao_tex = NULL; }
    if (gb_ao_bind)        { wgpuBindGroupRelease(gb_ao_bind); gb_ao_bind = NULL; }
    if (gb_lit_view)       { wgpuTextureViewRelease(gb_lit_view); gb_lit_view = NULL; }
    if (gb_lit_tex)        { wgpuTextureRelease(gb_lit_tex); gb_lit_tex = NULL; }
    gb_pyramid_mips = 0;
}

/* Recreate everything downstream of the G-buffer whenever the G-buffer
 * itself was recreated. Keyed on gb_targets_gen rather than on dimensions:
 * a resize back to a previous size still invalidates the bind groups, and
 * comparing sizes would miss that. */
static void gb_deferred_ensure(void) {
    if (gb_deferred_gen == gb_targets_gen && gb_pyramid_tex && gb_lit_tex) return;
    if (gb_target_w <= 0 || gb_target_h <= 0) return;
    gb_deferred_release_targets();

    int pw = gb_target_w / 2; if (pw < 1) pw = 1;
    int ph = gb_target_h / 2; if (ph < 1) ph = 1;
    int mips = 1;
    {
        int m = pw > ph ? pw : ph;
        while (m > 1 && mips < GB_PYRAMID_MAX_MIPS) { m /= 2; mips++; }
    }

    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)pw; td.size.height = (uint32_t)ph; td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = (uint32_t)mips; td.sampleCount = 1;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    td.format = WGPUTextureFormat_R32Float;
    gb_pyramid_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    for (int i = 0; i < mips; i++) {
        WGPUTextureViewDescriptor vd; memset(&vd, 0, sizeof(vd));
        vd.format = WGPUTextureFormat_R32Float;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = (uint32_t)i; vd.mipLevelCount = 1;
        vd.baseArrayLayer = 0; vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_All;
        gb_pyramid_rt[i]  = wgpuTextureCreateView(gb_pyramid_tex, &vd);
        gb_pyramid_src[i] = wgpuTextureCreateView(gb_pyramid_tex, &vd);
    }
    gb_pyramid_mips = mips;
    gb_pyramid_w = pw; gb_pyramid_h = ph;

    memset(&td, 0, sizeof(td));
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)gb_target_w; td.size.height = (uint32_t)gb_target_h;
    td.size.depthOrArrayLayers = 1;
    td.mipLevelCount = 1; td.sampleCount = 1;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    td.format = gb_lit_format;
    gb_lit_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    gb_lit_view = wgpuTextureCreateView(gb_lit_tex, NULL);

    /* AO target (#387). Full resolution and r8unorm: one byte per pixel,
     * and a fragment pass can write r8unorm as a colour attachment even
     * though it is not a writable STORAGE format. */
    td.size.width = (uint32_t)gb_target_w;
    td.size.height = (uint32_t)gb_target_h;
    td.mipLevelCount = 1;
    td.format = WGPUTextureFormat_R8Unorm;
    gb_ao_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    gb_ao_view = wgpuTextureCreateView(gb_ao_tex, NULL);

    /* TAA history, same format as radiance: the resolve accumulates
     * LINEAR HDR, before the composite's tone curve. Accumulating
     * tonemapped values would make convergence depend on exposure. */
    td.format = gb_lit_format;
    for (int i = 0; i < 2; i++) {
        gb_taa_tex[i] = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
        gb_taa_view[i] = wgpuTextureCreateView(gb_taa_tex[i], NULL);
    }
    gb_taa_have_history = false;

    gb_deferred_gen = gb_targets_gen;
}

static void gb_deferred_init_pipelines(void) {
    if (!gb_pyr_from_depth_pipeline)
        gb_pyr_from_depth_pipeline = gb_make_fullscreen_pipeline(
            GB_PYR_FROM_DEPTH_WGSL, WGPUTextureFormat_R32Float, "depth-pyramid mip0");
    if (!gb_pyr_reduce_pipeline)
        gb_pyr_reduce_pipeline = gb_make_fullscreen_pipeline(
            GB_PYR_REDUCE_WGSL, WGPUTextureFormat_R32Float, "depth-pyramid reduce");
    if (!gb_light_pipeline) {
        gb_lit_format = g_wgpu_have_rg11b10 ? WGPUTextureFormat_RG11B10Ufloat
                                            : WGPUTextureFormat_RGBA16Float;
        /* The lighting bind group references the shadow cascades, so
         * they must EXIST even for an app that never runs a shadow pass —
         * wgpu aborts the process on a null binding rather than returning
         * an error. An unrendered cascade is never sampled, because
         * shadowCfg.x stays 0 and sunVisibility returns 1.0 before
         * touching the texture. */
        g3d_shadow_init();
        g3d_shadow_ensure_targets(G3D_SHADOW_DEFAULT_RES, G3D_SHADOW_DEFAULT_CASCADES);
        gb_taa_pipeline = gb_make_fullscreen_pipeline(
            GB_TAA_WGSL, gb_lit_format, "taa");
        gb_ao_pipeline = gb_make_fullscreen_pipeline(
            GB_AO_WGSL, WGPUTextureFormat_R8Unorm, "ssao");
        gb_light_pipeline = gb_make_fullscreen_pipeline(
            GB_LIGHT_WGSL, gb_lit_format, "lighting");
        WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
        ud.size = GB_LIGHT_BYTES;
        ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        gb_light_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
    }
    if (!gb_composite_pipeline) {
        gb_composite_pipeline = gb_make_fullscreen_pipeline(
            GB_COMPOSITE_WGSL, g_g2d_fmt, "composite");
        WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
        ud.size = 16; ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        gb_composite_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
        gb_taa_ubuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
    }
}

/* One fullscreen pass: bind, draw three vertices, submit. */
static void gb_ensure_ao_bind(void) {
    if (gb_ao_bind || !gb_ao_pipeline || !gb_a_view || !gb_depth_view) return;
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_ao_pipeline, 0);
    WGPUBindGroupEntry e[3]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = gb_light_ubuf; e[0].size = GB_LIGHT_BYTES;
    e[1].binding = 1; e[1].textureView = gb_a_view;
    e[2].binding = 2; e[2].textureView = gb_depth_view;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 3; bgd.entries = e;
    gb_ao_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

static void gb_run_fullscreen(WGPURenderPipeline pipeline, WGPUBindGroup bind,
                              WGPUTextureView target) {
    if (!pipeline || !bind || !target) return;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = target;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
}

/* Build the depth pyramid: mip 0 from the G-buffer's depth, then each
 * level from the one above. Each level is a separate pass because a level
 * cannot be read until the level that produces it has finished. */
void rae_ext_gbuffer_depthPyramid(void) {
    if (!g_wgpu_dev || !gb_depth_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_pyramid_tex || !gb_pyr_from_depth_pipeline || !gb_pyr_reduce_pipeline) return;

    for (int i = 0; i < gb_pyramid_mips; i++) {
        if (!gb_pyr_bind[i]) {
            WGPURenderPipeline p = (i == 0) ? gb_pyr_from_depth_pipeline : gb_pyr_reduce_pipeline;
            WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(p, 0);
            WGPUBindGroupEntry e; memset(&e, 0, sizeof(e));
            e.binding = 0;
            e.textureView = (i == 0) ? gb_depth_view : gb_pyramid_src[i - 1];
            WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
            bgd.layout = bgl; bgd.entryCount = 1; bgd.entries = &e;
            gb_pyr_bind[i] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
            wgpuBindGroupLayoutRelease(bgl);
        }
        gb_run_fullscreen(i == 0 ? gb_pyr_from_depth_pipeline : gb_pyr_reduce_pipeline,
                          gb_pyr_bind[i], gb_pyramid_rt[i]);
    }
    if (getenv("RAE_GBUFFER_DEBUG")) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "[deferred] depth pyramid: %dx%d, %d mips\n",
                    gb_pyramid_w, gb_pyramid_h, gb_pyramid_mips);
            logged = 1;
        }
    }
}

/* Deferred lighting. Sun direction points TOWARD the scene, matching
 * Light3d and the forward path. */
/* SSAO (#387). Runs BEFORE lighting, so it writes the part of the light
 * uniform it needs itself — invViewProj and camPos, both derived from the
 * geometry pass's viewProj. Lighting rewrites the same values later; they
 * cannot disagree because both come from gb_viewproj. */
void rae_ext_gbuffer_ssao(float camX, float camY, float camZ) {
    if (!g_wgpu_dev || !gb_a_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_ao_pipeline || !gb_ao_view) return;

    float u[GB_LIGHT_BYTES / 4];
    memset(u, 0, sizeof(u));
    if (!g3d_invert_mat4(gb_viewproj_jittered, u)) {
        memset(u, 0, 16 * sizeof(float));
        u[0] = 1.0f; u[5] = 1.0f; u[10] = 1.0f; u[15] = 1.0f;
    }
    u[16] = camX; u[17] = camY; u[18] = camZ; u[19] = 0.0f;
    memcpy(u + 40, gb_viewproj_jittered, 16 * sizeof(float));
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_light_ubuf, 0, u, GB_LIGHT_BYTES);

    gb_ensure_ao_bind();
    if (!gb_ao_bind) return;
    gb_run_fullscreen(gb_ao_pipeline, gb_ao_bind, gb_ao_view);
}

void rae_ext_gbuffer_lighting(float camX, float camY, float camZ, float exposure,
                              float sunX, float sunY, float sunZ,
                              float sunR, float sunG, float sunB,
                              float skyR, float skyG, float skyB,
                              float gndR, float gndG, float gndB,
                              float skyKind, float turbidity, float skyExposure,
                              float sunSizeRad, float zenR, float zenG, float zenB,
                              float bands, float horR, float horG, float horB,
                              float discI, float groundAlbedo) {
    if (!g_wgpu_dev || !gb_a_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_light_pipeline || !gb_lit_view) return;

    float u[GB_LIGHT_BYTES / 4];
    memset(u, 0, sizeof(u));
    /* Invert THIS frame's view-projection — the one the geometry pass
     * rendered with — so reconstruction matches the depth being read. */
    if (!g3d_invert_mat4(gb_viewproj_jittered, u)) {
        /* Singular: a degenerate camera. Leaving the matrix zeroed would
         * collapse every pixel to the origin and light the frame from
         * inside itself; identity at least fails visibly and predictably. */
        memset(u, 0, 16 * sizeof(float));
        u[0] = 1.0f; u[5] = 1.0f; u[10] = 1.0f; u[15] = 1.0f;
    }
    u[16] = camX; u[17] = camY; u[18] = camZ; u[19] = exposure;
    u[20] = sunX; u[21] = sunY; u[22] = sunZ; u[23] = 0.0f;
    u[24] = sunR; u[25] = sunG; u[26] = sunB; u[27] = 0.0f;
    u[28] = skyR; u[29] = skyG; u[30] = skyB; u[31] = 0.0f;
    u[32] = gndR; u[33] = gndG; u[34] = gndB; u[35] = 0.0f;
    u[36] = gb_clear[0]; u[37] = gb_clear[1]; u[38] = gb_clear[2]; u[39] = 0.0f;
    memcpy(u + 40, gb_viewproj_jittered, 16 * sizeof(float));
    /* Sky (#400/#404), packed after viewProj to match LightU. The offsets
     * are not free-standing numbers: 56 is exactly where viewProj ends,
     * which is why the WGSL struct must declare viewProj even though the
     * lighting shader never reads it. */
    u[56] = skyKind; u[57] = turbidity; u[58] = skyExposure; u[59] = sunSizeRad;
    u[60] = zenR; u[61] = zenG; u[62] = zenB; u[63] = bands;
    u[64] = horR; u[65] = horG; u[66] = horB; u[67] = discI;
    /* Cooked Hosek-Wilkie state at u[68..97] (8 vec4 = 32 floats, 30 used).
     * Cooked HERE rather than passed in as 30 more arguments: this extern's
     * signature is already 29 floats, and growing an argument list that long is
     * how the sky parameters were silently dropped once already (the callee kept
     * the old arity and the extras went nowhere, with no diagnostic). The C side
     * calls the same memoized cook lib/sky.rae uses, so the CPU-side irradiance
     * and this background cannot disagree about the same sky. */
    memset(u + 68, 0, 36 * sizeof(float));
    if (skyKind > 3.5f) {
        /* sunDir points the way light TRAVELS, so the sun is at -sunDir and its
         * elevation above the horizon is asin(-sunZ). Clamped just above 0
         * because the dataset is not fitted below the horizon. */
        double elev = asin(fmin(1.0, fmax(0.0001, (double)(-sunZ))));
        double turb = (double)turbidity;
        double alb = (double)groundAlbedo;
        for (int ch = 0; ch < 3; ch++) {
            float* dst = u + 68 + ch * 12;
            for (int i = 0; i < 9; i++) {
                dst[i] = (float)rae_ext_hosek_config(turb, alb, elev, ch, i);
            }
            /* Layout per channel: [0..3]=A B C D, [4..7]=E F G H, [8]=I,
             * [9]=radiance — three vec4s, matching hosekChannel in the WGSL. */
            dst[9] = (float)rae_ext_hosek_config(turb, alb, elev, ch, 9);
        }
    }
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_light_ubuf, 0, u, GB_LIGHT_BYTES);

    if (!gb_light_bind) {
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_light_pipeline, 0);
        WGPUBindGroupEntry e[9]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = gb_light_ubuf; e[0].size = GB_LIGHT_BYTES;
        e[1].binding = 1; e[1].textureView = gb_a_view;
        e[2].binding = 2; e[2].textureView = gb_b_view;
        e[3].binding = 3; e[3].textureView = gb_c_view;
        e[4].binding = 4; e[4].textureView = gb_depth_view;
        e[5].binding = 5; e[5].textureView = gb_ao_view;
        e[6].binding = 6; e[6].buffer = g3d_sm_frame_ubuf; e[6].size = 320;
        e[7].binding = 7; e[7].textureView = g3d_sm_array_view;
        e[8].binding = 8; e[8].sampler = g3d_sm_sampler;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 9; bgd.entries = e;
        gb_light_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        wgpuBindGroupLayoutRelease(bgl);
    }
    gb_run_fullscreen(gb_light_pipeline, gb_light_bind, gb_lit_view);
}

/* Composite radiance into the presentable offscreen. */
/* TAA resolve (#390). Reads the radiance the lighting pass wrote plus
 * last frame's history, writes this frame's. The composite then reads
 * the resolved image rather than raw radiance. */
void rae_ext_gbuffer_taa(void) {
    if (!g_wgpu_dev || !gb_lit_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_taa_pipeline || !gb_taa_view[0] || !gb_taa_view[1]) return;
    if (!gb_taa_enabled) return;

    /* Resolve INTO the slot not read this frame, so no pass reads and
     * writes one texture. */
    gb_taa_cur = 1 - gb_taa_cur;

    float p[4] = { gb_taa_have_history ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_taa_ubuf, 0, p, sizeof(p));

    if (!gb_taa_bind[gb_taa_cur]) {
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_taa_pipeline, 0);
        WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].textureView = gb_lit_view;
        e[1].binding = 1; e[1].textureView = gb_taa_view[1 - gb_taa_cur];
        e[2].binding = 2; e[2].textureView = gb_c_view;
        e[3].binding = 3; e[3].buffer = gb_taa_ubuf; e[3].size = 16;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
        gb_taa_bind[gb_taa_cur] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        wgpuBindGroupLayoutRelease(bgl);
    }
    gb_run_fullscreen(gb_taa_pipeline, gb_taa_bind[gb_taa_cur], gb_taa_view[gb_taa_cur]);
    gb_taa_have_history = true;
}

void rae_ext_gbuffer_composite(float exposure) {
    if (!g_wgpu_dev || !g_g2d_off_view) return;
    gb_deferred_init_pipelines();
    gb_deferred_ensure();
    if (!gb_composite_pipeline || !gb_lit_view) return;

    float p[4] = { exposure, 0.0f, 0.0f, 0.0f };
    wgpuQueueWriteBuffer(g_wgpu_queue, gb_composite_ubuf, 0, p, sizeof(p));
    if (!gb_composite_bind[gb_taa_cur]) {
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(gb_composite_pipeline, 0);
        WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = gb_composite_ubuf; e[0].size = 16;
        /* The composite reads whatever the last radiometric pass wrote:
         * the TAA resolve when it ran, raw radiance when it did not. The
         * resolve's first frame copies radiance through unchanged, so
         * this is valid from frame one. */
        e[1].binding = 1; e[1].textureView = (gb_taa_enabled && gb_taa_view[gb_taa_cur])
                                              ? gb_taa_view[gb_taa_cur] : gb_lit_view;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
        gb_composite_bind[gb_taa_cur] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        wgpuBindGroupLayoutRelease(bgl);
    }
    gb_run_fullscreen(gb_composite_pipeline, gb_composite_bind[gb_taa_cur], g_g2d_off_view);
}

/* Number of mip levels in the pyramid — 0 before the first build. Lets a
 * caller or test confirm the chain was actually built rather than skipped. */
int64_t rae_ext_gbuffer_pyramidMips(void) { return (int64_t)gb_pyramid_mips; }

void rae_ext_gbuffer_deferredShutdown(void) {
    gb_deferred_release_targets();
    if (gb_pyr_from_depth_pipeline) { wgpuRenderPipelineRelease(gb_pyr_from_depth_pipeline); gb_pyr_from_depth_pipeline = NULL; }
    if (gb_pyr_reduce_pipeline)     { wgpuRenderPipelineRelease(gb_pyr_reduce_pipeline); gb_pyr_reduce_pipeline = NULL; }
    if (gb_light_pipeline)          { wgpuRenderPipelineRelease(gb_light_pipeline); gb_light_pipeline = NULL; }
    if (gb_light_ubuf)              { wgpuBufferRelease(gb_light_ubuf); gb_light_ubuf = NULL; }
    if (gb_composite_pipeline)      { wgpuRenderPipelineRelease(gb_composite_pipeline); gb_composite_pipeline = NULL; }
    if (gb_composite_ubuf)          { wgpuBufferRelease(gb_composite_ubuf); gb_composite_ubuf = NULL; }
    if (gb_taa_ubuf)                { wgpuBufferRelease(gb_taa_ubuf); gb_taa_ubuf = NULL; }
    if (gb_taa_pipeline)            { wgpuRenderPipelineRelease(gb_taa_pipeline); gb_taa_pipeline = NULL; }
    gb_deferred_gen = -1;
}
