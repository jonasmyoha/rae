# Rae Stdlib Bootstrap Tiers

Queue: `#296 Runtime migration R10`

Status: design. No implementation in this task.

## Purpose

Rae should dogfood more of its standard library without creating circular
dependencies between generated C, the C runtime kernel, and Rae stdlib modules.

This document defines bootstrap tiers, build ordering, migration gates, and
success metrics for moving more library policy into Rae while keeping the
permanent C runtime small and stable.

## Core Principle

The C runtime is not the standard library.

- **Runtime kernel:** a small, unsafe C ABI that generated programs need in
  order to allocate memory, move bytes, call OS/platform APIs, and manage native
  handles.
- **Standard library:** ordinary Rae source compiled like user code, with narrow
  extern calls only where it crosses the runtime/platform boundary.

The stdlib must be tiered so low-level modules do not accidentally depend on
high-level platform modules. A module that only needs allocation must not import
filesystem, JSON, WebGPU, SDL, or application services by accident.

## Tier Model

### Tier 0: Compiler Builtins

Not Rae stdlib code.

Owned by compiler and generated C:

- primitive scalar representation;
- function ABI and call lowering;
- struct, enum, optional, and generic-specialization layout;
- scope-exit drop insertion;
- string/optional/list representation decisions while compiler-known;
- direct calls to permanent runtime ABI helpers.

Allowed dependencies:

- none. This is below Rae source.

Migration rule:

- Do not migrate Tier 0 into Rae until the corresponding representation is no
  longer compiler-known. This is a language/compiler architecture change, not a
  stdlib migration task.

### Tier 1: Runtime Kernel Externs

Small permanent C ABI imported by low-level Rae modules.

Examples:

- raw allocation, free, reallocation;
- raw buffer allocation, copy, move, get, set, drop-at;
- string allocation/copy/drop while `String` remains compiler-known;
- atomics, mutexes, condition variables, OS threads;
- crash/signal hooks;
- raw time/env/file primitives;
- opaque native-handle create/destroy/validate functions.

Allowed dependencies:

- Tier 0 only.

Migration rule:

- C remains the owner when the function crosses raw memory, OS ABI, platform
  callback, external library, or undefined-behavior boundary.
- Any policy above these primitives should be pushed to a higher Rae tier.

### Tier 2: `core`

Always-available, portable Rae source.

Purpose:

- minimal APIs needed by most Rae programs;
- no platform services;
- no filesystem/window/GPU/audio/network;
- no dependency on `std` or `platform`.

Examples:

- `List(T)` policy over raw buffers;
- optional/result helper functions;
- basic math helpers that do not require platform state;
- string algorithms that operate above allocation/drop;
- small deterministic algorithms needed by other tiers.

Allowed dependencies:

- Tier 0;
- Tier 1 runtime-kernel externs.

Forbidden dependencies:

- `std`;
- `platform`;
- SDL/WebGPU/raylib;
- filesystem, process, env convenience policy;
- JSON/package managers if they pull allocation-heavy or platform helpers in a
  way that creates cycles.

Migration gates:

- must compile without package/project context;
- must not require platform feature macros;
- must have ownership/leak tests before replacing C helpers;
- must not use app-specific assumptions.

### Tier 3: `alloc`

Heap-backed reusable Rae library code.

Purpose:

- richer dynamic containers and builders over Tier 1 allocation;
- code that needs allocation but not platform services.

Examples:

- `HashMap(K,V)` and specialized maps;
- string builders;
- byte buffers and slices;
- arena-style utility types if they are useful to ordinary Rae programs;
- serialization builders that do not perform file IO.

Allowed dependencies:

- `core`;
- Tier 1 allocation/raw-buffer externs.

Forbidden dependencies:

- platform APIs;
- filesystem/env/process;
- renderer/audio/image decoder calls.

Migration gates:

- generic specialization and ownership diagnostics must be strong enough for the
  container shape being migrated;
- move/drop behavior must be covered by tests for owned fields, nested lists,
  optionals, early returns, and reassignment;
- performance must be measured against the C bridge before deleting the bridge.

### Tier 4: `std`

Portable Rae policy over narrow runtime/platform calls.

Purpose:

- normal batteries-included library functionality;
- algorithms and policy that may use files, env, time, or process calls through
  narrow extern boundaries.

Examples:

- filesystem/path convenience helpers;
- JSON parser/serializer;
- package metadata and `.raepack` parsing;
- image/resource registries and retry policy;
- logging/formatting policy;
- task-group/channel policy above raw thread primitives.

Allowed dependencies:

