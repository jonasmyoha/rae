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
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <webgpu/webgpu.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* Asyncify bridge for Rae's current blocking application-loop shape. Unlike
 * emscripten_sleep(0), this yields at an actual browser animation frame, which
 * is when WebGPU canvas textures are presented (including inside iframes). */
EM_ASYNC_JS(void, rae_browser_next_frame, (), {
    await new Promise(resolve => requestAnimationFrame(resolve));
});
#else
#include <webgpu/wgpu.h>
#endif

static WGPUInstance g_wgpu_inst = NULL;
static WGPUDevice   g_wgpu_dev = NULL;
static WGPUQueue    g_wgpu_queue = NULL;

/* Whether the device has been lost: wgpu said so (rae_wgpu_on_device_lost), or
 * Rae did (lib/gpu/GpuFault.rae: work that never ends, a driver that refuses
 * the app's work). */
static int g_wgpu_device_lost = 0;
int64_t rae_wgpu_device_is_lost(void) { return g_wgpu_device_lost; }
void rae_wgpu_mark_device_lost(void) { g_wgpu_device_lost = 1; }

/* For a check (checks/gpuHang): the GPU's work never ends, as after a fault */
static int g_wgpu_test_hang = 0;
void rae_wgpu_test_hang(int64_t on) { g_wgpu_test_hang = on != 0; }

/* How long a blocking wait lasts at most (ms): the app does not hang for good
 * on work that never ends. RAE_GPU_WAIT_SECONDS sets it; long on a desktop,
 * whose GPU other programs may keep busy. */
static int64_t rae_wgpu_wait_limit_ms(void) {
    static int64_t limit = 0;
    if (limit == 0) {
        const char* text = getenv("RAE_GPU_WAIT_SECONDS");
        limit = text && *text ? (int64_t)(atof(text) * 1000.0) : 0;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        if (limit <= 0) limit = 5000;
#else
        if (limit <= 0) limit = 20000;
#endif
    }
    return limit;
}

/* wgpu-native exposes device polling; browser WebGPU advances when control
 * returns to JavaScript. WASM builds use Asyncify, so a zero-duration sleep
 * yields to the browser before processing callbacks on the instance.
 *
 * Natively a wait is a poll that does not block, again and again until the
 * queue is empty: wgpu's own blocking poll has no limit in its C API, and on
 * Metal it leaves only for a completed command buffer, never for a failed one,
 * so after a GPU fault it waited for good (an iPhone, 2026-11-24). Past the
 * limit the device counts as lost. Each sleep counts at most 50 ms, so the time
 * the app was suspended does not. */
static void rae_wgpu_poll(int wait) {
#ifdef __EMSCRIPTEN__
    if (wait) emscripten_sleep(0);
    if (g_wgpu_inst) wgpuInstanceProcessEvents(g_wgpu_inst);
#else
    if (!g_wgpu_dev) return;
    int64_t waited_us = 0;
    int64_t limit_us = rae_wgpu_wait_limit_ms() * 1000;
    for (;;) {
        int empty = wgpuDevicePoll(g_wgpu_dev, 0, NULL) && !g_wgpu_test_hang;
        if (!wait || empty || g_wgpu_device_lost) return;
        if (waited_us >= limit_us) {
            fprintf(stderr, "[gpu] a wait for the GPU gave up after %lld ms: the device counts as lost\n",
                    (long long)(waited_us / 1000));
            g_wgpu_device_lost = 1;
            return;
        }
        struct timespec before, after, nap = { 0, 250000 };
        clock_gettime(CLOCK_MONOTONIC, &before);
        nanosleep(&nap, NULL);
        clock_gettime(CLOCK_MONOTONIC, &after);
        int64_t us = (after.tv_sec - before.tv_sec) * 1000000LL + (after.tv_nsec - before.tv_nsec) / 1000;
        waited_us += us > 50000 ? 50000 : us;
    }
#endif
}


static WGPUStringView rae_wgpu_sv(const char* s) { WGPUStringView v; v.data = s; v.length = WGPU_STRLEN; return v; }

static WGPUAdapter g_wgpu_adapter; static int g_wgpu_adapter_done;
static void rae_wgpu_on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_adapter = a; g_wgpu_adapter_done = 1;
}
/* Set during device creation; read by renderers choosing a target format.
 * Zero means the adapter did not offer the feature and the fallback path
 * must be used. */
