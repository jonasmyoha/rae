/* Native WebGPU compute/resource binding layer. Raw WebGPU calls stay C; resource management policy should migrate to Rae.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* ============================================================
 * Native WebGPU via wgpu-native — see lib/webgpu.rae (RAE_HAS_WEBGPU).
 *
 * Runs a WGSL compute shader on the native GPU (Metal/Vulkan/D3D12 through
 * wgpu-native's webgpu.h) — the SAME shader the browser runs (WebGPU-everywhere
 * per docs/tech-stack-and-dependencies.md). The device/queue are created once
 * and cached; each webgpuRaytrace call uploads the scene, dispatches one
 * invocation per pixel, reads the framebuffer back, and writes packed-0xRRGGBB
 * ints (so the SDL3 backend can present it). v29 API: Future/CallbackInfo +
 * WGPUStringView; readback via wgpuBufferMapAsync + wgpuDevicePoll.
 * ============================================================ */
#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

static WGPUInstance g_wgpu_inst = NULL;
static WGPUDevice   g_wgpu_dev = NULL;
static WGPUQueue    g_wgpu_queue = NULL;

static WGPUStringView rae_wgpu_sv(const char* s) { WGPUStringView v; v.data = s; v.length = WGPU_STRLEN; return v; }

static WGPUAdapter g_wgpu_adapter; static int g_wgpu_adapter_done;
static void rae_wgpu_on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_adapter = a; g_wgpu_adapter_done = 1;
}
static int g_wgpu_device_done;
static void rae_wgpu_on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_dev = d; g_wgpu_device_done = 1;
}
static int g_wgpu_map_done;
static void rae_wgpu_on_map(WGPUMapAsyncStatus st, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_map_done = 1;
}

static int rae_wgpu_init(void) {
    if (g_wgpu_dev) return 1;
    g_wgpu_inst = wgpuCreateInstance(NULL);
    if (!g_wgpu_inst) { fprintf(stderr, "[wgpu] no instance\n"); return 0; }
    WGPURequestAdapterOptions ao; memset(&ao, 0, sizeof(ao));
    WGPURequestAdapterCallbackInfo aci; memset(&aci, 0, sizeof(aci));
    aci.mode = WGPUCallbackMode_AllowProcessEvents; aci.callback = rae_wgpu_on_adapter;
    wgpuInstanceRequestAdapter(g_wgpu_inst, &ao, aci);
    while (!g_wgpu_adapter_done) wgpuInstanceProcessEvents(g_wgpu_inst);
    if (!g_wgpu_adapter) { fprintf(stderr, "[wgpu] no adapter\n"); return 0; }
    WGPURequestDeviceCallbackInfo dci; memset(&dci, 0, sizeof(dci));
    dci.mode = WGPUCallbackMode_AllowProcessEvents; dci.callback = rae_wgpu_on_device;
    wgpuAdapterRequestDevice(g_wgpu_adapter, NULL, dci);
    while (!g_wgpu_device_done) wgpuInstanceProcessEvents(g_wgpu_inst);
    if (!g_wgpu_dev) { fprintf(stderr, "[wgpu] no device\n"); return 0; }
    g_wgpu_queue = wgpuDeviceGetQueue(g_wgpu_dev);
    return 1;
}

/* scene: sceneLen f64 (camera 19 + spheres*10) -> narrowed to f32 for the GPU.
 * fb: width*height int64 written as packed 0xRRGGBB. wgsl: shader source. */