- `core`;
- `alloc`;
- selected Tier 1 raw platform primitives.

Forbidden dependencies:

- renderer-specific globals unless the module explicitly belongs to a renderer
  package;
- application-local services such as Spotify integration.

Migration gates:

- C compatibility bridge stays until Rae implementation passes existing tests;
- migration must include deterministic tests that do not depend on the
  developer machine where practical;
- memory diagnostics must be run when ownership or container state is involved.

### Tier 5: `platform`

Platform and backend packages over opaque native handles.

Purpose:

- expose host capabilities to Rae apps while keeping raw ABI calls in C;
- make policy Rae-owned while raw handles remain opaque and owned.

Examples:

- SDL3 window/input/cursor wrappers;
- WebGPU descriptors, resource caches, bind tracking, validation, render graph;
- gpu2d renderer policy over raw WebGPU calls;
- image decoder/upload APIs;
- audio/network bindings;
- OS-specific integrations.

Allowed dependencies:

- `core`;
- `alloc`;
- `std` when no cycle is introduced;
- narrow C platform externs.

Migration gates:

- every native handle must have an explicit ownership/drop story;
- renderer/window modules must not become implicit dependencies of `core` or
  `alloc`;
- platform-specific behavior must have documented fallbacks or capability
  checks;
- example apps must prove the API is usable without runtime-only magic.

## Build Ordering

The build should become explicit rather than relying on accidental flat import
visibility.

Recommended ordering:

```text
1. Parse + sema compiler-owned builtins
2. Register Tier 1 runtime-kernel extern declarations
3. Compile core modules
4. Compile alloc modules
5. Compile std modules
6. Compile enabled platform modules
7. Compile project/app modules
8. Emit generated C
9. Link generated C with the C runtime kernel and selected platform libraries
```

Important rules:

- Lower tiers cannot import higher tiers.
- Platform modules are enabled by package/project capability, not by accidental
  use from `core`.
- App modules may import any enabled tier, but imports should make platform
  dependencies visible.
- `core` remains the only candidate for implicit/global availability. Everything
  else should move toward explicit module import or qualifier rules.

This ordering depends on the stdlib namespacing work in `QUEUE.md #255`. Until
that lands, migration tasks must be conservative about adding new globally
visible names.

## Migration Gates

Before moving a C helper or policy block into Rae:

1. Classify its target tier.
2. Verify the target tier can depend only on lower tiers.
3. Identify the permanent C ABI calls it still needs.
4. Add compatibility tests against current behavior.
5. Add ownership/leak tests if it owns strings, buffers, lists, maps, optionals,
   or native handles.
6. Keep the C bridge until the Rae implementation is proven.
7. Delete or deprecate the C bridge only in a separate cleanup task.

Block a migration if it requires:

- `core` importing `std` or `platform`;
- a language feature that ordinary Rae programmers would not benefit from;
- runtime-only magic unavailable to user code;
- app-specific policy inside a general stdlib tier.

## Success Metrics

Short term:

- each migrated module declares its intended tier in docs or comments;
- no new C runtime helper is added without a tier/boundary justification;
- migration tasks include compatibility tests before deleting C behavior.

Medium term:

- `core` and `alloc` build without platform feature macros;
- Filesystem, JSON, image registry, string algorithms, and container policy are
  ordinary Rae modules over narrow C primitives;
- generated apps link against fewer temporary C bridge symbols.

Long term:

- new stdlib functionality defaults to Rae unless it crosses a genuine platform
  ABI boundary;
- new runtime functionality defaults to Rae unless it belongs to the permanent C
  kernel;
- the C runtime surface is mostly allocator/raw memory/thread/atomic/crash/FFI
  glue plus external library calls;
- example apps such as 106 mobile UI and the raytracer use the same stdlib
  mechanisms available to ordinary apps.

## Current Placement

Current examples based on the July 2026 migration work:

- `core`: `List(T)` policy in `lib/core.rae`; string search/trim/case helpers in
  Rae where migrated by `#293`.
- `alloc`: future home for map and richer container policy after generic
  ownership/codegen issues are stable.
- `std`: `lib/filesystem.rae`, `lib/json.rae`, `lib/image_registry.rae`.
- `platform`: future WebGPU/gpu2d resource managers, SDL3 handle wrappers, image
  upload APIs.

Open design dependency:

- `docs/runtime-migration-to-rae.md` records the broader roadmap.
- `docs/runtime-kernel-abi.md` defines the permanent C boundary.
- `docs/native-handle-ownership.md` defines owned opaque platform handles.
- A future standard-library ownership model document should decide which types
  remain compiler-known and which become ordinary Rae code.
