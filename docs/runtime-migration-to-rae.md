# Runtime Migration To Rae

Status: design plan, no implementation yet.

## Motivation

`compiler/runtime/rae_runtime.c` has grown beyond a small runtime. It now contains
core allocation/string/buffer support, task/channel glue, OS calls, raylib and
GLFW wrappers, WebGPU/gpu2d rendering, image decoding, screenshot support, and
macOS Spotify integration.

That is useful while the language is young, but it is not the long-term shape we
want. Rae should dogfood itself wherever practical. Runtime policy written in Rae
exercises the ownership model, generic containers, move semantics, destructors,
module boundaries, optimizer, and Compiled backend in ways toy examples cannot.

The goal is not to eliminate C. The goal is to make C the thin, unsafe platform
edge and move higher-level algorithms and policy into Rae.

## Current Shape

As of July 2026, `rae_runtime.c` is roughly 6,262 lines:

| Area | Lines | Share |
| --- | ---: | ---: |
| Task/channel/crash/mem/string/time/log/io/sys/buf/math/json | 1,940 | 31% |
| raylib/GLFW/OpenGL wrappers | 768 | 12% |
| WebGPU core/raytrace/compute | 1,098 | 18% |
| SDL3 gpu2d renderer/window/input/text/images/screenshot | 1,784 | 29% |
| Spotify/macOS integration | 672 | 11% |

Some of this is proper runtime substrate. A lot is backend policy or application
integration living in the runtime because C was the quickest place to add it.

## Architectural Vision

Desired long-term stack:

```text
Compiler and stdlib policy written in Rae
        |
        v
Generated C
        |
        v
Small C runtime and platform ABI layer
        |
        v
OS APIs, SDL3, WebGPU, image/audio/network libraries
```

The permanent C layer should mostly provide:

- ABI glue and symbol names used by generated C.
- Allocator wrappers and raw memory primitives.
- Atomics, mutexes, threads, condition variables, and crash/signal handling.
- External library bindings.
- Platform ABI bridges: POSIX, Win32, Objective-C/Cocoa where required.
- SDL3, WebGPU C API, and image/audio decoder entry points.
- FFI helpers for opaque handles and byte buffers.

Everything above that should be evaluated as a Rae candidate.

## Runtime Is Not The Standard Library

The C runtime is not the Rae standard library.

The runtime exists because generated C needs a stable ABI and a small set of
operations that cannot be expressed portably as ordinary Rae code yet:

- symbol names and calling conventions generated C can link against;
- allocator and raw memory primitives;
- platform interfaces and OS integration;
- thread, mutex, atomic, signal, and crash primitives;
- external library and platform ABI boundaries.

The standard library is different. It should increasingly be ordinary Rae code
compiled exactly like user projects. A `lib/json.rae`, `lib/filesystem.rae`, or
future `lib/webgpu/resource_manager.rae` module should not be "runtime code" just
because it is shipped with Rae. It should be normal Rae source using narrow
extern imports where it crosses into the platform.

This distinction is a core architecture rule:

- **Runtime:** the minimal trusted C kernel needed by generated programs.
- **Standard library:** portable Rae modules plus narrow platform bindings.
- **Applications:** Rae projects using the same compiler and stdlib mechanisms.

If a feature can live as stdlib Rae code without circular bootstrap or unsafe ABI
problems, that is preferred over adding it to the C runtime.

## Migration Philosophy

Prefer migrating code that maximizes compiler and language dogfooding while
minimizing bootstrap risk.

This is deliberately broader than "move policy before primitives." Policy code
is often a good first migration target, but the order should be chosen from the
current compiler state, testability, and risk profile. For example, WebGPU
resource-cache policy may be a better early dogfooding target than a generic
container rewrite because it exercises ownership and data-oriented design without
destabilizing every `List(T)` user.

The migration order should be re-evaluated after the runtime audit. Good early
targets have most of these properties:

- mostly deterministic and testable without platform race conditions;
- enough real complexity to exercise Rae, not just wrapper boilerplate;
- narrow C boundary;
- clear compatibility oracle from the existing C implementation;
- limited blast radius if ownership codegen still has bugs;
- useful to ordinary Rae programmers, not just runtime internals.

Prefer this split:

- C owns unsafe calls, raw handles, raw bytes, OS callbacks, and external ABIs.
- Rae owns descriptors, validation, ownership policy, caches, registries,
  resource lifetime rules, parsing, serialization, and application-facing APIs.

Do not add language features only to make runtime migration possible. A proposed
low-level feature must pass this test:

> Would ordinary Rae programmers also benefit from this feature?

If the answer is no, keep it as compiler/runtime implementation detail instead
of exposing it as a language feature.

## Lessons From Other Systems Languages