void rae_ext_webgpu_raytrace(const double* scene, int64_t sceneLen, int64_t* fb,
                            int64_t width, int64_t height, int64_t samples,
                            int64_t maxDepth, rae_String wgsl) {
    if (!fb || width <= 0 || height <= 0) return;
    if (!rae_wgpu_init()) return;

    float* sf = (float*)malloc((size_t)sceneLen * sizeof(float));
    if (!sf) return;
    for (int64_t i = 0; i < sceneLen; i++) sf[i] = (float)scene[i];
    int64_t sphereCount = (sceneLen - 19) / 10;

    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = wgsl.data ? rae_wgpu_sv((const char*)wgsl.data) : rae_wgpu_sv("");
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    uint32_t params[8] = { (uint32_t)width, (uint32_t)height, (uint32_t)samples,
                           (uint32_t)maxDepth, (uint32_t)sphereCount, 0, 0, 0 };
    WGPUBufferDescriptor pbd; memset(&pbd, 0, sizeof(pbd));
    pbd.size = sizeof(params); pbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    WGPUBuffer pbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &pbd);
    wgpuQueueWriteBuffer(g_wgpu_queue, pbuf, 0, params, sizeof(params));

    size_t scene_bytes = (size_t)sceneLen * sizeof(float);
    WGPUBufferDescriptor sbd; memset(&sbd, 0, sizeof(sbd));
    sbd.size = scene_bytes; sbd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    WGPUBuffer sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &sbd);
    wgpuQueueWriteBuffer(g_wgpu_queue, sbuf, 0, sf, scene_bytes);

    size_t obytes = (size_t)width * (size_t)height * 4;
    WGPUBufferDescriptor obd; memset(&obd, 0, sizeof(obd));
    obd.size = obytes; obd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    WGPUBuffer obuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &obd);
    WGPUBufferDescriptor rbd; memset(&rbd, 0, sizeof(rbd));
    rbd.size = obytes; rbd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer rbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &rbd);

    WGPUComputePipelineDescriptor cpd; memset(&cpd, 0, sizeof(cpd));
    cpd.compute.module = mod; cpd.compute.entryPoint = rae_wgpu_sv("main");
    WGPUComputePipeline pipe = wgpuDeviceCreateComputePipeline(g_wgpu_dev, &cpd);
    if (!pipe) { fprintf(stderr, "[wgpu] pipeline failed\n"); free(sf); return; }

    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    WGPUBindGroupEntry be[3]; memset(be, 0, sizeof(be));
    be[0].binding = 0; be[0].buffer = pbuf; be[0].size = sizeof(params);
    be[1].binding = 1; be[1].buffer = sbuf; be[1].size = scene_bytes;
    be[2].binding = 2; be[2].buffer = obuf; be[2].size = obytes;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 3; bgd.entries = be;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPUComputePassDescriptor cpassd; memset(&cpassd, 0, sizeof(cpassd));
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cpassd);
    wgpuComputePassEncoderSetPipeline(pass, pipe);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (uint32_t)((width + 7) / 8), (uint32_t)((height + 7) / 8), 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(enc, obuf, 0, rbuf, 0, obytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);

    WGPUBufferMapCallbackInfo mci; memset(&mci, 0, sizeof(mci));
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = rae_wgpu_on_map;
    g_wgpu_map_done = 0;
    wgpuBufferMapAsync(rbuf, WGPUMapMode_Read, 0, obytes, mci);
    while (!g_wgpu_map_done) wgpuDevicePoll(g_wgpu_dev, true, NULL);
    const uint32_t* px = (const uint32_t*)wgpuBufferGetConstMappedRange(rbuf, 0, obytes);
    if (px) {
        int64_t n = width * height;
        for (int64_t i = 0; i < n; i++) {
            uint32_t p = px[i];                      /* GPU packed R|G<<8|B<<16|A<<24 */
            uint32_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
            fb[i] = (int64_t)((r << 16) | (g << 8) | b);  /* -> 0xRRGGBB for SDL */
        }
    }
    wgpuBufferUnmap(rbuf);
    /* per-call GPU objects */
    wgpuBindGroupRelease(bg); wgpuBindGroupLayoutRelease(bgl);
    wgpuComputePipelineRelease(pipe); wgpuShaderModuleRelease(mod);
    wgpuBufferRelease(rbuf); wgpuBufferRelease(obuf);
    wgpuBufferRelease(sbuf); wgpuBufferRelease(pbuf);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    wgpuComputePassEncoderRelease(pass);
    free(sf);
}

/* ---- Generic GPU compute (lib/gpu.rae): handle tables over the cached
 * device, so arbitrary WGSL kernels + buffers can be authored from Rae, not
 * just the raytracer. Handles are 1-based (0 = failure). ---- */
