# Runtime Audit 2026-07

Queue: `#286 Runtime migration R0`

Status: audit only. No runtime migration implemented.

Source audited:

- `compiler/runtime/rae_runtime.c` — 6,262 LOC
- `compiler/runtime/rae_runtime.h` — 752 LOC

This document classifies the current C runtime into:

- **Permanent C kernel:** likely remains C because it is ABI, raw memory,
  platform, thread, signal, or external-library boundary.
- **C binding:** raw external API call surface should remain C, but policy above
  it should move to Rae.
- **Rae candidate:** algorithms or policy that should become Rae stdlib/app
  code when compiler/runtime prerequisites are ready.
- **Temporary bridge:** useful compatibility code while migration proceeds.
- **Legacy:** retained for existing examples/backends, but not strategic.

## Executive Summary

`rae_runtime.c` is doing too many jobs. It is runtime kernel, platform binding
layer, renderer implementation, image-loader, file/path helper, and app-specific
Spotify integration in one translation unit.

The audit confirms the architecture from `docs/runtime-migration-to-rae.md`:

- Keep C for raw memory, ABI, threads, atomics, OS/signal handling, and external
  library calls.
- Move stdlib algorithms and backend policy into Rae where doing so gives real
  dogfooding value without creating bootstrap cycles.
- Split the C runtime before major migration so the permanent kernel and
  temporary bridges are visible.

The best early migration candidates are not necessarily Filesystem or JSON.
After the kernel ABI is documented, the first implementation slice should be
chosen from:

- image/resource registry policy;
- WebGPU/gpu2d descriptor/cache/resize policy;
- filesystem/path convenience helpers;
- JSON parse/serialize;
- string algorithms above allocation.

The riskiest early migration candidates are `String` representation,
`List(T)`, and `HashMap`, because ownership/codegen mistakes affect every app.

## Domain Inventory

