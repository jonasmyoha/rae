/* A Metal command buffer that failed on the GPU, said as it happens (lib/gpu/GpuFault.rae).
 *
 * Platform reason: wgpu reports no failed command buffer (its C API has no command buffer, and it
 * neither logs one nor calls the device-lost callback), while Metal's own completed handler gets
 * its status and error. -commit of the command buffer class the device's buffers have (found from a
 * queue of the same device) is replaced by one that adds such a handler and then calls the original, as
 * runtime_gpu_metal_timing.c does when asked. The device is wgpu's own, else the system's default
 * (wgpuQueueGetNativeMetalCommandQueue answers NULL in this wgpu-native). RAE_GPU_FAULT_WATCH=0
 * leaves it out.
 *
 * The handler runs on a thread of Metal's: it writes the error on stderr at once (the main thread
 * may hang) and keeps its code. What the codes mean, and what to do, is Rae's.
 *
 * RAE_GPU_ENCODER_STATUS=1 (a diagnostic: it slows the GPU's work down) also asks Metal for each
 * encoder's state in a failed buffer (errorOptions EncoderExecutionStatus), and names each encoder by
 * the first pipeline it sets, so the error's lines name the guilty pass. */
#include <stdint.h>
#if defined(__APPLE__) && defined(RAE_HAS_WEBGPU)
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAE_GPU_FAULTS_KEPT 16
static IMP g_fault_commit = NULL;
static _Atomic int64_t g_fault_completed = 0;
static _Atomic int64_t g_fault_count = 0;
static _Atomic int64_t g_fault_codes[RAE_GPU_FAULTS_KEPT];

static const char* rae_ns_text(void* string) {
    if (!string) return "";
    const char* text = ((const char* (*)(void*, SEL))objc_msgSend)(string, sel_registerName("UTF8String"));
    return text ? text : "";
}

static void rae_gpu_fault_note(int64_t code) {
    int64_t at = atomic_fetch_add(&g_fault_count, 1);
    atomic_store(&g_fault_codes[at % RAE_GPU_FAULTS_KEPT], code);
}

/* The encoders' states of a failed buffer (RAE_GPU_ENCODER_STATUS) */
static void rae_gpu_fault_encoders(void* error) {
    void* info = ((void* (*)(void*, SEL))objc_msgSend)(error, sel_registerName("userInfo"));
    if (!info) return;
    void* key = ((void* (*)(void*, SEL, const char*))objc_msgSend)(
        (void*)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "MTLCommandBufferEncoderInfoErrorKey");
    void* list = ((void* (*)(void*, SEL, void*))objc_msgSend)(info, sel_registerName("objectForKey:"), key);
    if (!list) return;
    static const char* states[] = { "unknown", "completed", "affected", "pending", "faulted" };
    unsigned long count = ((unsigned long (*)(void*, SEL))objc_msgSend)(list, sel_registerName("count"));
    for (unsigned long i = 0; i < count; i++) {
        void* one = ((void* (*)(void*, SEL, unsigned long))objc_msgSend)(list, sel_registerName("objectAtIndex:"), i);
        long state = ((long (*)(void*, SEL))objc_msgSend)(one, sel_registerName("errorState"));
        void* label = ((void* (*)(void*, SEL))objc_msgSend)(one, sel_registerName("label"));
        fprintf(stderr, "[gpu]   encoder %lu: %s, '%s'\n", i, state >= 0 && state <= 4 ? states[state] : "?", rae_ns_text(label));
    }
}

static void rae_gpu_fault_completed(void* buffer) {
    atomic_fetch_add(&g_fault_completed, 1);
    unsigned long status = ((unsigned long (*)(void*, SEL))objc_msgSend)(buffer, sel_registerName("status"));
    if (status != 5) return;   /* MTLCommandBufferStatusError */
    void* error = ((void* (*)(void*, SEL))objc_msgSend)(buffer, sel_registerName("error"));
    int64_t code = 0;
    if (error) code = (int64_t)((long (*)(void*, SEL))objc_msgSend)(error, sel_registerName("code"));
    /* a denied app's every buffer fails: the first ones are enough */
    if (atomic_load(&g_fault_count) < RAE_GPU_FAULTS_KEPT) {
        void* text = error ? ((void* (*)(void*, SEL))objc_msgSend)(error, sel_registerName("localizedDescription")) : NULL;
        fprintf(stderr, "[gpu] a command buffer failed (code %lld): %s\n", (long long)code, rae_ns_text(text));
        if (error) rae_gpu_fault_encoders(error);
    }
    rae_gpu_fault_note(code);
}

static void rae_gpu_fault_commit(void* self, SEL cmd) {
    void (^completed)(void*) = ^(void* buffer) { rae_gpu_fault_completed(buffer); };
    ((void (*)(void*, SEL, void*))objc_msgSend)(self, sel_registerName("addCompletedHandler:"), (void*)completed);
    ((void (*)(void*, SEL))g_fault_commit)(self, cmd);
}

/* ---- RAE_GPU_ENCODER_STATUS ---- */
static IMP g_fault_render_encoder = NULL, g_fault_compute_encoder = NULL;
static IMP g_fault_set_render = NULL, g_fault_set_compute = NULL;