static int g_wgpu_have_rg11b10 = 0;
static int g_wgpu_have_timestamp = 0;   /* #528: timestamp-query features present */
static int g_wgpu_device_done;
static void rae_wgpu_on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_dev = d; g_wgpu_device_done = 1;
}
/* The device's lost and uncaptured-error callbacks: without them a lost
 * device (an iOS GPU watchdog, a driver reset) or a validation error was
 * silent, and the next wgpu call that checks the device aborted the app with
 * no reason in the log. They say why, on stderr. */
static int g_wgpu_uncaptured_errors = 0;
static void rae_wgpu_on_device_lost(WGPUDevice const* d, WGPUDeviceLostReason reason, WGPUStringView m,
                                    void* u1, void* u2) {
    (void)d; (void)u1; (void)u2;
    g_wgpu_device_lost = 1;
    fprintf(stderr, "[wgpu] device lost (reason %d): %.*s\n", (int)reason,
            m.data ? (int)(m.length == WGPU_STRLEN ? strlen(m.data) : m.length) : 0, m.data ? m.data : "");
}
static void rae_wgpu_on_uncaptured_error(WGPUDevice const* d, WGPUErrorType type, WGPUStringView m,
                                         void* u1, void* u2) {
    (void)d; (void)u1; (void)u2;
    if (++g_wgpu_uncaptured_errors > 20) return;
    fprintf(stderr, "[wgpu] error (type %d): %.*s\n", (int)type,
            m.data ? (int)(m.length == WGPU_STRLEN ? strlen(m.data) : m.length) : 0, m.data ? m.data : "");
}

static int g_wgpu_map_done;
static void rae_wgpu_on_map(WGPUMapAsyncStatus st, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_map_done = 1;
}

/* The adapter, once: the callback-driven request (instance + adapter) Rae
 * cannot make itself. Rae asks it which features it has (wgpuAdapterHasFeature
 * over the bindings) before the device exists. */
static int rae_wgpu_adapter_init(void) {
    if (g_wgpu_adapter) return 1;
    if (!g_wgpu_inst) g_wgpu_inst = wgpuCreateInstance(NULL);
    if (!g_wgpu_inst) { fprintf(stderr, "[wgpu] no instance\n"); return 0; }
    WGPURequestAdapterOptions ao; memset(&ao, 0, sizeof(ao));
    WGPURequestAdapterCallbackInfo aci; memset(&aci, 0, sizeof(aci));
    aci.mode = WGPUCallbackMode_AllowProcessEvents; aci.callback = rae_wgpu_on_adapter;
    wgpuInstanceRequestAdapter(g_wgpu_inst, &ao, aci);
    while (!g_wgpu_adapter_done) rae_wgpu_poll(1);
    if (!g_wgpu_adapter) { fprintf(stderr, "[wgpu] no adapter\n"); return 0; }
    return 1;
}

/* The device features to request: Rae chooses them (lib/webgpu/Context.rae
 * chooseDeviceFeatures, only ones the adapter has) and passes them in before
 * the device exists. A device created first by a C entry point (the old
 * compute helpers) requests none. */
#define RAE_WGPU_MAX_FEATURES 8
static WGPUFeatureName g_wgpu_features[RAE_WGPU_MAX_FEATURES];
static size_t g_wgpu_feature_count = 0;
void rae_wgpu_set_device_features(const int32_t* features, int64_t count) {
    g_wgpu_feature_count = 0;
    for (int64_t i = 0; i < count && i < RAE_WGPU_MAX_FEATURES; i++)
        g_wgpu_features[g_wgpu_feature_count++] = (WGPUFeatureName)features[i];
}
void* rae_wgpu_ctx_adapter_ready(void) { return rae_wgpu_adapter_init() ? (void*)g_wgpu_adapter : NULL; }

/* Pipelines the C runtime created (the forward renderer's and the old compute
 * helpers'): lib/gpu/PipelineWatch.rae compares it each frame to spot one
 * created after the first frame (render-perf-13). */
static int64_t g_c_pipelines_created = 0;
int64_t rae_c_pipelines_created(void) { return g_c_pipelines_created; }

