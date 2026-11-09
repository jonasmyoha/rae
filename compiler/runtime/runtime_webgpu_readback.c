/* Owned asynchronous MapRead requests. Included by runtime_webgpu.c.
 * Platform callback ABI only; staging allocation and submission remain in Rae.
 * Calls belong to the context thread. AllowProcessEvents callbacks execute on
 * that thread, so owner/callback references need no cross-thread synchronization.
 */
typedef struct RaeWgpuReadRequest {
    WGPUBuffer buffer;
    size_t offset;
    size_t size;
    int status; /* 0 pending, 1 success, -1 failed/canceled */
    int references;
    struct RaeWgpuReadRequest* nextDeferred;
} RaeWgpuReadRequest;

/* Requests whose last reference went in a map callback. wgpu runs map
 * callbacks while it holds its own locks (in a poll, or inline in Unmap), so
 * releasing the buffer there takes a lock this thread already holds: wgpu
 * panics ("panic in a function that cannot unwind", a mutex lock failing) and
 * the app aborts. It happened on an iPhone when pass timing gave up on reads
 * still in flight. They are released here instead, outside any callback, at
 * the next start, poll or release. Context thread only, as the callbacks. */
static RaeWgpuReadRequest* g_rae_read_deferred = NULL;
static int g_rae_read_in_callback = 0;

static void raeWgpuReadFree(RaeWgpuReadRequest* request) {
    if (request->buffer) wgpuBufferRelease(request->buffer);
    free(request);
}

static void raeWgpuReadFlushDeferred(void) {
    while (g_rae_read_deferred) {
        RaeWgpuReadRequest* request = g_rae_read_deferred;
        g_rae_read_deferred = request->nextDeferred;
        raeWgpuReadFree(request);
    }
}

static void raeWgpuReadDrop(RaeWgpuReadRequest* request) {
    if (--request->references != 0) return;
    if (g_rae_read_in_callback) {
        request->nextDeferred = g_rae_read_deferred;
        g_rae_read_deferred = request;
        return;
    }
    raeWgpuReadFree(request);
}

static void raeWgpuReadCompleted(WGPUMapAsyncStatus status, WGPUStringView message,
                                  void* userdata1, void* userdata2) {
    (void)message;
    (void)userdata2;
    RaeWgpuReadRequest* request = userdata1;
    /* Cancellation may have happened before this callback, including inline
     * during Unmap. Never publish success again on a canceled request. */
    if (request->status == 0)
        request->status = status == WGPUMapAsyncStatus_Success ? 1 : -1;
    g_rae_read_in_callback++;
    raeWgpuReadDrop(request);
    g_rae_read_in_callback--;
}

void* rae_wgpu_read_start(void* buffer, uint64_t offset, uint64_t size) {
    raeWgpuReadFlushDeferred();
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
    raeWgpuReadFlushDeferred();
    return request->status;
}

/* Constructor-only status check: unlike poll, this never processes device
 * events. It lets the Rae owner reject immediate validation/allocation failure
 * without allowing a valid map callback to run during construction. */
int rae_wgpu_read_start_status(void* handle) {
    RaeWgpuReadRequest* request = handle;
    return request ? request->status : -1;
}

int rae_wgpu_read_copy(void* handle, void* destination, uint64_t capacity) {
    RaeWgpuReadRequest* request = handle;
    if (!request || request->status != 1 || !destination || capacity < request->size)
        return 0;
    /* #897: the caller-supplied `capacity` is derived from the destination
     * List's public length/cap fields and can be forged. Bound the write by the
     * destination's REAL allocation size, reported by the allocator, which no
     * caller field can inflate. A forged length/cap therefore cannot overflow
     * the heap: if the request does not fit the real allocation the copy is
     * refused. `request->size` is the request's own recorded byte count, never a
     * caller-modified mirror. */
    int64_t allocated = rae_malloc_size_safe(destination);
    if (allocated <= 0 || (uint64_t)allocated < request->size)
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
    raeWgpuReadFlushDeferred();
    return NULL;
}
