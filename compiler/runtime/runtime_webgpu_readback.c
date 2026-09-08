/* Owned asynchronous MapRead requests. Included by runtime_webgpu.c.
 * Platform callback ABI only; staging allocation and submission remain in Rae.
 * Calls belong to the context thread. AllowProcessEvents callbacks execute on
 * that thread, so owner/callback references need no cross-thread synchronization.
 */
typedef struct RaeWgpuReadRequest {
    WGPUBuffer buffer;
    size_t offset;
    size_t size;
    int status; /* 0 pending, 1 success, -1 failed/cancelled */
    int references;
} RaeWgpuReadRequest;

static void raeWgpuReadDrop(RaeWgpuReadRequest* request) {
    if (--request->references != 0) return;
    if (request->buffer) wgpuBufferRelease(request->buffer);
    free(request);
}

static void raeWgpuReadCompleted(WGPUMapAsyncStatus status, WGPUStringView message,
                                  void* userdata1, void* userdata2) {
    (void)message;
    (void)userdata2;
    RaeWgpuReadRequest* request = userdata1;
    /* Cancellation may have happened before this callback, including inline
     * during Unmap. Never publish success again on a cancelled request. */
    if (request->status == 0)
        request->status = status == WGPUMapAsyncStatus_Success ? 1 : -1;
    raeWgpuReadDrop(request);
}

void* rae_wgpu_read_start(void* buffer, uint64_t offset, uint64_t size) {
    RaeWgpuReadRequest* request = calloc(1, sizeof(*request));
    if (!request) return NULL;
    request->references = 1;
    request->status = -1;
    WGPUBuffer mappedBuffer = (WGPUBuffer)buffer;
    if (!mappedBuffer || !size || offset % 8 || size % 4 ||
        offset > SIZE_MAX || size > SIZE_MAX) return request;
    uint64_t bufferSize = wgpuBufferGetSize(mappedBuffer);
    if (offset > bufferSize || size > bufferSize - offset ||
        !(wgpuBufferGetUsage(mappedBuffer) & WGPUBufferUsage_MapRead))
        return request;
    /* The caller exclusively owns this buffer's map lifecycle. Do not query
     * GetMapState here: the pinned wgpu-native exports it but aborts as an
     * unimplemented entry point. MapAsync reports asynchronous failures. */
    request->buffer = mappedBuffer;
    request->offset = (size_t)offset;
    request->size = (size_t)size;
    request->status = 0;
    wgpuBufferAddRef(mappedBuffer);
    /* Register the callback reference BEFORE MapAsync: failure can complete
     * inline. The owner reference keeps start/release safe in that case. */
    request->references++;
    WGPUBufferMapCallbackInfo callback = {0};
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.callback = raeWgpuReadCompleted;
    callback.userdata1 = request;
    wgpuBufferMapAsync(mappedBuffer, WGPUMapMode_Read, request->offset,
                       request->size, callback);
    return request;
}

int rae_wgpu_read_poll(void* handle) {
    RaeWgpuReadRequest* request = handle;
    if (!request) return -1;
    rae_wgpu_poll(0);
    return request->status;
}

int rae_wgpu_read_copy(void* handle, void* destination, uint64_t capacity) {
    RaeWgpuReadRequest* request = handle;
    if (!request || request->status != 1 || !destination || capacity < request->size)
        return 0;
    const void* source = wgpuBufferGetConstMappedRange(request->buffer,
                                                     request->offset, request->size);
    if (!source) { request->status = -1; return 0; }
    memcpy(destination, source, request->size);
    return 1;
}

void* rae_wgpu_read_release(void* handle) {
    RaeWgpuReadRequest* request = handle;
    if (!request) return NULL;
    int previousStatus = request->status;
    request->status = -1;
    if (request->buffer && previousStatus >= 0) wgpuBufferUnmap(request->buffer);
    /* Pending callback owns its reference until it arrives, even after the
     * caller releases both this request and its original buffer reference. */
    raeWgpuReadDrop(request);
    return NULL;
}