static int rae_wgpu_init(void) {
    if (g_wgpu_dev) return 1;
    if (!rae_wgpu_adapter_init()) return 0;
    WGPUDeviceDescriptor dd; memset(&dd, 0, sizeof(dd));
    dd.requiredFeatureCount = g_wgpu_feature_count;
    dd.requiredFeatures = g_wgpu_feature_count ? g_wgpu_features : NULL;
    dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    dd.deviceLostCallbackInfo.callback = rae_wgpu_on_device_lost;
    dd.uncapturedErrorCallbackInfo.callback = rae_wgpu_on_uncaptured_error;
    WGPURequestDeviceCallbackInfo dci; memset(&dci, 0, sizeof(dci));
    dci.mode = WGPUCallbackMode_AllowProcessEvents; dci.callback = rae_wgpu_on_device;
    wgpuAdapterRequestDevice(g_wgpu_adapter, &dd, dci);
    while (!g_wgpu_device_done) rae_wgpu_poll(1);
    if (!g_wgpu_dev) { fprintf(stderr, "[wgpu] no device\n"); return 0; }
    g_wgpu_queue = wgpuDeviceGetQueue(g_wgpu_dev);
    /* What the device has, for the renderers' fallbacks: rg11b10ufloat
     * renderable HDR targets (#370, else rgba16float), timestamp queries
     * (#528, else no GPU timing). */
    g_wgpu_have_rg11b10 = wgpuDeviceHasFeature(g_wgpu_dev, WGPUFeatureName_RG11B10UfloatRenderable) ? 1 : 0;
    g_wgpu_have_timestamp = wgpuDeviceHasFeature(g_wgpu_dev, (WGPUFeatureName)WGPUFeatureName_TimestampQuery) ? 1 : 0;
    return 1;
}

/* ---- Context bootstrap accessors (#501) -----------------------------------
 * The async instance/adapter/device REQUEST (and any platform surface config)
 * genuinely must stay in C — WebGPU's request calls are callback-driven and
 * passing a Rae closure to C is a separate future feature. These functions run
 * that bootstrap once and hand the resulting handles to Rae as opaque Ptr, so
 * every downstream resource call (createBuffer/queueWriteBuffer/…) is made from
 * Rae over the generated bindings, not from a C shim. Symbols are bound in Rae
 * via extern("…") from lib/webgpu/context.rae. */
int   rae_wgpu_ctx_bootstrap(void) { return rae_wgpu_init(); }
void* rae_wgpu_ctx_device(void)     { return (void*)g_wgpu_dev; }
void* rae_wgpu_ctx_queue(void)      { return (void*)g_wgpu_queue; }
void* rae_wgpu_ctx_adapter(void)    { return (void*)g_wgpu_adapter; }
void* rae_wgpu_ctx_instance(void)   { return (void*)g_wgpu_inst; }
/* Advance the device's event queue (map/submit callbacks). wait!=0 blocks. */
void  rae_wgpu_ctx_poll(int wait)   { rae_wgpu_poll(wait); }

/* Per-submission completion (#938). wgpuDevicePoll's return value means "the
 * queue is EMPTY", not "that submission index finished" (wgpu.h says so), so
 * a consumer that submits every frame while another keeps the GPU busy never
 * saw its own work retire: its manager slots filled, submit failed, and the
 * grass field vanished until the queue happened to drain. The honest signal
 * is wgpuQueueOnSubmittedWorkDone, which fires once everything submitted
 * before the call is done; indices are monotonic on the one queue, so the
 * highest index whose callback fired bounds every completed submission. The
 * index rides in userdata1 — no allocation per frame, nothing to free. */
static uint64_t g_wgpu_done_index = 0;
static void rae_wgpu_on_work_done(WGPUQueueWorkDoneStatus st, WGPUStringView m, void* u1, void* u2) {
    (void)st; (void)m; (void)u2;
    uint64_t index = (uint64_t)(uintptr_t)u1;
    if (index > g_wgpu_done_index) g_wgpu_done_index = index;
}
void rae_wgpu_watch_submission(uint64_t index) {
    if (!g_wgpu_queue) return;
    WGPUQueueWorkDoneCallbackInfo ci; memset(&ci, 0, sizeof(ci));
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = rae_wgpu_on_work_done;
    ci.userdata1 = (void*)(uintptr_t)index;
    wgpuQueueOnSubmittedWorkDone(g_wgpu_queue, ci);
}
/* The browser's end of a frame (lib/gpu/FramePacer.rae): it cannot block. A
 * frame callback (RAE_WEB_FRAME_CALLBACK, docs/web-frame-loop.md) is paced by
 * the browser and only processes events; the Asyncify loop yields first.
 * Natively Rae waits itself, with a limit (gpu/GpuFault). */