Rust separates the world into layers: `core` can exist without OS services,
`alloc` adds heap-backed containers, and `std` adds platform services. Rust also
has explicit `no_std` behavior where the standard prelude switches to `core`
instead of `std`, and the standard library is documented as the place for
portable abstractions, containers, I/O, and threading.

Rae should borrow the layering idea, not the exact Rust trait ecosystem. A Rae
equivalent could be:

- `core`: compiler-known primitives, minimal `String`/slice/buffer contracts,
  no filesystem/window/GPU.
- `alloc`: owned dynamic containers built on runtime allocation primitives.
- `std`: files, time, process, channels, JSON, paths, package support.
- `platform`: SDL3/WebGPU/audio/image bindings and host capabilities.

Zig's self-hosting path shows the value of a staged bootstrap kernel. The
important lesson is not "rewrite everything at once"; it is to keep a small
trusted bootstrap surface while the self-hosted layer grows around it.

Swift is another useful warning: a safe high-level language can still keep a
real runtime for ABI, metadata, reference counting, Objective-C interop, and OS
integration. Rae should not be embarrassed by keeping C where C is the natural
ABI boundary.

References:

- Rust standard library docs: <https://doc.rust-lang.org/std/>
- Rust `no_std` reference: <https://doc.rust-lang.org/reference/names/preludes.html#the-no_std-attribute>
- Zig self-hosting bootstrap note: <https://ziglang.org/news/goodbye-cpp/>
- Swift runtime design notes: <https://github.com/swiftlang/swift/blob/main/docs/Runtime.md>

## Bootstrap Model For Rae

Rae needs a stable bootstrap boundary:

1. The compiler emits C for Rae code.
2. Generated C links against a small C runtime.
3. Rae stdlib modules are compiled like normal Rae code.
4. Runtime-owned primitives are imported through narrow extern declarations.

The bootstrap layer should be explicit. A module should not accidentally depend
on all of `std` just because it needs allocation. Suggested tiers:

### Tier 0: Compiler Primitives

These are not ordinary stdlib code:

- Raw integer/float/bool/char representations.
- Function ABI.
- Stack locals and scope-exit drop codegen.
- Struct layout and enum/optional representation.
- Direct calls to C runtime allocation and memory copy primitives.

### Tier 1: C Runtime Kernel

Small, permanent C ABI:

- `alloc`, `realloc`, `free`, `memcpy`, `memmove`, `memcmp`.
- String allocation/copy/drop primitives if `String` remains compiler-special.
- Raw buffer allocation/resize/free.
- Thread, mutex, condvar, atomic primitives.
- OS time, file descriptor, env, process, signal/crash hooks.
- Opaque handle creation/destruction for platform APIs, following
  `docs/native-handle-ownership.md`.

### Tier 2: Rae Core/Alloc Stdlib

Dogfood candidates once compiler ownership is stable:

- UTF-8 helpers.
- String algorithms above allocation: trim, contains, startsWith, split, replace.
- `List(T)` policy and bounds checks over raw buffers.
- `HashMap(K,V)` / `StringMap(V)` algorithms.
- Optional helpers.
- Result/error helpers if added.

### Tier 3: Rae Platform Stdlib

Rae code over narrow C imports:

- Paths and filesystem convenience APIs.
- JSON parser and serializer.
- Package metadata parsing.
- Asset registries and caches.
- Image registry policy.
- WebGPU descriptors, validation, resource caches, resize policy.
- Renderer command generation and render graph policy.

## Recommended Boundaries

### Strong Candidates For Rae

- UTF-8 validation and iteration, except maybe SIMD fast paths.
- String algorithms that operate on `String`/slices.
- `List(T)` growth policy and methods, backed by raw buffer primitives.
- `HashMap` and specialized maps.
- JSON parse/serialize.
- Path manipulation.
- Filesystem convenience helpers over a small syscall/SDL layer.
- Asset registry and resource cache policy.
- Image registry, decode-result tracking, failed-image throttling.
- Render graph construction and sorting.
- ECS and UI framework code.
- WebGPU resource descriptors, ownership, validation, bind tracking.
- Package manager libraries and `.raepack` parsing.

### Should Stay In C

- Allocator wrappers and raw memory operations.
- Atomics, mutexes, condition variables, OS threads.
- Signal handlers and crash handlers.
- Platform ABI glue: POSIX, Win32, Objective-C/Cocoa.
- SDL3 and WebGPU raw API calls.
- External library integration: image decoders, audio, crypto, compression
  or network libraries while they are external dependencies.
- OS callbacks and event pump entry points.
- Raw GPU resource creation/destruction calls.

These are natural FFI boundaries because they depend on platform headers,
calling conventions, handles, callbacks, or undefined behavior constraints that
Rae should not expose broadly.