| Lines | Approx LOC | Domain | Classification | Split target | Migration note |
| ---: | ---: | --- | --- | --- | --- |
| 36-76 | 41 | `Task(T)` thread handle/runtime | Permanent C kernel plus future Rae policy | `runtime_task.c` | Thread creation/join remains C; task grouping, cancellation policy, richer payload APIs should be Rae. |
| 77-176 | 100 | `Channel(Int)` MPSC queue | Permanent C kernel plus temporary bridge | `runtime_channel.c` | Mutex/condvar stays C; typed channel policy and non-`Int` payloads should move to Rae once queue #279 lands. |
| 177-250 | 74 | stdout flushing and crash/signal handler | Permanent C kernel | `runtime_crash.c` | Signals/backtrace are platform ABI. Keep C. |
| 252-523 | 272 | memory stats, malloc-size probing, pointer-site hash | Permanent C kernel/debug | `runtime_mem.c` | Runtime diagnostics can remain C; Rae can expose reporting APIs. |
| 527-676 | 150 | `rae_String` allocation/drop/copy/temp pool | Mixed: kernel plus future Rae algorithms | `runtime_string.c` | Allocation/drop/pool stays kernel until `String` ownership model is redesigned. Algorithms below can migrate first. |
| 677-725 | 49 | string interpolation varargs | Temporary bridge | `runtime_string.c` | Generated C currently depends on it. Long-term compiler may lower interpolation to Rae builders or stdlib calls. |
| 726-786 | 61 | monotonic time, sleep, timestamp/date formatting, bare spawn | Mixed | `runtime_time.c`, `runtime_task.c` | Raw clock/sleep/thread C stays. Formatting policy can move to Rae when locale/time APIs are designed. |
| 799-861 | 63 | ad-hoc JSON field getter / `RaeAny` JSON | Rae candidate / temporary bridge | `runtime_json_bridge.c` | Replace with `lib/json.rae` parse/query APIs; keep only compatibility until callers migrate. |
| 862-1099 | 238 | logging and `RaeAny` formatting | Mixed | `runtime_log.c` | Raw stdout/stderr C stays. Formatting and structured output can move to Rae once `String`/builder support is stable. |
| 1100-1257 | 158 | string algorithms: concat, len, compare, hash, sub, contains, starts/ends, lower, trim, parse int/float | Rae candidate above kernel | `runtime_string.c` then `lib/string.rae` | Keep allocation/drop in C; move algorithms gradually with compatibility tests and leak checks. |
| 1259-1285 | 27 | stdin read line/char and process exit | Permanent C kernel / platform | `runtime_io.c` | Raw console and process exit are OS boundary. |
| 1289-1467 | 179 | sys filesystem/env helpers: get env, read/write/rename/delete/mkdir/exists/lock/rss/list dir/mtime | Mixed | `runtime_sys.c`, `runtime_fs.c` | Raw syscalls stay C. Path manipulation, glob/index/date naming, convenience APIs should be Rae. |
| 1473-1567 | 95 | primitive-to-string and `RaeAny` stringification helpers | Mixed | `runtime_string.c` | Numeric conversion may remain C initially; formatting policy can move to Rae later. |
| 1571-1600 | 30 | RNG and primitive casts | Mixed | `runtime_math.c` | Raw RNG state can stay C; higher random distributions should be Rae. |
| 1601-1775 | 175 | raw buffer allocation/resize/copy/get/set and debug bounds records | Permanent C kernel | `runtime_buf.c` | This is the core ABI under Rae containers. Keep C; Rae containers should build on it. |
| 1780-1793 | 14 | math wrappers to libc | C binding | `runtime_math.c` | Keep thin C or direct externs. Rae may provide higher math utilities. |
| 1795-1838 | 44 | C JSON extraction helpers | Rae candidate / temporary bridge | `runtime_json_bridge.c` | Migrate to Rae JSON. |
| 1841-1854 | 14 | crypto stubs | Temporary bridge | `runtime_crypto.c` | Either bind real crypto behind C ABI or remove stubs once capability model exists. |
| 1857-1939 | 83 | macOS App Nap and process activation | Platform C binding | `runtime_macos.c` | Objective-C/Cocoa bridge belongs at C boundary. |
| 1941-2125 | 185 | raylib basic window/draw/time/texture wrappers | Legacy C binding | `runtime_raylib.c` | raylib is parked/legacy. Keep wrapper while examples need it; do not expand strategic policy here. |
| 2127-2261 | 135 | GLFW wait-events, close wake, mouse hook workaround | Legacy/platform binding | `runtime_raylib_glfw.c` | Platform workaround belongs C while raylib path exists. |
| 2272-2420 | 149 | raylib input/window/texture/cropped texture helpers/log level | Legacy binding plus policy | `runtime_raylib.c` | Raw raylib calls stay. Cropping/image policy is a migration candidate only if raylib path remains active. |
| 2430-2706 | 277 | raylib shader helpers for rounded sprites, MSDF, gradients, blur | Legacy renderer implementation | `runtime_raylib_render.c` | Strategic renderer is gpu2d/WebGPU; do not migrate this to Rae unless raylib is revived. |
| 2731-2940 | 210 | raylib screenshot/draw texture/text/font helpers | Legacy C binding | `runtime_raylib.c` | Keep only for existing raylib examples. |
| 2954-3090 | 137 | PNG save/load and compression oracle helpers | C binding / reference oracle | `runtime_image.c`, `runtime_compress_oracle.c` | External codec/reference oracle belongs C until Rae PNG/DEFLATE tasks prove replacement. |
| 3150-3335 | 186 | SDL3 software window/input/pixel-present helpers | C binding | `runtime_sdl3.c` | Raw SDL3 boundary. Higher window/app loop policy should be Rae. |
| 3363-3474 | 112 | SDF atlas loader and filesystem convenience helpers | Mixed | `runtime_sdf.c`, `runtime_fs.c` | Atlas file IO/raw parse can move partly Rae later. Filesystem convenience path/index/today policy is Rae candidate. |
| 3511-3791 | 281 | WebGPU init/callbacks/generic compute buffer/kernel/readback/reset | C binding plus policy | `runtime_webgpu.c`, `runtime_gpu.c` | Raw WebGPU calls stay C. Buffer/kernel resource managers and validation should move to Rae. |
| 3812-4247 | 436 | gpu2d global state, coordinate transform, clip stacks, SDL3 window/input, design resolution | Mixed | `runtime_gpu2d_platform.c`, Rae resource/window policy | SDL3/WebGPU calls stay C. Coordinate/window policy and clip-stack ownership should be evaluated for Rae. |
| 4252-4491 | 240 | gpu2d box pipeline and draw queue | Mixed | `runtime_gpu2d_box.c` | Raw pipeline/draw calls stay C; queue building, sort, invalidation policy should move to Rae where feasible. |
| 4504-4727 | 224 | gpu2d MSDF text pipeline/glyph queue | Mixed | `runtime_gpu2d_text.c` | Shader/pipeline/upload stay C. Text layout/style policy already belongs in Rae and should continue moving there. |
| 4733-5237 | 505 | gpu2d image decode/upload/registry/draw queue/image pipeline | Mixed; strong Rae candidate above decoder/upload | `runtime_gpu2d_image.c`, `lib/image_registry.rae` | Decode/upload raw calls stay C. Key registry, failed-load throttling, cache policy, scale-mode choice should be Rae. |
| 5237-5481 | 245 | gpu2d frame begin/end, screenshot readback, flush, close | C binding plus policy | `runtime_gpu2d_frame.c` | Present/readback/raw command encoding stays C. Frame policy and screenshot selection can move Rae-side. |
| 5550-5590 | 41 | gpu2d no-op stubs when SDL3/WebGPU disabled | Temporary bridge | matching gpu2d modules | Keep as build compatibility unless capability validation replaces it. |
| 5593-6262 | 670 | macOS Spotify AppleScript, poll thread, artwork fetch/status/iTunes search, non-Apple stubs | Mixed; app/platform policy should move out | `runtime_spotify_macos.c`, Rae app/service layer | AppleScript/process bridge and thread/fetch primitives stay C. Spotify state/cache/poller policy should move to Rae or an app-local integration layer. |