void rae_wgpu_wait_submission(uint64_t index) {
    (void)index;
#if defined(__EMSCRIPTEN__) && !defined(RAE_WEB_FRAME_CALLBACK)
    rae_wgpu_poll(1);
#else
    rae_wgpu_poll(0);
#endif
}
/* Nonblocking: pumps callbacks, then answers for this index. The queue-empty
 * flag stays as a second yes — when nothing at all is in flight, everything
 * is done regardless of whether its callback has been observed yet. "Done" is
 * also a command buffer that failed (Metal calls its completed handler, which
 * wgpu's fence takes for its end): whether it did is rae_gpu_fault_count. */
uint32_t rae_wgpu_submission_done(uint64_t index) {
    uint32_t queueEmpty = 0;
#ifdef __EMSCRIPTEN__
    if (g_wgpu_inst) wgpuInstanceProcessEvents(g_wgpu_inst);
#else
    if (g_wgpu_dev) queueEmpty = (uint32_t)wgpuDevicePoll(g_wgpu_dev, 0, NULL);
    if (g_wgpu_inst) wgpuInstanceProcessEvents(g_wgpu_inst);
#endif
    if (g_wgpu_test_hang) return 0;
    if (index <= g_wgpu_done_index) return 1;
    return queueEmpty;
}
/* wgpu's own MTLDevice; null elsewhere */
void* rae_wgpu_metal_device(void) {
#if defined(__APPLE__)
    if (g_wgpu_dev) return wgpuDeviceGetNativeMetalDevice(g_wgpu_dev);
#endif
    return NULL;
}

/* After a loss: does a second device take work? 1: an empty submission to its
 * queue ended within `limit_ms` (a failed one ends too: rae_gpu_fault_count
 * tells). Platform reason: a device request answers by a callback. */
static WGPUDevice g_wgpu_probe_dev = NULL;
static int g_wgpu_probe_done = 0;
static int g_wgpu_probe_ended = 0;
static void rae_wgpu_on_probe(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_probe_dev = d; g_wgpu_probe_done = 1;
}
static void rae_wgpu_on_probe_ended(WGPUQueueWorkDoneStatus st, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_probe_ended = 1;
}
int64_t rae_wgpu_new_device_works(int64_t limit_ms) {
#ifdef __EMSCRIPTEN__
    (void)limit_ms;
    return 0;
#else
    if (!g_wgpu_adapter || !g_wgpu_inst) return 0;
    WGPUDeviceDescriptor dd; memset(&dd, 0, sizeof(dd));
    dd.uncapturedErrorCallbackInfo.callback = rae_wgpu_on_uncaptured_error;
    WGPURequestDeviceCallbackInfo dci; memset(&dci, 0, sizeof(dci));
    dci.mode = WGPUCallbackMode_AllowProcessEvents; dci.callback = rae_wgpu_on_probe;
    wgpuAdapterRequestDevice(g_wgpu_adapter, &dd, dci);
    for (int i = 0; i < 100 && !g_wgpu_probe_done; i++) {
        wgpuInstanceProcessEvents(g_wgpu_inst);
        usleep(1000);
    }
    if (!g_wgpu_probe_dev) return 0;
    WGPUQueue queue = wgpuDeviceGetQueue(g_wgpu_probe_dev);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_probe_dev, NULL);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(queue, 1, &cmd);
    WGPUQueueWorkDoneCallbackInfo ci; memset(&ci, 0, sizeof(ci));
    ci.mode = WGPUCallbackMode_AllowProcessEvents; ci.callback = rae_wgpu_on_probe_ended;
    wgpuQueueOnSubmittedWorkDone(queue, ci);
    for (int64_t ms = 0; ms < limit_ms && !g_wgpu_probe_ended; ms++) {
        wgpuDevicePoll(g_wgpu_probe_dev, 0, NULL);
        wgpuInstanceProcessEvents(g_wgpu_inst);
        if (!g_wgpu_probe_ended) usleep(1000);
    }
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    wgpuQueueRelease(queue);
    return g_wgpu_probe_ended;
#endif
}

/* A null opaque pointer, for WebGPU params that take an optional pointer/array
 * (e.g. dynamicOffsets when the count is 0). Rae has no null-Ptr literal yet. */
void* rae_wgpu_null_ptr(void)       { return (void*)0; }

/* #528: did the device get the timestamp-query features? Rae skips GPU timing
 * when 0 (e.g. an adapter that doesn't offer them). */
int rae_wgpu_have_timestamp(void)   { return g_wgpu_have_timestamp; }
int rae_wgpu_have_rg11b10(void)     { return g_wgpu_have_rg11b10; }   /* #923: the Rae lit target picks its format */

