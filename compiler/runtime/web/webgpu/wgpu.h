/* Browser builds only (`rae build --target wasm`; this directory is on emcc's
 * include path and on no native one). The generated WebGPU bindings
 * (lib/webgpu/Webgpu.rae, WebgpuTypes.rae) declare `cheader "webgpu/wgpu.h"`,
 * wgpu-native's header: the standard webgpu.h plus wgpu-native's extensions.
 * Emscripten's EmdawnWebGPU port ships only the standard header, so in the
 * browser this name resolves to it. The extensions (device polling, native
 * logging) are not used on the web: the runtime guards them with
 * __EMSCRIPTEN__ (runtime_webgpu.c). */
#ifndef RAE_WEB_WGPU_H
#define RAE_WEB_WGPU_H
#include <webgpu/webgpu.h>
#include <stdint.h>

/* wgpu-native's submit that hands back a submission index (lib/gpu/
 * GpuLifetime.rae tracks completion by it: the runtime's
 * rae_wgpu_watch_submission registers onSubmittedWorkDone with the index and
 * rae_wgpu_submission_done compares against the highest one reported). The
 * browser's submit has no index, so hand out an increasing one: completions
 * arrive in submission order, which is all the tracking needs. */
static inline uint64_t wgpuQueueSubmitForIndex(WGPUQueue queue, size_t commandCount,
                                               WGPUCommandBuffer const* commands) {
    static uint64_t rae_web_submission_index = 0;
    wgpuQueueSubmit(queue, commandCount, commands);
    rae_web_submission_index += 1;
    return rae_web_submission_index;
}
#endif