static void rae_gpu_name_encoder(void* encoder, void* state) {
    void* had = ((void* (*)(void*, SEL))objc_msgSend)(encoder, sel_registerName("label"));
    if (had && rae_ns_text(had)[0]) return;
    void* label = state ? ((void* (*)(void*, SEL))objc_msgSend)(state, sel_registerName("label")) : NULL;
    if (label) ((void (*)(void*, SEL, void*))objc_msgSend)(encoder, sel_registerName("setLabel:"), label);
}
static void rae_gpu_set_render(void* self, SEL cmd, void* state) {
    rae_gpu_name_encoder(self, state);
    ((void (*)(void*, SEL, void*))g_fault_set_render)(self, cmd, state);
}
static void rae_gpu_set_compute(void* self, SEL cmd, void* state) {
    rae_gpu_name_encoder(self, state);
    ((void (*)(void*, SEL, void*))g_fault_set_compute)(self, cmd, state);
}
static void rae_gpu_hook(Class on, const char* selector, IMP with, IMP* original) {
    if (*original) return;
    Method method = class_getInstanceMethod(on, sel_registerName(selector));
    if (!method) return;
    *original = method_getImplementation(method);
    class_replaceMethod(on, sel_registerName(selector), with, method_getTypeEncoding(method));
}
static void* rae_gpu_render_encoder(void* self, SEL cmd, void* descriptor) {
    void* encoder = ((void* (*)(void*, SEL, void*))g_fault_render_encoder)(self, cmd, descriptor);
    if (encoder) rae_gpu_hook(object_getClass((id)encoder), "setRenderPipelineState:", (IMP)rae_gpu_set_render, &g_fault_set_render);
    return encoder;
}
static void* rae_gpu_compute_encoder(void* self, SEL cmd, void* descriptor) {
    void* encoder = ((void* (*)(void*, SEL, void*))g_fault_compute_encoder)(self, cmd, descriptor);
    if (encoder) rae_gpu_hook(object_getClass((id)encoder), "setComputePipelineState:", (IMP)rae_gpu_set_compute, &g_fault_set_compute);
    return encoder;
}
static void* rae_gpu_status_buffer(void* self, SEL cmd) {
    (void)cmd;
    void* descriptor = ((void* (*)(void*, SEL))objc_msgSend)((void*)objc_getClass("MTLCommandBufferDescriptor"), sel_registerName("new"));
    ((void (*)(void*, SEL, signed char))objc_msgSend)(descriptor, sel_registerName("setRetainedReferences:"), 0);
    ((void (*)(void*, SEL, unsigned long))objc_msgSend)(descriptor, sel_registerName("setErrorOptions:"), 1);
    void* buffer = ((void* (*)(void*, SEL, void*))objc_msgSend)(self, sel_registerName("commandBufferWithDescriptor:"), descriptor);
    ((void (*)(void*, SEL))objc_msgSend)(descriptor, sel_registerName("release"));
    return buffer;
}

extern void* MTLCreateSystemDefaultDevice(void);

/* 1 when failed buffers of `device` (an MTLDevice; null: the system's default) are seen from now on */
int64_t rae_gpu_fault_watch(void* device, int64_t encoders) {
    if (g_fault_commit) return 1;
    void* made_device = NULL;
    if (!device) device = made_device = MTLCreateSystemDefaultDevice();
    if (!device) return 0;
    void* queue = ((void* (*)(void*, SEL))objc_msgSend)(device, sel_registerName("newCommandQueue"));
    void* buffer = queue ? ((void* (*)(void*, SEL))objc_msgSend)(queue, sel_registerName("commandBufferWithUnretainedReferences")) : NULL;
    if (buffer) {
        Class bufferClass = object_getClass((id)buffer);
        rae_gpu_hook(bufferClass, "commit", (IMP)rae_gpu_fault_commit, &g_fault_commit);
        if (encoders) {
            static IMP made = NULL;
            rae_gpu_hook(bufferClass, "renderCommandEncoderWithDescriptor:", (IMP)rae_gpu_render_encoder, &g_fault_render_encoder);
            rae_gpu_hook(bufferClass, "computeCommandEncoderWithDescriptor:", (IMP)rae_gpu_compute_encoder, &g_fault_compute_encoder);
            rae_gpu_hook(object_getClass((id)queue), "commandBufferWithUnretainedReferences", (IMP)rae_gpu_status_buffer, &made);
        }
    }
    if (queue) ((void (*)(void*, SEL))objc_msgSend)(queue, sel_registerName("release"));
    if (made_device) ((void (*)(void*, SEL))objc_msgSend)(made_device, sel_registerName("release"));
    return g_fault_commit ? 1 : 0;
}
int64_t rae_gpu_fault_buffers(void) { return atomic_load(&g_fault_completed); }
int64_t rae_gpu_fault_count(void) { return atomic_load(&g_fault_count); }
int64_t rae_gpu_fault_code(int64_t index) {
    if (index < 0) return 0;
    return atomic_load(&g_fault_codes[index % RAE_GPU_FAULTS_KEPT]);
}
/* For a check: a fault as the handler would note it */
void rae_gpu_fault_test(int64_t code) { rae_gpu_fault_note(code); }
#else
static int64_t g_fault_test_count = 0, g_fault_test_code = 0;
int64_t rae_gpu_fault_watch(void* device, int64_t encoders) { (void)device; (void)encoders; return 0; }
int64_t rae_gpu_fault_buffers(void) { return 0; }
int64_t rae_gpu_fault_count(void) { return g_fault_test_count; }
int64_t rae_gpu_fault_code(int64_t index) { (void)index; return g_fault_test_code; }
void rae_gpu_fault_test(int64_t code) { g_fault_test_count++; g_fault_test_code = code; }
#endif
