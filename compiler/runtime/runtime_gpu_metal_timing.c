/* The GPU time of every Metal command buffer the program commits, for
 * RAE_GPU2D_TIMING on Apple (lib/Gpu2dCanvasTiming.rae). Off unless asked.
 *
 * Platform reason: on an iPhone 15 Pro a render pass's own timestamps read 0
 * and a timestamp mark around the pass does not wait for its tile work, while
 * a command buffer's GPUStartTime and GPUEndTime (documented Metal) give the
 * GPU's real time (docs/ui-animation-design.md section 10). wgpu-native does
 * not expose its MTLCommandBuffer, so -commit of the command buffer class this
 * device uses (found from a command buffer of its own) is replaced by one that
 * adds a completed handler and then calls the original. It is installed only
 * by rae_gpu_metal_timing_start, which RAE_GPU2D_TIMING alone calls.
 *
 * The handler adds each buffer's GPU time to counters; Rae takes them once a
 * second (take, then taken(0..2): buffers, total ns, longest ns). */
#include <stdint.h>
#if defined(__APPLE__) && defined(RAE_HAS_WEBGPU)
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdatomic.h>

extern void* MTLCreateSystemDefaultDevice(void);

static IMP g_metal_commit = NULL;
static _Atomic int64_t g_metal_buffers = 0;
static _Atomic int64_t g_metal_total_ns = 0;
static _Atomic int64_t g_metal_longest_ns = 0;
static int64_t g_metal_taken[3] = { 0, 0, 0 };

static void rae_metal_commit_timed(void* self, SEL cmd) {
    void (^completed)(void*) = ^(void* buffer) {
        double start = ((double (*)(void*, SEL))objc_msgSend)(buffer, sel_registerName("GPUStartTime"));
        double end = ((double (*)(void*, SEL))objc_msgSend)(buffer, sel_registerName("GPUEndTime"));
        if (start <= 0.0 || end <= start) return;
        int64_t ns = (int64_t)((end - start) * 1e9);
        atomic_fetch_add(&g_metal_buffers, 1);
        atomic_fetch_add(&g_metal_total_ns, ns);
        int64_t longest = atomic_load(&g_metal_longest_ns);
        while (ns > longest && !atomic_compare_exchange_weak(&g_metal_longest_ns, &longest, ns)) {
        }
    };
    ((void (*)(void*, SEL, void*))objc_msgSend)(self, sel_registerName("addCompletedHandler:"), (void*)completed);
    ((void (*)(void*, SEL))g_metal_commit)(self, cmd);
}

/* 1 when the counting is on (already, or now), 0 when it cannot be */
int64_t rae_gpu_metal_timing_start(void) {
    if (g_metal_commit) return 1;
    void* device = MTLCreateSystemDefaultDevice();
    if (!device) return 0;
    void* queue = ((void* (*)(void*, SEL))objc_msgSend)(device, sel_registerName("newCommandQueue"));
    void* buffer = queue ? ((void* (*)(void*, SEL))objc_msgSend)(queue, sel_registerName("commandBuffer")) : NULL;
    int64_t started = 0;
    if (buffer) {
        Class bufferClass = object_getClass((id)buffer);
        Method commit = class_getInstanceMethod(bufferClass, sel_registerName("commit"));
        if (commit) {
            g_metal_commit = method_getImplementation(commit);
            class_replaceMethod(bufferClass, sel_registerName("commit"), (IMP)rae_metal_commit_timed, method_getTypeEncoding(commit));
            started = 1;
        }
    }
    if (queue) ((void (*)(void*, SEL))objc_msgSend)(queue, sel_registerName("release"));
    ((void (*)(void*, SEL))objc_msgSend)(device, sel_registerName("release"));
    return started;
}

void rae_gpu_metal_timing_take(void) {
    g_metal_taken[0] = atomic_exchange(&g_metal_buffers, 0);
    g_metal_taken[1] = atomic_exchange(&g_metal_total_ns, 0);
    g_metal_taken[2] = atomic_exchange(&g_metal_longest_ns, 0);
}
#else
int64_t rae_gpu_metal_timing_start(void) { return 0; }
void rae_gpu_metal_timing_take(void) {}
static int64_t g_metal_taken[3] = { 0, 0, 0 };
#endif

int64_t rae_gpu_metal_timing_taken(int64_t index) {
    if (index < 0 || index > 2) return 0;
    return g_metal_taken[index];
}
