# Rae Standard Library Ownership Model

Queue: `#297 Runtime migration architecture`

Status: design. No implementation in this task.

Depends on:

- `docs/runtime-audit-2026-07.md`
- `docs/runtime-kernel-abi.md`
- `docs/native-handle-ownership.md`
- `docs/stdlib-bootstrap-tiers.md`

## Purpose

This document defines the long-term ownership boundary for Rae's standard
library migration:

- which types remain compiler-known;
- which types can become ordinary Rae code;
- which operations need runtime support permanently;
- which resources should be opaque native handles;
- where the permanent ABI boundary sits.

The goal is not to force everything into Rae. The goal is to make the split
explicit so new library work defaults to Rae when safe, and defaults to narrow C
runtime ABI only when the operation genuinely belongs there.

## Core Rule

Use the weakest special status that still makes the type safe and efficient.

Prefer this order:

1. Ordinary Rae type in `core`, `alloc`, `std`, or `platform`.
2. Ordinary Rae type over narrow permanent runtime externs.
3. Opaque native handle with compiler/runtime-recognized drop metadata.
4. Compiler-known type only when code generation or representation requires it.

Compiler-known should be rare. It is justified when the compiler must understand
layout, move state, optional representation, drop insertion, or generic
specialization in ways ordinary Rae code cannot yet express.

## Ownership Categories

### Category A: Compiler-Known Language Types

Remain compiler-known for the foreseeable future:

- primitive scalars: `Int`, `Int64`, `Float`, `Float64`, `Bool`, `Char`;
- function types and call ABI;
- `type` parameters and generic specialization metadata;
- `opt T` representation and unwrap checks;
- `view`, `mod`, `copy`, and `own` parameter modes;
- scope-exit move/drop state.

Reason:

- these are language semantics, not library conveniences;
- generated C must know their representation and lifetime behavior;
- sema and codegen must reason about them before stdlib modules are compiled.

Migration direction:

- none for now. Improve diagnostics and representation consistency instead of
  trying to dogfood these directly.

### Category B: Compiler-Known Runtime-Backed Types

Remain compiler-known now, but may shrink over time:

- `String`;
- raw `Buffer(T)` / list storage backing;
- possibly `RaeAny` or equivalent dynamic/boxed representation while optional,
  JSON, dynamic calls, and bridge paths still need it.

Reason:

- ownership/drop behavior is central and currently codegen-sensitive;
- generated C needs stable layout and destructor calls;
- many existing bugs have been representation-confused drops or generic
  container ownership mistakes.

Target:

- keep allocation/drop kernels in C while moving algorithms into Rae;
- make policy ordinary Rae code first;
- only revisit representation after tests prove ownership through optionals,
  lists, structs, generics, moves, returns, early returns, and reassignment.

Current examples:

- `String` allocation/copy/drop/temp-pool remain runtime-supported;
- `String.contains`, `startsWith`, `endsWith`, `indexOf`, `trim`, and ASCII
  `toLower` have moved into Rae policy;
- `List(T)` policy increasingly lives in `lib/core.rae`, backed by raw buffer
  primitives.

### Category C: Ordinary Rae Core/Alloc Types

Should become or remain ordinary Rae code:

- `List(T)` policy;
- `HashMap(K,V)`, `StringMap(V)`, `IntMap(V)` algorithms;
- string builders;
- byte slices/views once designed;
- optional/result helper APIs;
- deterministic data structures and algorithms.

Required runtime support:

- raw allocation/free/realloc;
- raw buffer allocation/copy/move/get/set/drop-at;
- compiler-synthesized drop for elements where needed;
- `String` allocation/drop until its representation changes.

Reason:

- these are portable algorithms and ownership policy;
- dogfooding them exercises generics, move semantics, cascade drop, and the C
  backend;
- ordinary Rae programmers benefit from the same containers.

Gates:

- generic specialization must preserve concrete element layout;
- owned element overwrite/remove/clear/drop must have leak and double-free tests;
- map rehash/removal must be proven for owned keys and values.

### Category D: Ordinary Rae Std Policy

Should be ordinary Rae code over narrow externs:

- path manipulation and filesystem convenience policy;
- JSON parser/serializer and query helpers;
- package metadata and `.raepack` parsing;
- image registry status/retry/cache metadata;
- WebGPU descriptors, validation, caches, resource managers;
- renderer command policy and render graph;
- app-facing task/channel policy above raw threading primitives.

Required runtime support:

- narrow file/env/time/process externs;
- raw decoder/upload calls for image/audio/video;
- raw WebGPU/SDL/native API calls;
- thread/mutex/condvar/atomic primitives.

Reason:

- this code is policy, not ABI;
- it is testable as Rae source;
- keeping it in Rae prevents the runtime from becoming an app framework.

Gates:

- compatibility tests before removing the C bridge;
- deterministic tests where possible;
- platform-backed tests isolated behind feature/capability checks.

### Category E: Opaque Native Handles

Should be Rae-visible owned wrapper types over runtime-managed native resources:

- `SdlWindow`, `SdlCursor`;
- `GpuDevice`, `GpuSurface`, `GpuBuffer`, `GpuTexture`, `GpuPipeline`,
  `GpuBindGroup`, `GpuSampler`, `GpuShaderModule`;