## WebGPU Philosophy

WebGPU should not be treated as "must be C". The raw calls are C; the policy
above them should be Rae.

C should provide low-level operations such as:

- Create/destroy device resources.
- Create buffers, textures, samplers, bind groups, pipelines.
- Write buffer/texture data.
- Begin/end passes and submit command buffers.
- Present or read back surfaces.

Rae should own:

- Resource descriptors and validation.
- Texture/buffer lifetime policy.
- Caches and registries.
- Resize/reconfigure policy.
- Bind tracking and invalidation.
- Render graph and pass ordering.
- Convenience APIs for UI/gpu2d/raytracer code.
- Debug labels and diagnostics.

This keeps the hard ABI edge small while making the renderer architecture
dogfood Rae's ownership and ECS/data-oriented model.

## Language Evolution Guidelines

Do not add runtime-only features. Prefer features that unlock normal systems
programming:

- Opaque external handle types with explicit ownership/drop.
- Safer byte slices and fixed-size views.
- Better extern declarations and package-scoped native libraries.
- Function pointer/callback support if needed for platform callbacks.
- Module-level destructors or explicit app teardown hooks, if justified by
  ordinary global resource management.
- Compile-time constants if they improve stdlib quality generally.
- Stronger generic specialization if containers need it.

Be skeptical of:

- Raw pointer arithmetic exposed casually.
- Runtime-only magic attributes.
- Compiler intrinsics that normal Rae code cannot reason about.
- Special cases that make the stdlib pass but make user code less predictable.

## Staged Roadmap

### Stage 0: Audit And Split

Inventory `rae_runtime.c` into clear domains and split it into smaller C files
without changing behavior. This is not dogfooding yet; it makes the boundary
visible.

Current audit: `docs/runtime-audit-2026-07.md`.
Native handle model: `docs/native-handle-ownership.md`.
Stdlib bootstrap tiers: `docs/stdlib-bootstrap-tiers.md`.

Deliverables:

- Runtime module map.
- Build still emits/copies the required runtime files.
- No behavior changes.

### Stage 1: Define The Runtime Kernel

Design the permanent C ABI layer and document each function as either permanent
kernel, temporary migration bridge, or deprecated.

Deliverables:

- `docs/runtime-kernel-abi.md`.
- `docs/native-handle-ownership.md`.
- Header annotations or grouped declarations.
- Test that generated apps link using the split runtime.

### Stage 2: Select First Migration Slices

After the audit and kernel-boundary design, choose the first Rae migrations by
dogfooding value and bootstrap risk, not by a fixed category order.

Strong candidate families:

- Resource-management policy: image registries, failed-load throttling, asset
  caches, WebGPU descriptors/resource managers, gpu2d render graph policy.
- Portable algorithms: UTF-8/string helpers, path helpers, JSON
  serializer/parser.
- Container policy: `List(T)` mutation/drop ownership is now Rae-owned over raw
  C buffers; remaining `HashMap`/`StringMap`/`IntMap` internals should wait
  until generic ownership/codegen is stable enough.

Resource-management policy may move before some portable algorithms if it gives
better dogfooding value with lower bootstrap risk. JSON and Filesystem should
remain good candidates, but the audit should decide whether they are the first
ones.

Keep C reference implementations until the Rae version passes compatibility
tests and memory diagnostics.

First completed policy slice: `#290` moved filesystem render-output next-index
scanning and portable path helpers into `lib/filesystem.rae`, leaving C with
only SDL-backed platform primitives such as known folders, directory existence,
directory listing, and date formatting.

Second completed policy slice: `#291` moved JSON string escaping/quoting and
106 mobile UI JSON writer policy into `lib/json.rae`. The remaining C JSON
helpers are compatibility bridges for compiler-generated `Type.fromJson()` code
and deprecated Live bridge paths; deleting them requires a separate generated-C
stdlib dependency design rather than another ad hoc app-level serializer.

Third completed policy slice: `#292` kept raw buffer allocation/copy/set/get in
C but moved more ownership policy into Rae's `List(T)` methods. `List.set`,
`List.remove`, `List.clear`, and explicit `List.free` now destroy active owned
elements via the compiler-aware raw `buf_drop_at` primitive before overwriting,
hiding, or releasing storage. `List.drop` remains the compiler-synthesized
scope-exit entry point that injects element drops before raw buffer release.
`StringMap`/`IntMap` algorithms are already Rae code, but deeper map
rehash/removal ownership cleanup is deferred until the remaining
generic-container codegen issues are fixed.

