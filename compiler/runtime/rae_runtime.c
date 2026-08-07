#include "rae_runtime.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

/* WASM (wasip1) lacks signals, threads, flock, and fork/exec. Pure-compute
 * Rae programs (the raytracer, etc.) use none of these, but the runtime is one
 * monolithic file, so the host-only features are guarded with `#ifndef __wasm__`
 * (with no-op/failure fallbacks where a symbol must remain) so it still
 * compiles for the WASM target. */
#ifndef __wasm__
#include <signal.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
/* pthread.h includes cleanly under wasi-sdk (provides pthread_t for the task
 * struct); only the pthread_* *calls* are guarded out for __wasm__ below. */
#include <pthread.h>
#endif

#if defined(__APPLE__) || defined(__linux__) || defined(__GLIBC__)
#include <execinfo.h>
#define RAE_HAVE_BACKTRACE 1
#endif

/* Runtime implementation modules. These are included into one translation
 * unit to preserve existing static helper visibility, linker behavior, and
 * emitted-app build mechanics. See docs/runtime-kernel-abi.md. */
#include "runtime_threads.c"
#include "runtime_core_memory.c"
#include "runtime_strings_core.c"
#include "runtime_system_log.c"
#include "runtime_strings_algorithms.c"
#include "runtime_filesystem.c"
#include "runtime_buffers_math.c"
#include "runtime_platform_apple.c"
#include "runtime_raylib.c"
#include "runtime_image_sdl3.c"
#ifdef RAE_HAS_WEBGPU
#include "runtime_webgpu.c"
#ifdef RAE_HAS_SDL3
#include "runtime_gpu2d_platform.c"
#include "runtime_gpu2d_box.c"
#include "runtime_gpu2d_text.c"
#include "runtime_gpu2d_image.c"
#include "runtime_gpu2d_frame.c"
#include "runtime_gpu3d.c"
/* After runtime_gpu3d.c: the deferred geometry pass shares mesh storage
 * (an asset, not a frame resource) and the platform present with it. */
#include "runtime_gpu3d_gbuffer.c"
#else
#include "runtime_gpu2d_stubs.c"
#include "runtime_gpu3d_stubs.c"
#endif /* RAE_HAS_SDL3 */
#endif /* RAE_HAS_WEBGPU */

/* The deferred G-buffer's CPU path (transform building through Mat4) is
 * meaningful without a GPU, so its stubs cover every build that did not
 * compile the real pass — including builds with no WebGPU at all, which
 * the gpu2d/gpu3d stubs above deliberately do not. */
#if !defined(RAE_HAS_WEBGPU) || !defined(RAE_HAS_SDL3)
#include "runtime_gbuffer_stubs.c"
#endif
#include "runtime_spotify_apple.c"