#define RAE_GPU_MAX_BUF 256
#define RAE_GPU_MAX_PIPE 64
static WGPUBuffer g_gpu_buf[RAE_GPU_MAX_BUF];
static size_t     g_gpu_buf_size[RAE_GPU_MAX_BUF];
static int        g_gpu_buf_n = 0;
static WGPUComputePipeline g_gpu_pipe[RAE_GPU_MAX_PIPE];
static int        g_gpu_pipe_n = 0;

static int rae_gpu_add_buf(WGPUBuffer b, size_t size) {
    if (!b || g_gpu_buf_n >= RAE_GPU_MAX_BUF) return 0;
    g_gpu_buf[g_gpu_buf_n] = b; g_gpu_buf_size[g_gpu_buf_n] = size;
    return ++g_gpu_buf_n;  /* 1-based handle */
}

int64_t rae_ext_gpu_storageF32(const double* data, int64_t count) {
    if (!rae_wgpu_init() || count <= 0) return 0;
    size_t bytes = (size_t)count * 4;
    float* tmp = (float*)malloc(bytes);
    if (!tmp) return 0;
    for (int64_t i = 0; i < count; i++) tmp[i] = (float)data[i];
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = bytes; bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    WGPUBuffer b = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    if (b) wgpuQueueWriteBuffer(g_wgpu_queue, b, 0, tmp, bytes);
    free(tmp);
    return rae_gpu_add_buf(b, bytes);
}
int64_t rae_ext_gpu_uniformU32(const int64_t* data, int64_t count) {
    if (!rae_wgpu_init() || count <= 0) return 0;
    size_t bytes = (size_t)count * 4;
    uint32_t* tmp = (uint32_t*)malloc(bytes);
    if (!tmp) return 0;
    for (int64_t i = 0; i < count; i++) tmp[i] = (uint32_t)data[i];
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = bytes; bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    WGPUBuffer b = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    if (b) wgpuQueueWriteBuffer(g_wgpu_queue, b, 0, tmp, bytes);
    free(tmp);
    return rae_gpu_add_buf(b, bytes);
}
static int64_t rae_gpu_alloc(int64_t count) {
    if (!rae_wgpu_init() || count <= 0) return 0;
    size_t bytes = (size_t)count * 4;
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = bytes; bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    WGPUBuffer b = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    return rae_gpu_add_buf(b, bytes);
}
int64_t rae_ext_gpu_allocF32(int64_t count) { return rae_gpu_alloc(count); }
int64_t rae_ext_gpu_allocU32(int64_t count) { return rae_gpu_alloc(count); }

void rae_ext_gpu_writeF32(int64_t buf, const double* data, int64_t count) {
    if (!g_wgpu_queue || buf < 1 || buf > g_gpu_buf_n || count <= 0) return;
    size_t bytes = (size_t)count * 4;
    float* tmp = (float*)malloc(bytes);
    if (!tmp) return;
    for (int64_t i = 0; i < count; i++) tmp[i] = (float)data[i];
    wgpuQueueWriteBuffer(g_wgpu_queue, g_gpu_buf[buf - 1], 0, tmp, bytes);
    free(tmp);
}
void rae_ext_gpu_writeU32(int64_t buf, const int64_t* data, int64_t count) {
    if (!g_wgpu_queue || buf < 1 || buf > g_gpu_buf_n || count <= 0) return;
    size_t bytes = (size_t)count * 4;
    uint32_t* tmp = (uint32_t*)malloc(bytes);
    if (!tmp) return;
    for (int64_t i = 0; i < count; i++) tmp[i] = (uint32_t)data[i];
    wgpuQueueWriteBuffer(g_wgpu_queue, g_gpu_buf[buf - 1], 0, tmp, bytes);
    free(tmp);
}

int64_t rae_ext_gpu_kernel(rae_String wgsl, rae_String entry) {
    if (!rae_wgpu_init() || g_gpu_pipe_n >= RAE_GPU_MAX_PIPE) return 0;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(wgsl.data ? (const char*)wgsl.data : "");
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);
    WGPUComputePipelineDescriptor cpd; memset(&cpd, 0, sizeof(cpd));
    cpd.compute.module = mod;
    cpd.compute.entryPoint = rae_wgpu_sv(entry.data ? (const char*)entry.data : "main");
    WGPUComputePipeline pipe = wgpuDeviceCreateComputePipeline(g_wgpu_dev, &cpd);
    wgpuShaderModuleRelease(mod);
    if (!pipe) { fprintf(stderr, "[gpu] kernel compile failed\n"); return 0; }
    g_gpu_pipe[g_gpu_pipe_n] = pipe;
    return ++g_gpu_pipe_n;  /* 1-based */
}

