# Runtime Kernel ABI

Queue: `#287 Runtime migration R1`

Status: design contract. No implementation yet.

This document defines the intended permanent ABI boundary between generated C,
Rae stdlib modules, and the small C runtime kernel.

It follows:

- `docs/runtime-migration-to-rae.md`
- `docs/runtime-audit-2026-07.md`
- `docs/native-handle-ownership.md`

## Core Principle

The C runtime is not the Rae standard library.

The C runtime kernel exists because generated C needs a stable ABI and because
some operations are inherently platform or external-library boundaries. The Rae
standard library should increasingly be ordinary Rae code compiled like user
projects.

An API belongs in the permanent C kernel only when at least one is true:

- generated C needs it for language semantics;
- it performs raw allocation or raw memory access;
- it crosses an OS/platform ABI boundary;
- it creates, destroys, or calls an external native library object;
- it implements threads, mutexes, atomics, condition variables, or signals;
- it must run before Rae stdlib code can be compiled or initialized.

Everything else is a migration candidate.

## ABI Stability Levels

Every runtime symbol should eventually be classified as one of these:

### Permanent Kernel

Stable C ABI required by generated code or by low-level platform boundaries.

Examples:

- raw buffer allocation/copy/get/set;
- string allocation/drop while `String` is compiler-known;
- thread/mutex/channel primitives;
- crash/signal handlers;
- raw SDL3/WebGPU/raylib calls while those bindings exist;
- native handle table allocation/release/validation;
- platform file/env/time primitives.

### C Binding

Thin wrapper around a native library or OS API. The raw call stays C, but policy
above it should be Rae.

Examples:

- `wgpuCreateBuffer`-style resource calls;
- SDL window/input event calls;
- ImageIO/stb/lodepng decode entry points;
- AppleScript/Cocoa bridge calls;
- raylib raw wrappers.

### Temporary Bridge

Compatibility API used while generated C or stdlib code migrates.

Examples:

- C JSON extraction helpers used only by generated `Type.fromJson()` and
  deprecated Live bridge paths; `lib/json.rae` owns ordinary parser/query and
  string-serialization policy;
- no-op stubs for disabled capabilities if capability validation replaces them;
- legacy `RaeAny` boxed buffer helpers once typed containers no longer need them.

### Legacy

Kept for existing examples/backends, not a strategic expansion point.

Examples:

- raylib rendering helpers;
- raylib/GLFW event workarounds;
- raylib-specific shader helpers.

## Permanent Kernel Surface

### Generated-C Representation Types

These remain ABI-level until a later ownership model explicitly changes them:

- `rae_String`
- `RaeAny`
- `RaeTask`
- primitive aliases such as `rae_Bool`, `rae_Char`, `rae_Char32`
- view/mod helper structs generated for C lowering

Container algorithms are not part of the permanent kernel. `List(T)`,
`StringMap(V)`, and future `HashMap(K,V)` policy should live in Rae over this
raw buffer ABI; C owns only allocation, byte movement, and compiler-directed
element destruction hooks.

Rules:

- Layout changes are breaking ABI changes.
- If layout must change, update generated C, tests, and migration docs in the
  same task.
- Rae stdlib code may wrap these representations but should not invent a second
  incompatible representation.

### Allocation And Raw Memory

Permanent kernel responsibilities:

- allocate/free/reallocate raw memory;
- copy/move/compare bytes;
- allocate/resize/free raw element buffers;
- raw typed buffer get/set/copy using `elem_size`;
- optional debug bounds and memory diagnostics.

Current examples:

- `rae_ext_rae_buf_alloc`
- `rae_ext_rae_buf_free`
- `rae_ext_rae_buf_resize`
- `rae_ext_rae_buf_copy`
- `rae_ext_rae_buf_set`
- `rae_ext_rae_buf_get`

Migration rule:

- `List(T)`, `HashMap`, and higher containers should migrate to Rae over these
  primitives, but these primitives remain C.

### String Kernel

Permanent for now:

- allocate owned string body;
- copy a string;
- free owned string body;
- statement-scope string temp pool used by generated C;
- conversion between Rae string and C string where needed for platform calls.

Migration candidates:

- `#293` moved `contains`, `startsWith`, `endsWith`, `trim`, ASCII
  `toLower`, and `indexOf` into Rae.
- Remaining candidates: formatting policy, parsing helpers, and eventual
  removal of compatibility C string algorithm entry points once no generated or
  legacy code path references them.

Constraint:

- Do not change `rae_String` representation during routine migration. That
  belongs to the future standard-library ownership model design.

### Task, Thread, Channel, Atomic

Permanent C kernel:

- OS thread creation and join;
- mutex/condvar-backed wait/wake;
- atomics when added;
- low-level channel storage/wakeup primitives.

Rae candidates:

- `TaskGroup`;
- cancellation policy;
- richer channel payload ownership;
- worker lifecycle;
- actor/system conventions.

### OS And Process Primitives

Permanent C kernel:

- process exit;
- env var access;
- raw file read/write/stat/list/lock;
- monotonic time and sleep;
- RSS/memory process stats;
- stdout/stderr flushing.

Rae candidates:

- path manipulation;
- globbing;
- next-index filename policy;
- date/formatting policy;
- filesystem convenience APIs.

### Crash And Diagnostics

Permanent C kernel:

- signal handlers;
- backtrace capture;
- crash-handler installation;
- low-level allocation-site recording where platform allocator metadata is used.

Rae candidates:

- formatting crash reports;
- optional user-facing diagnostics layers;
- test/report aggregation.

## Platform Binding Surface

### SDL3

C owns:

- creating/destroying windows;
- event polling;
- mouse/keyboard state reads;
- cursor setting;
- platform position/size APIs.

Rae should own:

- typed `SdlWindow`/`SdlCursor` values over opaque handles;
- app loop policy;
- idle/busy render scheduling;
- coordinate conversion policy where it does not require direct native state;
- persisted window placement policy.

### WebGPU

C owns:

- raw WebGPU calls;
- adapter/device/surface creation;
- buffer/texture/sampler/pipeline/bind group creation;
- queue write/readback/submit;
- command encoder/pass lifetime;
- native handle table validation and raw release calls.

Rae should own:

- typed `GpuDevice`/`GpuBuffer`/`GpuTexture`/`GpuPipeline`/`GpuBindGroup`
  values over opaque handles;
- descriptors;
- validation;
- resource ownership policy;
- cache keys;
- resize/reconfigure logic;
- bind tracking;
- render graph and pass ordering;
- debug labels;
- convenience APIs.

The detailed `#295` design is `docs/webgpu-resource-management-in-rae.md`.
That document is the source of truth for the staged migration from today's raw
runtime tables toward Rae-owned descriptors, caches, validation, bind tracking,
resize/reconfigure policy, render graph policy, and labels over thin C calls.

### Image Decoding

C owns:

- external decoder calls;
- platform decoder bridges;
- raw RGBA byte allocation returned from decoder;
- upload into GPU textures.

Rae should own:

- typed `ImagePixels` and `ImageTexture`/`GpuTexture` ownership;
- image registry keys;
- failed-load throttling;
- retry policy;
- cache metadata;
- fallback asset selection;
- decode-status propagation.

### raylib

raylib is legacy/parked. Keep its raw wrappers while examples need them, but do
not expand raylib policy as part of the strategic runtime migration.

### macOS/Spotify

C owns:

- AppleScript invocation;
- Cocoa/App Nap/process activation bridge;
- atomic file download primitive if implemented via shell/curl;
- thread primitive for poll workers.

Rae should own:

- Spotify state cache policy;
- polling schedule;
- artwork metadata cache;
- app-specific playback behavior.

## Header Organization Target

`rae_runtime.h` should eventually be organized into sections matching ABI
classification:

1. Generated-C representation types.
2. Permanent kernel APIs.
3. Platform binding APIs.
4. Temporary bridges.
5. Legacy APIs.

Short-term, comments are enough. Long-term, split headers may be useful:

```text
rae_runtime.h              # umbrella public ABI for emitted apps
rae_runtime_core.h         # generated-C representation + permanent kernel
rae_runtime_platform.h     # SDL/WebGPU/raylib/macOS bindings
rae_runtime_legacy.h       # raylib and compatibility bridges
```

Do not split public headers until the C implementation split is stable.

## Split Mechanics For #288

Use the conservative include-based split first.

Recommended shape:

```text
compiler/runtime/
  rae_runtime.c              # umbrella translation unit
  runtime_threads.c
  runtime_core_memory.c
  runtime_strings_core.c
  runtime_system_log.c
  runtime_strings_algorithms.c
  runtime_filesystem.c
  runtime_buffers_math.c
  runtime_platform_apple.c
  runtime_raylib.c
  runtime_image_sdl3.c
  runtime_webgpu.c
  runtime_gpu2d_platform.c
  runtime_gpu2d_box.c
  runtime_gpu2d_text.c
  runtime_gpu2d_image.c
  runtime_gpu2d_frame.c
  runtime_gpu2d_stubs.c
  runtime_spotify_apple.c
```

`rae_runtime.c` should `#include` the domain `.c` files in a controlled order.

Why include-based first:

- emitted standalone builds already copy and compile one `rae_runtime.c`;
- no compiler/linker plumbing change is needed for multiple runtime objects;
- static helpers can be moved with fewer linkage surprises;
- behavior-preserving split is easier to review.

After the split is stable, a later task can evaluate true multi-translation-unit
runtime builds.

Ordering rule for include-based split:

1. shared includes/macros/types;
2. core/memory/string/buffer/task/channel/sys/math;
3. optional platform sections behind feature macros;
4. legacy bindings;
5. stubs for disabled capabilities.

## Rules For Adding New Runtime APIs

Before adding a new C runtime API, answer:

1. Is this required by generated C language semantics?
2. Is this a raw platform/external ABI call?
3. Could this be ordinary Rae stdlib code over existing narrow externs?
4. Does it introduce ownership or lifetime state that Rae should own instead?
5. Is it permanent kernel, C binding, temporary bridge, or legacy?
6. If it creates a native resource, does it follow
   `docs/native-handle-ownership.md` rather than returning a raw `Int`?

Default decision:

- if it is platform/raw/unsafe, add a narrow C binding;
- if it is policy/algorithm/cache/registry, write it in Rae;
- if it must start in C for bootstrapping, label it temporary bridge and create
  a queue task for migration.

## #288 Acceptance Criteria

The runtime split task should satisfy:

- no exported symbol names change;
- emitted apps still receive a single compilable runtime entry point;
- existing feature macros still gate the same code;
- full compiler tests pass;
- example runner passes;
- 106 headless gpu2d smoke still works;
- raylib examples still compile unless explicitly parked out of the test set;
- no runtime migration or behavior change is mixed into the split commit.
