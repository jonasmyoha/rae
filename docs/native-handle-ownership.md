# Rae-Owned Native Handle Ownership

Queue: `#289 Runtime migration R3`

Status: design contract. No implementation yet.

This document defines the target ownership model for native platform resources
that Rae code can create, pass around, and drop. It builds on:

- `docs/runtime-migration-to-rae.md`
- `docs/runtime-kernel-abi.md`
- `docs/runtime-audit-2026-07.md`
- `docs/standard-library-ownership-model.md`

The goal is not to expose raw pointers to Rae. The goal is to let ordinary Rae
apps own platform resources safely while the C runtime remains a thin ABI layer.

## Problem

Current platform and renderer APIs use a mix of patterns:

- hidden process-global resources, such as the current SDL/gpu2d window;
- integer handles into C runtime tables, such as GPU buffers and image handles;
- raw external resources entirely hidden behind high-level extern calls;
- ad hoc reset/close calls, such as `gpu.reset()` or `gpu2d.closeWindow()`.

This worked while the runtime was small, but it does not scale. As more policy
migrates into Rae, apps need stable resource values with clear ownership and
drop behavior:

- SDL windows and cursors;
- WebGPU devices, surfaces, buffers, textures, samplers, pipelines, bind groups,
  command encoders, render passes, and queue-owned uploads;
- decoded images and uploaded image textures;
- audio devices, file watchers, sockets, and process handles later.

The model must be useful to user apps, not a special runtime-only mechanism.

## Design Goals

- **No raw pointers in Rae.** Rae sees typed opaque handles, not `void*`.
- **Ownership is visible in Rae types.** A value either owns the native resource
  or borrows one; the difference must be statically meaningful.
- **Drop is deterministic.** Owned handles release native resources during Rae
  scope-exit/cascade-drop, not only at process exit.
- **Type confusion is impossible at the Rae boundary.** A `TextureHandle` cannot
  be accidentally used as a `BufferHandle`.
- **Generation counters prevent stale handle reuse.** Old handles fail cleanly if
  a runtime table slot is recycled.
- **C stays a raw ABI layer.** C creates/destroys/calls native APIs; Rae owns
  descriptors, validation, caches, registries, and lifetime policy.
- **Ordinary apps benefit.** The same model should work for SDL, WebGPU, image,
  audio, network, and package/plugin resources.

## Non-Goals

- Do not change current handle representations in this task.
- Do not migrate WebGPU policy to Rae in this task.
- Do not add a full trait system only for native handles.
- Do not expose pointer arithmetic or raw native layouts to Rae.
- Do not make all platform resources globally singleton by default.

## Core Model

Each native resource should have a Rae-visible opaque value type:

```rae
type GpuBuffer {
  handle: NativeHandle
}

type GpuTexture {
  handle: NativeHandle
}

type SdlWindow {
  handle: NativeHandle
}
```

`NativeHandle` is a compiler/runtime-known representation, not a pointer. The
target physical representation is an integer token with enough information to
validate the lookup:

```text
NativeHandle = {
  tableKind: small integer
  slot: integer
  generation: integer
}
```

The exact packing can be decided later. The important property is semantic:
runtime lookup checks both table kind and generation before touching native
memory.

### Why Not Plain `Int`?

Plain `Int` handles are easy to pass to the wrong API and impossible to
auto-drop safely. They also hide ownership from the compiler. A typed opaque
wrapper lets Rae's existing `own`/`view`/`mod`/`copy` rules apply:

- `own GpuTexture` transfers ownership;
- `view GpuTexture` borrows for use without releasing;
- `mod GpuTexture` allows mutation/configuration where valid;
- `copy GpuTexture` should normally be rejected unless the type explicitly
  represents a cheap shared reference.

## Ownership Semantics

### Owned Handles

An owned native handle releases exactly once:

```rae
func loadTexture(path: view String) ret GpuTexture

func draw() {
  let tex: GpuTexture = loadTexture(path: "cover.png")
  # tex auto-drops at scope exit
}
```

Compiler-generated cascade drop calls the handle's destructor when `tex` leaves
scope. The destructor calls a narrow C runtime API such as:

```c
void rae_native_drop_handle(RaeNativeHandle h);
```

or a type-specific wrapper generated or declared for the opaque type.

After a move, the source must be made inert according to normal Rae move
semantics. Scope exit must not drop the moved-from handle.

### Borrowed Handles

Borrowed handles never release resources:

```rae
func drawImage(tex: view GpuTexture, dst: Rect)
```

The C runtime lookup validates the handle but does not consume it. Borrowing is
the default for per-frame draw calls and command recording.

### Shared Handles

Some native APIs are internally reference-counted, such as WebGPU objects. Rae
should still model ownership explicitly:

- one Rae owner value calls the corresponding release function;
- passing by `view` borrows;
- if shared ownership is needed, introduce an explicit `SharedHandle(T)` or
  `retain()` API later.

Do not silently make every handle copyable just because the underlying C API has
reference counts. Copyability must be a Rae API decision.

### Optional Handles

`opt GpuTexture` owns the payload when present and drops it when absent no-op.
This should use the same optional/drop rules as `opt String` and `opt Struct`.
The handle payload must not be represented as a raw `Int` that bypasses cascade
drop.

## Runtime Tables

C owns native pointer tables. Rae owns typed tokens into those tables.

Each table entry should contain:

```text
entry {
  void* nativePtr;
  uint32_t generation;
  uint32_t flags;
  optional debug label;
  optional owning parent handle;
}
```

Lookup rules:

1. Check table kind matches the called API.
2. Check slot is in range.
3. Check generation matches.
4. Check native pointer is non-null.
5. Check capability/state flags if required.

On failure, return a safe error value or emit a deterministic runtime diagnostic.
Do not crash through a stale pointer.

### Parent/Child Lifetimes

Many native resources depend on parents:

- texture depends on device;
- surface depends on window and device;
- bind group depends on layout, buffer, texture, or sampler;
- render pipeline depends on device and shader modules.

The preferred rule:

- parent resources outlive children;
- dropping a parent with live children is either rejected diagnostically or
  cascades in a documented order;
- Rae resource managers should usually own parents and children together, so
  raw apps rarely manage this manually.

For WebGPU specifically, the C runtime can call WebGPU release functions, but
Rae should own the higher-level resource graph that prevents invalid drop order.

## API Shape

Raw C binding layer:

```rae
func nativeCreateBuffer(device: view GpuDevice, desc: view BufferDesc) ret GpuBuffer extern
func nativeDropBuffer(buffer: own GpuBuffer) extern
```

Rae stdlib policy layer:

```rae
func createBuffer(this: mod GpuContext, desc: BufferDesc) ret GpuBuffer {
  validateBufferDesc(desc: desc)
  ret nativeCreateBuffer(device: this.device, desc: desc)
}
```

User code should normally call the Rae layer, not the raw native entry point.

### Naming

Types should be specific and PascalCase:

- `SdlWindow`
- `SdlCursor`
- `GpuDevice`
- `GpuBuffer`
- `GpuTexture`
- `GpuPipeline`
- `GpuBindGroup`
- `ImagePixels`
- `ImageTexture`

Low-level extern functions may use a clear prefix such as `native` or live in a
module namespace once namespacing is stable:

- `gpu/native.createBuffer`
- `sdl/native.createWindow`
- `image/native.decode`

## Resource Categories

### SDL Window And Cursor

Current gpu2d mostly owns one hidden window. Target shape:

- `SdlWindow` owns an SDL window.
- `SdlCursor` owns or borrows a system cursor depending on SDL semantics.
- app/window placement policy lives in Rae.
- event polling may remain C, but event routing/state belongs in Rae.

Window values should not be casually copyable. One owner should call close/drop.
Multiple windows become possible later without rewriting the model.

### WebGPU

C remains responsible for raw WebGPU calls:

- adapter/device/surface creation;
- buffer/texture/sampler/pipeline/bind group creation;
- queue writes;
- command encoding calls;
- submit/present/readback;
- raw release calls.

Rae should own:

- descriptors;
- validation;
- resize/reconfigure policy;
- resource caches;
- bind tracking;
- render graph;
- debug labels;
- lifetime groupings.

The first practical target is not to wrap every WebGPU object manually in app
code. It is to make `GpuContext` and its resource managers Rae-owned while C
keeps narrow native functions.

### Image Handles

Separate decoded pixels from uploaded GPU textures:

- `ImagePixels` owns CPU RGBA memory returned by a decoder.
- `ImageTexture` or `GpuTexture` owns uploaded GPU texture state.
- image registry keys, retry policy, failed-load throttling, and cache metadata
  belong in Rae.
- decoder calls and upload calls remain C.

This boundary is now active for #294: `lib/image_registry.rae` owns image-key
metadata, failed-load throttling, and fired-fetch throttling while C keeps
decoder/upload calls and the temporary gpu2d render-time key table.