/* Live wgpu-native object counts per type. numAllocated is what the backend
 * currently holds; a per-frame climb pinpoints a leaked resource type that Rae's
 * own alloc tracking can't see (it does not track wgpu objects). Env-gated
 * (RAE_WGPU_REPORT) and dumped periodically from the present path, like
 * RAE_MEM_STATS — off by default, a diagnostic for the next leak hunt. */
/* Periodic live-object report from the present paths (2D and 3D), every 120
 * frames. OPT-IN: RAE_WGPU_REPORT=1 (or any value but 0/off) turns it on. It
 * was on by default while the ownership slices (#920, #923, #925) and the
 * 119 leak hunt confirmed their in-flight teardown — every count came back
 * flat, so it is a diagnostic for the next leak hunt again, not frame noise
 * in every log. */
void rae_wgpu_report_periodic(void) {
    static int enabled = -1; static long frame = 0;
    if (enabled < 0) {
        const char* env = getenv("RAE_WGPU_REPORT");
        enabled = (env && env[0] && strcmp(env, "0") != 0 && strcmp(env, "off") != 0) ? 1 : 0;
    }
    if (enabled && (frame++ % 120) == 0) {
        char tag[24]; snprintf(tag, sizeof(tag), "f%ld", frame - 1);
        rae_wgpu_report(tag);
    }
}

void rae_wgpu_report(const char* tag) {
#ifdef __EMSCRIPTEN__
    /* wgpu-native's object report; the browser's WebGPU has none */
    (void)tag;
    return;
#else
    if (!g_wgpu_inst) return;
    WGPUGlobalReport r; memset(&r, 0, sizeof(r));
    wgpuGenerateReport(g_wgpu_inst, &r);
    fprintf(stderr, "[wgpu-report %s] buf=%zu tex=%zu texView=%zu bind=%zu bindLayout=%zu "
        "shader=%zu rpipe=%zu cpipe=%zu cmdbuf=%zu query=%zu sampler=%zu surface=%zu\n",
        tag ? tag : "",
        r.hub.buffers.numAllocated, r.hub.textures.numAllocated, r.hub.textureViews.numAllocated,
        r.hub.bindGroups.numAllocated, r.hub.bindGroupLayouts.numAllocated,
        r.hub.shaderModules.numAllocated, r.hub.renderPipelines.numAllocated,
        r.hub.computePipelines.numAllocated, r.hub.commandBuffers.numAllocated,
        r.hub.querySets.numAllocated, r.hub.samplers.numAllocated, r.surfaces.numAllocated);
    fflush(stderr);
#endif
}

/* #528's handle store for lib/GpuTiming.rae (query set + 2 buffers) is gone
 * (#923): the Rae GpuTiming owner holds its handles as fields. No global GPU
 * slot table is left in the runtime. */

/* #528: blocking read of a MapRead buffer's [0,size) into `dst`. This is the
 * ONE piece that must stay in C: wgpuBufferMapAsync needs a C callback pointer,
 * which Rae cannot yet synthesize (same reason rae_gb_submit exists for &cmd).
 * Everything else — query set, timestamp writes, resolve, copy — is done from
 * Rae over the bindings. Used on a buffer whose GPU work is already a frame old,
 * so the poll returns without a real stall. Returns 1 on success. */
int rae_wgpu_map_read(void* buffer, uint64_t size, void* dst) {
    if (!buffer || !dst || size == 0) return 0;
    WGPUBuffer buf = (WGPUBuffer)buffer;
    g_wgpu_map_done = 0;
    WGPUBufferMapCallbackInfo ci; memset(&ci, 0, sizeof(ci));
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = rae_wgpu_on_map;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, (size_t)size, ci);
    int spins = 0;
    while (!g_wgpu_map_done && !g_wgpu_device_lost && spins++ < 100000) rae_wgpu_poll(1);
    if (!g_wgpu_map_done) return 0;
    const void* src = wgpuBufferGetConstMappedRange(buf, 0, (size_t)size);
    if (!src) { wgpuBufferUnmap(buf); return 0; }
    memcpy(dst, src, (size_t)size);
    wgpuBufferUnmap(buf);
    return 1;
}

#include "runtime_webgpu_readback.c"

/* scene: sceneLen f64 (camera 19 + spheres*10) -> narrowed to f32 for the GPU.
 * fb: width*height int64 written as packed 0xRRGGBB. wgsl: shader source. */