## Migration Priority Buckets

### Keep In C Permanently

- Raw allocation, free, realloc, memory copy/move/compare.
- Raw buffer allocation/resize/get/set.
- C ABI structs needed by generated C: `rae_String`, `RaeAny`, `RaeTask` until
  their ownership model changes.
- OS threads, mutexes, condvars, atomics, signal/crash handling.
- POSIX/Win32/Objective-C/Cocoa bridges.
- SDL3/WebGPU/raylib raw calls while those backends exist.
- External codec/library entry points.

### Split First, But Do Not Rewrite Yet

- `runtime_string.c`
- `runtime_buf.c`
- `runtime_task.c`
- `runtime_channel.c`
- `runtime_sys.c`
- `runtime_math.c`
- `runtime_raylib*.c`
- `runtime_sdl3.c`
- `runtime_webgpu.c`
- `runtime_gpu2d_*.c`
- `runtime_spotify_macos.c`

This split is queue `#288`. It should happen after the kernel ABI design in
`#287`, but before broad migration. It is a mechanical clarity step, not a
behavior change.

### Strong Rae Migration Candidates

- `lib/image_registry.rae`: image-key registry, failed-load throttling, decode
  status cache, retry policy, scale-mode choice. C keeps decode/upload.
- `lib/webgpu/resource_manager.rae`: descriptors, cache keys, ownership,
  resize/reconfigure policy, bind tracking, debug labels. C keeps raw WebGPU
  calls.
