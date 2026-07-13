/* gpu2d box/shape pipeline and draw queue. Raw pipeline calls stay C; render batching policy is a future Rae migration candidate.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* --- Box uber-shader pipeline (#110) ----------------------------------
 * Instanced rounded-box SDF with analytic AA: one quad per primitive, the
 * fragment shader evaluates a rounded-box signed distance and antialiases
 * with screen-space derivatives (no MSAA). One pipeline → filled/rounded
 * rects, per-corner radius, borders. Primitives are accumulated CPU-side
 * each frame and drawn in one instanced draw at endFrame (painter's order).
 * Instance layout = 6×vec4 (std430): rect, radius, fill, border, params, grad. */
#define G2D_PRIM_FLOATS 24

static const char* G2D_BOX_WGSL =
"struct Prim {\n"
"  rect: vec4<f32>,\n"
"  radius: vec4<f32>,\n"
"  fill: vec4<f32>,\n"
"  border: vec4<f32>,\n"
"  params: vec4<f32>,\n"
"  grad: vec4<f32>,\n"
"};\n"
/* uXform[0] = (physW, physH, scaleX, scaleY); uXform[1] = (offsetX, offsetY,..)
 * maps design-unit coords -> physical px: px = design*scale + offset. */
"@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;\n"
"@group(0) @binding(1) var<storage, read> prims: array<Prim>;\n"
/* #118 rounded clip: uClip[0]=(x,y,w,h) design units, uClip[1]=(radius,enabled,..) */
"@group(0) @binding(2) var<uniform> uClip: array<vec4<f32>, 2>;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) local: vec2<f32>,\n"
"  @location(1) @interpolate(flat) inst: u32,\n"
"  @location(2) posD: vec2<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {\n"
"  var corners = array<vec2<f32>, 6>(\n"
"    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),\n"
"    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));\n"
"  let c = corners[vi];\n"
"  let p = prims[ii];\n"
"  let phys = uXform[0].xy;\n"
"  let local = c * p.rect.zw;\n"            /* box-local, unrotated (0..w, 0..h) */
"  let center = p.rect.zw * 0.5;\n"
"  let a = p.params.y;\n"                   /* rotation (radians), 0 for rects */
"  let ca = cos(a); let sa = sin(a);\n"
"  let rel = local - center;\n"
"  let rot = vec2<f32>(rel.x * ca - rel.y * sa, rel.x * sa + rel.y * ca);\n"
"  let posDesign = p.rect.xy + center + rot;\n"
"  let posPx = posDesign * uXform[0].zw + uXform[1].xy;\n"
"  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0,\n"
"                      1.0 - posPx.y / phys.y * 2.0);\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(ndc, 0.0, 1.0);\n"
"  o.local = local;\n"                      /* SDF evaluates in unrotated box frame */
"  o.inst = ii;\n"
"  o.posD = posDesign;\n"                   /* design-space pos for the clip SDF */
"  return o;\n"
"}\n"
"fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: vec4<f32>) -> f32 {\n"
"  let rad = select(r.zw, r.xy, p.x > 0.0);\n"
"  let rr = select(rad.y, rad.x, p.y > 0.0);\n"
"  let q = abs(p) - b + vec2<f32>(rr, rr);\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - rr;\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @location(0) vec4<f32> {\n"
"  let p = prims[in.inst];\n"
"  let halfSize = p.rect.zw * 0.5;\n"
"  let center = in.local - halfSize;\n"
"  let d = sdRoundBox(center, halfSize, p.radius);\n"
"  let aa = max(fwidth(d), 0.0001);\n"
"  var cov = 1.0 - smoothstep(-aa, aa, d);\n"
"  if (uClip[1].y > 0.5) {\n"                /* #118: rounded clip coverage */
"    let cc = uClip[0].xy + uClip[0].zw * 0.5;\n"
"    let ch = uClip[0].zw * 0.5;\n"
"    let cr = uClip[1].x;\n"
"    let cd = sdRoundBox(in.posD - cc, ch, vec4<f32>(cr, cr, cr, cr));\n"
"    let caa = max(fwidth(cd), 0.0001);\n"
"    cov = cov * (1.0 - smoothstep(-caa, caa, cd));\n"
"  }\n"
"  var col = p.fill;\n"
"  if (p.params.z > 0.5) {\n"
"    let uv = in.local / max(p.rect.zw, vec2<f32>(1.0, 1.0));\n"
"    let dir = vec2<f32>(cos(p.params.w), sin(p.params.w));\n"
"    let centered = uv - vec2<f32>(0.5, 0.5);\n"
"    let extent = max(abs(dir.x) * 0.5 + abs(dir.y) * 0.5, 0.0001);\n"
"    let t = clamp(dot(centered, dir) / (extent * 2.0) + 0.5, 0.0, 1.0);\n"
"    col = mix(p.fill, p.grad, t);\n"
"  }\n"
"  let bw = p.params.x;\n"
"  if (bw > 0.0) {\n"
"    let inner = 1.0 - smoothstep(-aa, aa, d + bw);\n"
"    col = mix(p.border, p.fill, inner);\n"
"  }\n"
"  return col * cov;\n"
"}\n";