void rae_ext_Webgpu_raytrace(const float* scene, int64_t sceneLen, int64_t* fb,
                            int64_t width, int64_t height, int64_t samples,
                            int64_t maxDepth, rae_String wgsl){
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
    g_c_pipelines_created++;
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
    while (!g_wgpu_map_done && !g_wgpu_device_lost) rae_wgpu_poll(1);
    const uint32_t* px = g_wgpu_map_done ? (const uint32_t*)wgpuBufferGetConstMappedRange(rbuf, 0, obytes) : NULL;
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

int64_t rae_ext_Gpu_storageF32(const float* data, int64_t count){
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
int64_t rae_ext_Gpu_uniformU32(const int64_t* data, int64_t count) {
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
int64_t rae_ext_Gpu_allocF32(int64_t count) { return rae_gpu_alloc(count); }
int64_t rae_ext_Gpu_allocU32(int64_t count) { return rae_gpu_alloc(count); }

void rae_ext_Gpu_writeF32(int64_t buf, const float* data, int64_t count){
    if (!g_wgpu_queue || buf < 1 || buf > g_gpu_buf_n || count <= 0) return;
    size_t bytes = (size_t)count * 4;
    float* tmp = (float*)malloc(bytes);
    if (!tmp) return;
    for (int64_t i = 0; i < count; i++) tmp[i] = (float)data[i];
    wgpuQueueWriteBuffer(g_wgpu_queue, g_gpu_buf[buf - 1], 0, tmp, bytes);
    free(tmp);
}
void rae_ext_Gpu_writeU32(int64_t buf, const int64_t* data, int64_t count) {
    if (!g_wgpu_queue || buf < 1 || buf > g_gpu_buf_n || count <= 0) return;
    size_t bytes = (size_t)count * 4;
    uint32_t* tmp = (uint32_t*)malloc(bytes);
    if (!tmp) return;
    for (int64_t i = 0; i < count; i++) tmp[i] = (uint32_t)data[i];
    wgpuQueueWriteBuffer(g_wgpu_queue, g_gpu_buf[buf - 1], 0, tmp, bytes);
    free(tmp);
}

int64_t rae_ext_Gpu_kernel(rae_String wgsl, rae_String entry) {
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
    g_c_pipelines_created++;
    wgpuShaderModuleRelease(mod);
    if (!pipe) { fprintf(stderr, "[gpu] kernel compile failed\n"); return 0; }
    g_gpu_pipe[g_gpu_pipe_n] = pipe;
    return ++g_gpu_pipe_n;  /* 1-based */
}

void rae_ext_Gpu_run(int64_t kernel, const int64_t* bufs, int64_t bufCount,
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
    rae_wgpu_poll(1);  /* wait/yield for completion */
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
    while (!g_wgpu_map_done && !g_wgpu_device_lost) rae_wgpu_poll(1);
    *staging_out = staging;
    if (!g_wgpu_map_done) return NULL;
    return wgpuBufferGetConstMappedRange(staging, 0, bytes);
}
void rae_ext_Gpu_downloadF32(int64_t buf, float* out, int64_t count){
    if (!out || count <= 0) return;
    WGPUBuffer staging = NULL;
    const float* p = (const float*)rae_gpu_readback(buf, (size_t)count * 4, &staging);
    if (p) for (int64_t i = 0; i < count; i++) out[i] = (double)p[i];
    if (staging) { wgpuBufferUnmap(staging); wgpuBufferRelease(staging); }
}
void rae_ext_Gpu_downloadU32(int64_t buf, int64_t* out, int64_t count) {
    if (!out || count <= 0) return;
    WGPUBuffer staging = NULL;
    const uint32_t* p = (const uint32_t*)rae_gpu_readback(buf, (size_t)count * 4, &staging);
    if (p) for (int64_t i = 0; i < count; i++) out[i] = (int64_t)p[i];
    if (staging) { wgpuBufferUnmap(staging); wgpuBufferRelease(staging); }
}
void rae_ext_Gpu_reset(void) {
    for (int i = 0; i < g_gpu_buf_n; i++) if (g_gpu_buf[i]) wgpuBufferRelease(g_gpu_buf[i]);
    for (int i = 0; i < g_gpu_pipe_n; i++) if (g_gpu_pipe[i]) wgpuComputePipelineRelease(g_gpu_pipe[i]);
    g_gpu_buf_n = 0; g_gpu_pipe_n = 0;
}