- `lib/filesystem.rae` path policy: join/base/dir/ext/glob/next-index/date
  naming. C keeps platform file operations.
- `lib/json.rae`: parser/serializer/query policy. `#291` moved JSON string
  escaping/quoting and 106 app writer policy here; the C JSON helpers remain
  temporary compatibility bridges for generated `Type.fromJson()` and
  deprecated Live bridge paths.
- `lib/string.rae`: algorithms above allocation/drop: trim, contains,
  startsWith, endsWith, split/replace, case conversion, formatting helpers.
  `#293` moved contains/startsWith/endsWith/indexOf/trim/ASCII toLower into Rae;
  the C versions are now compatibility candidates.
- `lib/time.rae`: formatting policy. C keeps raw clocks.
- Spotify state/cache/poller policy should move out of the core runtime. The
  AppleScript bridge may remain C or become a platform capability binding.

### Defer Until Compiler Ownership Is Stronger

- `String` representation.
- `List(T)` internals.
- `HashMap(K,V)` / `StringMap(V)` internals. `#292` moved `List(T)`
  mutation/drop ownership policy further into Rae, but map rehash/removal
  ownership cleanup remains blocked on generic-container codegen issues.
- Generic channel payload storage.

These are high dogfooding value, but the blast radius is large. They should wait
for the standard-library ownership design task and the known generic container
codegen bugs to be resolved.

## Proposed Runtime Split

The first non-audit implementation should split by domain while preserving the
same exported symbols:

```text
compiler/runtime/
  rae_runtime.h
  rae_runtime.c              # umbrella includes or shared bootstrap only
  runtime_threads.c          # Task and Channel primitives
  runtime_core_memory.c      # crash/stdout/mem diagnostics
  runtime_strings_core.c     # String allocation/drop/temp pool
  runtime_system_log.c       # time/spawn/json bridge/logging
  runtime_strings_algorithms.c
  runtime_filesystem.c       # console/env/file primitives
  runtime_buffers_math.c     # raw buffers/math/json bridge/crypto stubs
  runtime_platform_apple.c
  runtime_raylib.c           # legacy raylib wrapper
  runtime_image_sdl3.c       # image codecs + SDL3 software/platform helpers
  runtime_webgpu.c           # raw WebGPU helpers
  runtime_gpu2d_platform.c   # gpu2d window/input/surface/clip state
  runtime_gpu2d_box.c
  runtime_gpu2d_text.c
  runtime_gpu2d_image.c
  runtime_gpu2d_frame.c
  runtime_gpu2d_stubs.c
  runtime_spotify_apple.c
```

Open choice for `#288`: either compile multiple C files, or keep a single
`rae_runtime.c` that includes the split implementation files. The include-based
approach is less invasive for emitted standalone builds; the multi-translation
unit approach is cleaner long-term. `docs/runtime-kernel-abi.md` chooses the
include-based split as the first behavior-preserving step.

## Dependencies And Follow-Up Tasks

- `#287` must define the permanent kernel ABI before splitting names into
  modules.
- `#288` should be mechanical and behavior-preserving.
- `#289` opaque native handle ownership is a prerequisite for serious WebGPU
  manager migration.
- `#290` Filesystem and `#291` JSON are candidates, not mandatory first
  migrations. Pick based on audit results and current compiler stability.
- `#292` containers and `#293` String layering depend on the standard-library
  ownership model from `#297`.
- `#294` image/resource registry and `#295` WebGPU resource management may be
  better first dogfooding targets than containers because they exercise real
  ownership with narrower blast radius.

## Immediate Recommendation

Proceed in this order:

1. `#287`: document the runtime-kernel ABI and decide split mechanics.
2. `#288`: split `rae_runtime.c` into domain files without behavior changes.
3. `#289`: design opaque handle ownership/drop.
4. Choose one low-blast-radius Rae migration from `#290`, `#291`, `#294`, or
   `#295` based on test coverage and current compiler risks.