- `ImagePixels`, `ImageTexture`;
- future audio devices, sockets, file watchers, process handles.

Representation:

- Rae should not see raw pointers;
- wrappers should contain a typed `NativeHandle` token or a compiler/runtime
  equivalent;
- runtime lookup validates kind, slot, and generation before using native
  memory.

Ownership:

- `own HandleType` releases exactly once;
- `view HandleType` borrows without releasing;
- `copy HandleType` is rejected unless an explicit shared/ref-counted wrapper
  exists;
- `opt HandleType` drops the payload when present.

Required runtime support:

- native table allocation/release/validation;
- type-specific or table-kind-specific destructors;
- raw API calls that create/use/destroy native objects.

Reason:

- platform resources need deterministic lifetime and type safety;
- users benefit from the same model as the runtime;
- raw `Int` handles are too easy to confuse and cannot be auto-dropped safely.

Reference:

- `docs/native-handle-ownership.md`.

## Permanent ABI Boundary

The permanent C runtime owns:

- allocator wrappers and raw memory operations;
- raw buffer allocation/copy/move/get/set/drop-at;
- compiler-required string allocation/copy/drop while `String` is special;
- atomics, mutexes, condition variables, OS threads;
- crash/signal hooks;
- platform ABI glue: POSIX, Win32, Cocoa/Objective-C, SDL3, WebGPU C API;
- external library entry points: image/audio/network/crypto/compression codecs;
- native handle tables and validation;
- raw callbacks and event pumps where the host API requires C function pointers.

The Rae stdlib owns:

- algorithms;
- containers and maps once ownership/codegen is stable;
- descriptors and validation;
- registries, caches, retry policy, metadata;
- render graph and resource-management policy;
- package, JSON, filesystem convenience policy;
- application-facing APIs over raw platform externs.

If a new API owns policy, it belongs in Rae. If it only crosses a raw ABI edge,
it may belong in C.

## Type Placement Table

| Type or area | Current placement | Target placement | Runtime support |
| --- | --- | --- | --- |
| `Int`, `Float`, `Bool`, `Char` | compiler-known | compiler-known | generated C representation |
| `opt T` | compiler-known | compiler-known until representation can be expressed safely | drop/unwrap codegen |
| `String` representation | compiler/runtime-known | likely compiler/runtime-known medium term | allocation/copy/drop kernel |
| String algorithms | mixed C/Rae | Rae `core`/`alloc` policy | string kernel only |
| `Buffer(T)` raw storage | runtime externs | permanent runtime kernel | raw allocation/copy/get/set/drop-at |
| `List(T)` policy | Rae over raw buffer | Rae `core`/`alloc` | raw buffer primitives |
| `HashMap`/`StringMap` policy | Rae with known bugs/bridges | Rae `alloc` | raw buffer/String kernel |
| JSON parser/serializer | Rae plus C bridges | Rae `std` | file/string primitives only |
| Filesystem/path policy | Rae plus raw sys calls | Rae `std` | raw platform file calls |
| Image registry policy | Rae | Rae `std` | raw decode/upload calls |
| WebGPU resource management | C-heavy | Rae `platform` policy | raw WebGPU calls + handles |
| SDL/gpu2d window/input | C-heavy | Rae `platform` policy over handles | raw SDL/WebGPU calls |
| Spotify integration | runtime/app glue | app/platform package policy | AppleScript/process bridge |

## Migration Rules

Before moving a type or API:

1. Classify it into Category A, B, C, D, or E.
2. Identify the bootstrap tier from `docs/stdlib-bootstrap-tiers.md`.
3. List required permanent runtime externs.
4. Prove there is no lower-tier-to-higher-tier dependency.
5. Add ownership tests for every owned field/container/optional path.
6. Keep the C bridge until Rae behavior matches.
7. Delete the bridge only in a separate cleanup after tests and diagnostics pass.

Do not migrate if:

- it requires exposing raw pointers to normal Rae code;
- it requires a language feature only useful for the runtime;
- it makes `core` depend on `std` or `platform`;
- it hides platform state inside implicit globals that ordinary apps cannot
  reason about.

## Decisions

- `String` remains compiler/runtime-known for representation and allocation/drop
  until a separate string-representation design proves a safer path.
- `List(T)` and map policy should continue moving into Rae, but raw buffer
  primitives remain permanent runtime kernel.
- `RaeAny`/dynamic boxed values are temporary/bridge territory unless a future
  dynamic type becomes an explicit language feature.
- Opaque native handles should not be plain `Int`; they need typed ownership and
  drop metadata.
- Runtime migration should prioritize ordinary Rae modules over compiler magic.

## Open Questions

- Can `String` eventually become an ordinary Rae struct without making every
  generated program depend on a large bootstrap stdlib?
- What byte-slice model is needed before parsers, codecs, and builders can be
  fully Rae-owned?
- Should `NativeHandle` itself be compiler-known, or can it be an ordinary Rae
  struct with compiler-known drop metadata?
- How should module-level globals that own resources be dropped at program exit?
- What is the final replacement for `RaeAny` bridges?
- How much typed reflection or compile-time metadata is needed for generic
  container drops without runtime type confusion?