void rae_ext_gpu_run(int64_t kernel, const int64_t* bufs, int64_t bufCount,
                    int64_t gx, int64_t gy, int64_t gz) {
    if (!g_wgpu_dev || kernel < 1 || kernel > g_gpu_pipe_n || bufCount < 0 || bufCount > 16) return;
    WGPUComputePipeline pipe = g_gpu_pipe[kernel - 1];
    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    WGPUBindGroupEntry be[16]; memset(be, 0, sizeof(be));
    for (int64_t i = 0; i < bufCount; i++) {
        int64_t h = bufs[i];
        if (h < 1 || h > g_gpu_buf_n) return;
        be[i].binding = (uint32_t)i;
        be[i].buffer = g_gpu_buf[h - 1];
        be[i].size = g_gpu_buf_size[h - 1];
    }
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = (size_t)bufCount; bgd.entries = be;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPUComputePassDescriptor cpassd; memset(&cpassd, 0, sizeof(cpassd));
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cpassd);
    wgpuComputePassEncoderSetPipeline(pass, pipe);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (uint32_t)gx, (uint32_t)gy, (uint32_t)gz);
    wgpuComputePassEncoderEnd(pass);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);
    wgpuDevicePoll(g_wgpu_dev, true, NULL);  /* wait for completion */
    wgpuCommandBufferRelease(cmd); wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc); wgpuBindGroupRelease(bg); wgpuBindGroupLayoutRelease(bgl);
}

/* Copy a GPU buffer to a staging buffer, map it, and read into `out`. */
static const void* rae_gpu_readback(int64_t buf, size_t bytes, WGPUBuffer* staging_out) {
    if (!g_wgpu_dev || buf < 1 || buf > g_gpu_buf_n) return NULL;
    WGPUBufferDescriptor rbd; memset(&rbd, 0, sizeof(rbd));
    rbd.size = bytes; rbd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(g_wgpu_dev, &rbd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    wgpuCommandEncoderCopyBufferToBuffer(enc, g_gpu_buf[buf - 1], 0, staging, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    g_wgpu_map_done = 0;
    WGPUBufferMapCallbackInfo mci; memset(&mci, 0, sizeof(mci));
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = rae_wgpu_on_map;
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, mci);
    while (!g_wgpu_map_done) wgpuDevicePoll(g_wgpu_dev, true, NULL);
    *staging_out = staging;
    return wgpuBufferGetConstMappedRange(staging, 0, bytes);
}
void rae_ext_gpu_downloadF32(int64_t buf, double* out, int64_t count) {
    if (!out || count <= 0) return;
    WGPUBuffer staging = NULL;
    const float* p = (const float*)rae_gpu_readback(buf, (size_t)count * 4, &staging);
    if (p) for (int64_t i = 0; i < count; i++) out[i] = (double)p[i];
    if (staging) { wgpuBufferUnmap(staging); wgpuBufferRelease(staging); }
}
void rae_ext_gpu_downloadU32(int64_t buf, int64_t* out, int64_t count) {
    if (!out || count <= 0) return;
    WGPUBuffer staging = NULL;
    const uint32_t* p = (const uint32_t*)rae_gpu_readback(buf, (size_t)count * 4, &staging);
    if (p) for (int64_t i = 0; i < count; i++) out[i] = (int64_t)p[i];
    if (staging) { wgpuBufferUnmap(staging); wgpuBufferRelease(staging); }
}
void rae_ext_gpu_reset(void) {
    for (int i = 0; i < g_gpu_buf_n; i++) if (g_gpu_buf[i]) wgpuBufferRelease(g_gpu_buf[i]);
    for (int i = 0; i < g_gpu_pipe_n; i++) if (g_gpu_pipe[i]) wgpuComputePipelineRelease(g_gpu_pipe[i]);
    g_gpu_buf_n = 0; g_gpu_pipe_n = 0;
}