static WGPURenderPipeline g_g2d_pipeline = NULL;
static WGPUBuffer    g_g2d_uniform = NULL;
static WGPUBuffer    g_g2d_instbuf = NULL;
static WGPUBindGroup g_g2d_bind = NULL;
static int   g_g2d_inst_cap = 0;       /* capacity in primitives */
static float* g_g2d_prims = NULL;      /* CPU accumulation (floats) */
static int   g_g2d_prim_count = 0;
static int   g_g2d_prim_capf = 0;      /* capacity in floats */

static void g2d_color(uint32_t c, float* out) {
    /* 0xAARRGGBB -> premultiplied RGBA in 0..1 */
    float a = (float)((c >> 24) & 0xFF) / 255.0f;
    float r = (float)((c >> 16) & 0xFF) / 255.0f;
    float g = (float)((c >> 8)  & 0xFF) / 255.0f;
    float b = (float)( c        & 0xFF) / 255.0f;
    out[0] = r * a; out[1] = g * a; out[2] = b * a; out[3] = a;
}

static void rae_g2d_push(double x, double y, double w, double h,
                         double rtl, double rtr, double rbr, double rbl,
                         uint32_t fill, uint32_t border, double bw, double angle) {
    int need = (g_g2d_prim_count + 1) * G2D_PRIM_FLOATS;
    if (need > g_g2d_prim_capf) {
        int cap = g_g2d_prim_capf ? g_g2d_prim_capf : (64 * G2D_PRIM_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_prims = (float*)realloc(g_g2d_prims, (size_t)cap * sizeof(float));
        g_g2d_prim_capf = cap;
    }
    float* p = g_g2d_prims + g_g2d_prim_count * G2D_PRIM_FLOATS;
    p[0]=(float)x; p[1]=(float)y; p[2]=(float)w; p[3]=(float)h;
    p[4]=(float)rtl; p[5]=(float)rtr; p[6]=(float)rbr; p[7]=(float)rbl;
    g2d_color(fill, &p[8]);
    g2d_color(border, &p[12]);
    p[16]=(float)bw; p[17]=(float)angle; p[18]=0.0f; p[19]=0.0f;
    g2d_color(fill, &p[20]);
    rae_g2d_prim_clip_ensure(g_g2d_prim_count + 1);
    g_g2d_prim_clip[g_g2d_prim_count] = g_g2d_cur_clip;
    g_g2d_prim_count++;
}

static void rae_g2d_push_gradient(double x, double y, double w, double h,
                                  double radius, uint32_t from, uint32_t to,
                                  double angle_deg) {
    int need = (g_g2d_prim_count + 1) * G2D_PRIM_FLOATS;
    if (need > g_g2d_prim_capf) {
        int cap = g_g2d_prim_capf ? g_g2d_prim_capf : (64 * G2D_PRIM_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_prims = (float*)realloc(g_g2d_prims, (size_t)cap * sizeof(float));
        g_g2d_prim_capf = cap;
    }
    float* p = g_g2d_prims + g_g2d_prim_count * G2D_PRIM_FLOATS;
    p[0]=(float)x; p[1]=(float)y; p[2]=(float)w; p[3]=(float)h;
    p[4]=(float)radius; p[5]=(float)radius; p[6]=(float)radius; p[7]=(float)radius;
    g2d_color(from, &p[8]);
    g2d_color(0, &p[12]);
    p[16]=0.0f;
    p[17]=0.0f;
    p[18]=1.0f;
    p[19]=(float)(angle_deg * 0.017453292519943295);
    g2d_color(to, &p[20]);
    rae_g2d_prim_clip_ensure(g_g2d_prim_count + 1);
    g_g2d_prim_clip[g_g2d_prim_count] = g_g2d_cur_clip;
    g_g2d_prim_count++;
}

static void rae_g2d_init_pipeline(void) {
    if (g_g2d_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G2D_BOX_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUBlendState blend; memset(&blend, 0, sizeof(blend));
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;            /* premultiplied alpha */
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.blend = &blend; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;  /* auto layout from shader bindings */
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g_g2d_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);

    WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
    ud.size = 32; ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;  /* 2*vec4 xform */
    g_g2d_uniform = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
}

static void rae_g2d_rebuild_bind(void) {
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_pipeline, 0);
    WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
    e[1].binding = 1; e[1].buffer = g_g2d_instbuf;
    e[1].size = (uint64_t)g_g2d_inst_cap * G2D_PRIM_FLOATS * sizeof(float);
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
    g_g2d_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

static void rae_g2d_ensure_inst(int prims) {
    if (g_g2d_instbuf && prims <= g_g2d_inst_cap) return;
    int cap = g_g2d_inst_cap ? g_g2d_inst_cap : 64;
    while (cap < prims) cap *= 2;
    if (g_g2d_instbuf) wgpuBufferRelease(g_g2d_instbuf);
    if (g_g2d_bind) { wgpuBindGroupRelease(g_g2d_bind); g_g2d_bind = NULL; }
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = (uint64_t)cap * G2D_PRIM_FLOATS * sizeof(float);
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g_g2d_instbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    g_g2d_inst_cap = cap;
}

void rae_ext_gpu2d_drawRect(double x, double y, double w, double h, int64_t color) {
    rae_g2d_push(x, y, w, h, 0, 0, 0, 0, (uint32_t)color, 0, 0.0, 0.0);
}
void rae_ext_gpu2d_drawRoundedRect(double x, double y, double w, double h, double radius, int64_t color) {
    rae_g2d_push(x, y, w, h, radius, radius, radius, radius, (uint32_t)color, 0, 0.0, 0.0);
}
void rae_ext_gpu2d_drawBox(double x, double y, double w, double h, double radius,
                           int64_t fill, double borderWidth, int64_t border) {
    rae_g2d_push(x, y, w, h, radius, radius, radius, radius,
                 (uint32_t)fill, (uint32_t)border, borderWidth, 0.0);
}
void rae_ext_gpu2d_drawGradientRect(double x, double y, double w, double h,
                                    double radius, int64_t from, int64_t to,
                                    double angleDeg) {
    rae_g2d_push_gradient(x, y, w, h, radius, (uint32_t)from, (uint32_t)to, angleDeg);
}
/* A line from (x0,y0) to (x1,y1), `thickness` px wide, with rounded caps —
 * a rotated capsule (rounded rect of length x thickness, radius thickness/2). */
void rae_ext_gpu2d_drawLine(double x0, double y0, double x1, double y1,
                            double thickness, int64_t color) {
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6 || thickness <= 0.0) return;
    double angle = atan2(dy, dx);
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double r = thickness * 0.5;
    if (r > len * 0.5) r = len * 0.5;
    rae_g2d_push(cx - len * 0.5, cy - thickness * 0.5, len, thickness,
                 r, r, r, r, (uint32_t)color, 0, 0.0, angle);
}

