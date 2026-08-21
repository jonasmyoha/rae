# WebGPU bindings & the general FFI layer

Rae's renderer is written **in Rae, on low-level Rae bindings that call the C
ABI directly** — no per-function C shim:

```
Rae renderer / graphics code
    → generated low-level Rae bindings (lib/webgpu/*.rae)
    → exact C ABI symbols (wgpuDeviceCreateBuffer, …)
    → wgpu-native / WebGPU
    → GPU
```

The pieces that make this possible are **general FFI features**, not
WebGPU-specific hacks. They work for any C library (SDL3, audio, …).

## The FFI features (#497)

| Feature | What it does |
|---|---|
| `func f(...) extern("c_symbol") ret T` | Binds a Rae extern to an **exact** C symbol. Without it the mangler forces `rae_ext_<module>_<name>`; with it the symbol is emitted verbatim, and — because the symbol is declared by an included header — Rae emits **no prototype** of its own (avoiding a clash with the real declaration). |
| `cheader "path.h"` | A module directive (contextual keyword, like `open`). Every file's headers are collected and emitted once as `#include "path.h"` at the top of the C output, so `c_struct` types and `extern("symbol")` functions resolve against the real library declarations. |
| `type X: c_struct { ... }` | Declares a type whose real definition comes from a `cheader`. The compiler emits the **bare C name** (`WGPUBufferDescriptor`, not `rae_WGPUBufferDescriptor`) for variables, fields, params and compound literals, and never redefines it. |
| `Ptr` | The opaque pointer primitive — an untyped `void*` (modelled as `Buffer(void)`). Used for handles, callbacks and raw data pointers. |
| `UInt8`/`UInt16`/`Int8`/`Int16` | First-class fixed-width integers, so a generated C-ABI struct matches its C definition byte-for-byte. |

## The generator (#498)

`rae bindgen` is a subcommand of the compiler (`compiler/src/bindgen.c`): a
small, dependency-free parser for the regular WebGPU header style. It is **not**
a general C parser — it recognises the specific constructs these headers use.

Regenerate the WebGPU bindings with:

```sh
tools/gen_webgpu_bindings.sh
# or directly:
rae bindgen \
  "$HOME/.local/wgpu-native/include/webgpu/webgpu.h" \
  "$HOME/.local/wgpu-native/include/webgpu/wgpu.h" \
  --out-dir lib/webgpu --module webgpu --import-prefix webgpu \
  --cheader "webgpu/wgpu.h" \
  --module-comment "WebGPU (webgpu.h + wgpu-native wgpu.h) low-level bindings."
```

Output is **deterministic** (same headers → byte-identical files) and checked
in, so a normal build never needs the generator. Because the compiler caps
files at 1000 lines, the output is split into three files:

- `lib/webgpu/webgpu_enums.rae` — enum / flag / `#define` constants
- `lib/webgpu/webgpu_types.rae` — `c_struct` type mirrors (+ `cheader`)
- `lib/webgpu/webgpu.rae` — the functions (`import webgpu/webgpu_types`)

`cheader "webgpu/wgpu.h"` is enough for both headers, since `wgpu.h` itself
`#include`s `webgpu.h`.

### Coverage (current headers)

enums 70 · flag/const values 41 · `#define` constants 12 · structs 114 ·
functions 223 · handles 23 (→ `Ptr`).

### C type → Rae type mapping

| C | Rae | notes |
|---|---|---|
| `void` (return) | *(no `ret`)* | |
| `uint8/16/32/64_t`, `int8/16/32/64_t`, `size_t` | `UInt8/16/32/64`, `Int8/16/32/64`, `UInt64` | |
| `float`, `double` | `Float32`, `Float64` | |
| `WGPUBool` | `UInt32` | |
| enum `WGPUX` | `Int32` (values → `const … : Int32`) | C enums are int-sized |
| `WGPUFlags` set | `UInt64` (values → `const … : UInt64`) | |
| handle `WGPUXImpl*` | `Ptr` | opaque, passed by value |
| `WGPUStringView` / other structs | the `c_struct` type | by value |
| **field** pointer (any) | `Ptr` | struct holds a raw pointer |
| **param** `const S*` / `S*` (S a struct) | `view S` / `mod S` | single descriptor / out-param |
| **param** `void*` / `T*` / handle* / array | `Ptr` | pass a List's `.data` |
| return handle / struct-pointer | `Ptr` | |

Enum and flag member names keep their C spelling (`WGPUBufferUsage_Vertex`) so
they match the docs and are findable.

## List vs Buffer vs `view` at the FFI boundary (#499)

Investigated because idiomatic Rae code holds arrays in `List(T)`, not raw
`Buffer`s. Findings, from the emitted ABI:

1. **List is contiguous** — `List(T)` lowers to `struct { T* data; i64 length; i64 cap }`; `data` is a single heap block.
2. **length/cap** are the two `i64` fields.
3. **Backing memory is exposable with no copy** — `.data` *is* the raw `T*` (`Buffer(T)`).
4. **A pointer into a List** is `list.data`; length is `list.length`.
5. **Lifetime** — the List owns the block; the pointer is valid while the List is alive and un-resized. There is no borrow tracker; it is programmer discipline (see the temporal rule below).
6. **Can a List cross an extern?** Not by value — a C function wanting `const T*` cannot receive a `{data,length,cap}` struct. But its **`.data` + `.length` feed the C params zero-copy.**
7. So passing `list.data` (Buffer) + `list.length` exposes the contiguous pointer + count **without copying**.
8. This is a **general** capability, not a WebGPU hack — and the length-carrying span already exists: **`view List(T)` IS the span.** It lowers to `const rae_List_T*` (zero-copy — a pointer to `{data, length, cap}`), carries the pointer (`.data`) AND the length (`.length`), and `view`/`mod` give the read-only/mutable forms. It is already the idiomatic wrapper param: `uploadPalette(rows: view List(Float))`, `uploadMesh(verts: view List(Float), …)` take it and unpack `.data`/`.length` at the C boundary.
9. `view T` today is a pointer to **one** `T`; for a whole array you pass `view List(T)` (which carries the length) — that is the span. A distinct `view [T]` value-type would only differ cosmetically (drop the `cap` field / pass by value); it can't add invalidation *enforcement* without a borrow checker, which Rae deliberately does not have (point 5).
10. **Reallocation hazard** — `List.grow()`/`add()` may `realloc` and move the block, dangling any pointer C still holds. Safe for **synchronous** consumers (`wgpuQueueWriteBuffer` copies immediately); **unsafe** for asynchronous GPU use (mapped buffers) — do not resize a List whose storage is mapped/in flight. (Same class as QUEUE #467.)
11. **Read-only vs mutable** is already distinguished at the boundary: `view` → `const T*`, `mod` → `T*`.

**Conclusion (#500).** The raw binding layer takes `Ptr` (the C ABI requires a
raw pointer), and Rae callers pass `myList.data, myList.length` — so **`List`
stays the idiomatic caller-side container with zero copy**. The ergonomic
`List`-taking wrappers live in `gpu.rae`/`gpu3d.rae` (`uploadPalette`,
`uploadMesh`, `uploadSkinnedMesh`, `updateMeshVerts`), each taking `view
List(T)` and unpacking `.data`/`.length` at the boundary. **`view List(T)` is
the span** the earlier draft proposed as `view [T]`: same zero-copy pointer +
length, same `view`/`mod` read/write distinction, same "don't resize while
borrowed" rule. A separate `view [T]` value-type would be cosmetic and could not
enforce invalidation without a borrow checker Rae does not have. So #500's
capability is present — no new language construct is needed.

## Context bootstrap & auto-linking (#501)

A program that imports the bindings now **auto-links** wgpu-native. The build's
`uses_webgpu` detection (`compiler/src/main.c`) triggers on any module path
containing `webgpu` (so the generated `webgpu/*` modules count) **or** any
module declaring a `cheader` whose path mentions `wgpu`/`webgpu`. No manual
`-I/-L/-lwgpu_native` flags are needed — `open webgpu/webgpu; wgpuGetVersion()`
compiles, links, and runs on its own.

The instance/adapter/device **request** is async (callback-driven) and any
platform surface configuration is genuinely platform C, so that one-time
bootstrap stays in the runtime. `lib/webgpu/context.rae` (hand-written, not
generated) exposes it to Rae as opaque `Ptr`:

| Rae | Returns | Notes |
|---|---|---|
| `webgpuBootstrap()` | `Int` | Creates instance/adapter/device/queue once (idempotent). `1` on success, `0` if no GPU. |
| `webgpuDevice()` / `webgpuQueue()` | `Ptr` | Valid after a successful bootstrap. |
| `webgpuAdapter()` / `webgpuInstance()` | `Ptr` | |
| `webgpuPoll(wait: Int)` | | Advances the device event queue (map/submit callbacks). |

The C symbols (`rae_wgpu_ctx_*`) are declared in `rae_runtime.h` under
`RAE_HAS_WEBGPU`, so the generated C sees real prototypes (an implicit
declaration would assume `int` and truncate the 64-bit handle). Everything
downstream — `wgpuDeviceCreateBuffer`, `wgpuQueueWriteBuffer`, render passes —
is a Rae call over the generated bindings; `context.rae` is the only
non-generated glue and it holds **no** per-resource shim. Proven end-to-end by
`compiler/tests/cases/629_webgpu_bootstrap` (bootstrap → Rae-constructed
`WGPUBufferDescriptor` → `wgpuDeviceCreateBuffer` → zero-copy `wgpuQueueWriteBuffer`
from a `List`'s `.data`).

Descriptor construction note: a `c_struct` compound literal zero-inits omitted
fields, so `WGPUBufferDescriptor { usage: …, size: … }` leaves `nextInChain`,
`label`, and `mappedAtCreation` zeroed — a zeroed `WGPUStringView` label is
"no label". That covers most descriptors without any null-`Ptr` helper.

## Known limitations (carried, not hidden)

- **`UInt32` vs `Char32`** share `uint32_t`, so string interpolation formats a
  `UInt32` as a character. Harmless for bindings (values pass to C, not logged).
- **Callbacks** are `Ptr` (function-pointer values). Passing a Rae function to C
  is a separate future feature.
- **Struct-by-value** params/returns are rare in WebGPU and mapped directly; if
  a specific one misbehaves at the ABI it will need a targeted note.
- **Null `Ptr` / descriptor construction** ergonomics (getting a null pointer,
  taking the address of a stack struct) are minimal in the raw layer; the
  `gpu.rae` wrappers will provide helpers.
- ~120 header lines are **skipped** (reported by the generator): non-literal
  flag combinations, a couple of platform-specific struct fields, and macros
  that aren't integer literals. None are needed by the renderer path.