Fourth completed policy slice: `#293` kept `rae_String` allocation, drop,
copy, temp-pool, concat, substring, conversion, and parse-number helpers in C,
but moved common string algorithms into Rae: `contains`, `startsWith`,
`endsWith`, `indexOf`, `trim`, and ASCII `toLower`. `split`, `replace`, `join`,
and `lastIndexOf` were already Rae code. The old C algorithm entry points remain
compatibility code until the runtime surface can be pruned safely.

Fifth completed design slice: `#295` defined the WebGPU resource-management
roadmap in `docs/webgpu-resource-management-in-rae.md`. The intended boundary is
thin raw C WebGPU calls plus opaque handles, with Rae owning descriptors,
resource caches, resize/reconfigure policy, bind tracking, validation, render
graph policy, and debug labels.

First completed implementation slice: `#294` introduced `lib/image_registry.rae`
and moved image-key metadata, failed-load throttling, and fired-fetch throttling
out of 106 app glue into Rae. C still owns raw decode/upload and render-time
gpu2d image-key lookup for now.

Sixth completed design slice: `#296` defined the stdlib bootstrap tiers in
`docs/stdlib-bootstrap-tiers.md`: Tier 0 compiler builtins, Tier 1 C runtime
kernel externs, Tier 2 `core`, Tier 3 `alloc`, Tier 4 `std`, and Tier 5
`platform`, with explicit build ordering and migration gates.

### Stage 3: Move Pure Algorithms And Platform Policy

Start with algorithms that require no platform callbacks:

- UTF-8/string helpers. `#293` moved the byte-preserving ASCII/string-search
  layer into Rae; representation and allocation stay in C.
- Path helpers.
- JSON serializer/parser.
- HashMap/List policy where compiler support is ready.

Move app/backend policy into Rae:

- Image registry and failed-load throttling. `#294` now provides the first
  Rae-owned policy layer in `lib/image_registry.rae`; remaining follow-up is to
  replace the runtime's raw key table with typed Rae-owned image handles.
- Asset caches and metadata stores.
- WebGPU descriptors/resource managers. `#295` records the concrete staged plan
  for descriptors, opaque handles, cache entries, bind tracking, validation,
  render graph policy, resize/reconfigure, and debug labels.
- gpu2d render graph and batching policy where feasible.

C remains the raw renderer/backend call surface. The order inside this stage is
intentionally flexible: resource policy, JSON/path helpers, and string utilities
should be sequenced based on test coverage and compiler readiness.

### Stage 4: Reduce Runtime Surface

Delete temporary C APIs whose Rae replacements are proven. Track runtime LOC and
permanent ABI count as health metrics.

### Stage 5: Compiler/Stdlib Dogfooding

Compile more of the compiler-adjacent libraries in Rae once generics,
namespacing, and ownership diagnostics are strong enough. Do this only after the
runtime-kernel split prevents circular bootstrap surprises.

## Risks

- Bootstrap cycles: `String` and containers may depend on runtime primitives
  that are themselves needed to compile the stdlib.
- Ownership bugs: migrating containers too early can create double-free/leak
  regressions in every app.
- Performance regressions: C helper loops may be faster until the optimizer is
  stronger.
- ABI drift: generated C and runtime headers must remain in sync.
- Platform drift: moving platform policy to Rae must not hide OS-specific edge
  cases.
- Over-generalization: adding language features for one runtime migration can
  burden all Rae programmers.

## Open Questions

- **Standard library ownership model.** This deserves a separate future design
  document: which types should remain compiler-known, which can become ordinary
  Rae code, which require runtime support forever, which should be opaque native
  handles, and where the permanent ABI boundary sits.
- **Stdlib tier enforcement.** `docs/stdlib-bootstrap-tiers.md` defines the
  target tiers, but compiler/package enforcement is still future work and
  depends on module namespacing.
- Should `String` remain compiler-special forever, or become a Rae struct over a
  raw buffer kernel?
- What is the minimum byte-slice model needed for safe parsers and decoders?
- How should owned opaque native handles be represented and auto-dropped?
  Initial design: `docs/native-handle-ownership.md`.
- Do module-level `var`s need language-level teardown, or should apps own
  explicit teardown?
- How much of WebGPU command encoding should be Rae before per-call FFI overhead
  becomes a problem?
- Should the stdlib be split into `core`, `alloc`, `std`, and `platform`
  packages, or is that too much structure too early?

## Success Metrics

- `rae_runtime.c` shrinks or is split into domain files with a clearly counted
  permanent kernel.
- New stdlib functionality defaults to Rae unless it truly crosses the platform
  ABI boundary.
- New runtime functionality defaults to Rae unless it genuinely belongs at the
  platform ABI boundary.
- 106 mobile UI and the raytracer continue to build without handwritten C in
  their example folders.
- Memory diagnostics stay flat under existing leak tests.
- Runtime migration tasks produce compatibility tests before deleting C paths.
