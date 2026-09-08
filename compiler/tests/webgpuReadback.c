/* Deterministic callback ordering test for the platform request bridge.
 * No device: mock callbacks can arrive inline or after owner cancellation.
 * Run with ASan/UBSan; see tools/test-webgpu-readback.sh.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int WGPUMapAsyncStatus;
typedef struct { const char* data; size_t length; } WGPUStringView;
typedef struct {
    int mode;
    void (*callback)(WGPUMapAsyncStatus, WGPUStringView, void*, void*);
    void* userdata1;
} WGPUBufferMapCallbackInfo;
typedef struct {
    int references;
    int state;
    int inlineFailure;
    int inlineCancel;
    uint64_t data[4];
    WGPUBufferMapCallbackInfo callback;
} MockBuffer;
typedef MockBuffer* WGPUBuffer;
enum { WGPUMapAsyncStatus_Success = 1, WGPUBufferMapState_Unmapped = 0,
       WGPUBufferUsage_MapRead = 1, WGPUMapMode_Read = 1,
       WGPUCallbackMode_AllowProcessEvents = 2 };
static int liveRequests;
static int polls;
static void* requestAllocate(size_t count, size_t size) {
    void* result = calloc(count, size);
    if (result) liveRequests++;
    return result;
}
static void requestFree(void* request) { liveRequests--; free(request); }
static void rae_wgpu_poll(int wait) { assert(wait == 0); polls++; }
static uint64_t wgpuBufferGetSize(WGPUBuffer buffer) { (void)buffer; return 32; }
static uint64_t wgpuBufferGetUsage(WGPUBuffer buffer) { (void)buffer; return WGPUBufferUsage_MapRead; }
static void wgpuBufferAddRef(WGPUBuffer buffer) { buffer->references++; }
static void wgpuBufferRelease(WGPUBuffer buffer) { assert(buffer->references > 0); buffer->references--; }
static void deliver(WGPUBuffer buffer, int status) {
    WGPUBufferMapCallbackInfo callback = buffer->callback;
    buffer->callback.callback = NULL;
    buffer->state = status == WGPUMapAsyncStatus_Success ? 2 : 0;
    callback.callback(status, (WGPUStringView){0}, callback.userdata1, NULL);
}
static void wgpuBufferMapAsync(WGPUBuffer buffer, int mode, size_t offset,
                               size_t size, WGPUBufferMapCallbackInfo callback) {
    (void)mode; (void)offset; (void)size;
    assert(callback.mode == WGPUCallbackMode_AllowProcessEvents);
    buffer->state = 1;
    buffer->callback = callback;
    if (buffer->inlineFailure) deliver(buffer, -1);
}
static void wgpuBufferUnmap(WGPUBuffer buffer) {
    buffer->state = 0;
    if (buffer->inlineCancel && buffer->callback.callback) deliver(buffer, -1);
}
static const void* wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
    assert(buffer->state == 2 && offset + size <= 32);
    return (const char*)buffer->data + offset;
}
#define calloc requestAllocate
#define free requestFree
#include "../runtime/runtime_webgpu_readback.c"
#undef calloc
#undef free

int main(void) {
    MockBuffer first = {.references = 1, .data = {11, 22, 33, 44}};
    MockBuffer second = {.references = 1, .data = {55, 66, 77, 88}};
    void* firstRead = rae_wgpu_read_start(&first, 8, 16);
    void* secondRead = rae_wgpu_read_start(&second, 0, 32);
    uint64_t destination[4] = {0};
    assert(rae_wgpu_read_poll(firstRead) == 0 && polls == 1);
    assert(!rae_wgpu_read_copy(firstRead, destination, sizeof(destination)));
    deliver(&second, WGPUMapAsyncStatus_Success);
    assert(rae_wgpu_read_poll(firstRead) == 0);
    assert(rae_wgpu_read_poll(secondRead) == 1);
    deliver(&first, WGPUMapAsyncStatus_Success);
    assert(!rae_wgpu_read_copy(firstRead, destination, 8));
    assert(rae_wgpu_read_copy(firstRead, destination, 16));
    assert(destination[0] == 22 && destination[1] == 33 && destination[2] == 0);
    rae_wgpu_read_release(firstRead);
    assert(rae_wgpu_read_copy(secondRead, destination, 32));
    assert(destination[0] == 55 && destination[3] == 88);
    rae_wgpu_read_release(secondRead);
    assert(liveRequests == 0 && first.references == 1 && second.references == 1);

    /* Deferred cancellation callback is the last owner of BOTH allocations. */
    firstRead = rae_wgpu_read_start(&first, 0, 32);
    rae_wgpu_read_release(firstRead);
    wgpuBufferRelease(&first);
    assert(liveRequests == 1 && first.references == 1);
    deliver(&first, -1);
    assert(liveRequests == 0 && first.references == 0);

    second.inlineCancel = 1;
    secondRead = rae_wgpu_read_start(&second, 0, 32);
    rae_wgpu_read_release(secondRead);
    assert(liveRequests == 0 && second.references == 1);
    second.inlineFailure = 1;
    secondRead = rae_wgpu_read_start(&second, 0, 32);
    assert(rae_wgpu_read_poll(secondRead) == -1);
    rae_wgpu_read_release(secondRead);
    assert(liveRequests == 0 && second.references == 1);

    const uint64_t offsets[] = {0, 4, 32, UINT64_MAX, 8};
    const uint64_t sizes[] = {0, 4, 4, 4, UINT64_MAX};
    for (size_t i = 0; i < 5; i++) {
        void* invalid = rae_wgpu_read_start(&second, offsets[i], sizes[i]);
        assert(rae_wgpu_read_poll(invalid) == -1);
        rae_wgpu_read_release(invalid);
    }
    assert(liveRequests == 0 && second.references == 1);
    assert(rae_wgpu_read_poll(NULL) == -1);
    rae_wgpu_read_release(NULL);
    puts("readback callback ownership and nonblocking polling OK");
}