### Legacy raylib

raylib remains legacy/parked. Existing raylib APIs should not block this model.
If a raylib resource is touched again, prefer wrapping it in the same typed
opaque-handle shape instead of adding new raw `Int` handles.

## Drop Integration

Opaque native handle types need a structural drop hook:

```text
drop(GpuBuffer) -> rae_native_drop_handle(buffer.handle)
```

Implementation options:

1. Compiler-known `NativeHandle` field convention.
2. Extern destructor metadata attached to a type.
3. Generated drop function for each opaque native wrapper.

Preferred direction: generated drop functions for typed wrappers, because this
matches existing struct cascade-drop and keeps type-specific behavior explicit.

The generated drop must:

- ignore zero/null handles;
- validate generation before release;
- release exactly once;
- clear the local handle after release if it remains addressable;
- not release borrowed `view` values;
- not release moved-from values.

## Interaction With `RaeAny` And `opt`

Native handles must not rely on `RaeAny` runtime type guessing for destruction.
If an opaque handle is boxed or wrapped in `opt`, the static type must still
drive the drop path.

Required behavior:

- `opt GpuTexture` drops the texture when present.
- `RaeAny` containing a native handle either carries explicit drop metadata or
  is rejected until that metadata exists.
- Generic containers of native handles must cascade-drop elements exactly once.

This is a direct lesson from representation-confused drops: base names or raw
runtime tags are not enough. Drop code must know the physical representation and
ownership mode.

## Threading And Main-Thread Rules

Some handles are thread-affine:

- SDL window operations generally belong to the main/UI thread.
- WebGPU queue/device operations may have API-specific thread rules.
- GPU uploads and rendering must stay on the rendering thread unless explicitly
  proven safe.

The handle metadata should eventually record thread-affinity flags, but the
first design rule is simpler:

- resource creation, mutation, and destruction happen on the owning system's
  thread;
- worker threads send commands/data, not raw platform handles;
- Rae systems own scheduling policy.

## Error Policy

Opaque handle APIs should prefer deterministic failure modes:

- constructors return `opt Handle` or `Result(Handle, Error)` once `Result`
  exists;
- draw/use APIs diagnose invalid/stale handles in debug builds;
- release of an already-empty handle is no-op;
- release of a stale non-empty handle is a diagnostic, not a crash.

The current `0 = failure` integer convention is acceptable as a bridge, but it
should not be the long-term user-facing API.

## Migration Path

### Phase H0: Document And Classify

- classify current raw `Int` handles and hidden globals;
- identify owner, borrower, and singleton resources;
- keep current behavior unchanged.

### Phase H1: Introduce Opaque Wrapper Types

- add Rae structs for `GpuBuffer`, `GpuKernel`, `GpuImage`, or equivalent;
- keep the same underlying C tables;
- add generated/declared drop hooks;
- migrate one small API surface first, preferably `lib/gpu.rae`.

### Phase H2: Move Resource Managers To Rae

- replace runtime/app-global image-key maps with Rae registries;
- move failed-load throttling and cache metadata into Rae;
- move WebGPU descriptor validation and cache keys into Rae.

### Phase H3: Remove Raw Int Handle APIs

- keep temporary compatibility wrappers while examples migrate;
- then delete or hide raw `Int` APIs from normal stdlib modules.

## Acceptance Criteria For Future Implementation

Before an opaque handle implementation is considered complete:

- scope-exit drop releases native resources exactly once;
- moved handles do not drop from the source scope;
- `opt Handle` present/absent drops correctly;
- `List(Handle)` and struct fields cascade-drop correctly;
- stale handles fail deterministically after table slot reuse;
- SDL/WebGPU/image examples build without raw pointer exposure;
- memory/resource diagnostics show no leak under 106 and renderer smoke tests.

## Open Questions

- Should `NativeHandle` be a compiler-known primitive or an ordinary stdlib
  struct with compiler-known drop metadata?
- Do we need `Result(T,E)` before replacing `0 = failure` constructors?
- How should generation counters be packed for 32-bit and WASM targets?
- Should shared native references use explicit `retain/release`, a
  `SharedHandle(T)`, or be avoided until a real need appears?
- How much thread-affinity checking belongs in the runtime versus the Rae
  scheduler/system layer?
- Should disabled capabilities use null handles, compile-time diagnostics, or
  runtime stubs during the migration window?
