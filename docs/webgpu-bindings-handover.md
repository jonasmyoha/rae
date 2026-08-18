# Handover: WebGPU / C bindings work

For the next agent (Codex) picking up the C-bindings epic. Read this, then
`docs/webgpu-bindings.md` for the reference detail. QUEUE tasks: **#496–#506**.

## The one rule

Rae calls the C ABI **directly** through generated low-level bindings — **never
a per-function C shim**. Path A was chosen deliberately; Path B (shim libraries
like `third_party/raylib/rae_raylib.c`) is rejected even temporarily. If you
find yourself writing a `rae_ext_webgpu_*` C wrapper, stop — the binding layer
plus `extern("symbol")` already makes the real call.

```
Rae renderer  →  lib/webgpu/*.rae (generated)  →  wgpu* C ABI  →  wgpu-native  →  GPU
```

## What already works (committed: 21fff5f6, e54d24c1, 5f5a58b8)

General FFI features (all library-agnostic, not WebGPU-only):

- `func f(...) extern("c_symbol") ret T` — binds to an exact C symbol. Emits
  **no** prototype of its own (the header declares it). Parser: `parse_func_declaration`
  in `compiler/src/parser.c`; mangler short-circuit in `rae_mangle_function`
  (`compiler/src/mangler.c`); prototype suppression in `compiler/src/c_backend.c`
  (search `extern_symbol`).
- `cheader "path.h"` — module directive; collected in `merge_module_graph`
  (`main.c`) and emitted as `#include` in `c_backend_emit_module`.
- `type X: c_struct { ... }` — emits the bare real C name (works outside raylib now).
- `Ptr` — real `void*` (sema resolves it to `Buffer(void)`; see `sema.c`).
- `UInt8/UInt16/Int8/Int16` — real fixed-width primitives (`mangler.c`,
  `runtime/rae_runtime.h` `_Generic`).

Generator: `rae bindgen` (`compiler/src/bindgen.c`). Output checked in at
`lib/webgpu/{webgpu_enums,webgpu_types,webgpu}.rae`. Regenerate with
`tools/gen_webgpu_bindings.sh`. Coverage: 70 enums, 41 flag sets, 12 consts,
114 structs, 223 functions, 23 handles.

Proven: `open webgpu/webgpu; let v: Int = wgpuGetVersion()` compiles, links
against wgpu-native, runs. `WGPUExtent3D` round-trips through Rae.

## Gotchas you will hit (learned the hard way)

- **1000-line/file cap is hard.** That's why the bindings are three files. Any
  new generated module must split too.
- **`wgpu.h` has no `WGPU_EXPORT`** — functions are detected by the `wgpu<Name>(`
  pattern, not the macro. `wgpu.h` `#include`s `webgpu.h`, so a single
  `cheader "webgpu/wgpu.h"` covers both.
- **`extern("symbol")` needs the symbol declared by an included header.** With a
  `cheader` that's automatic; standard-lib symbols (e.g. `llabs`) come from the
  runtime's `<stdlib.h>`. No header ⇒ implicit-declaration error.
- **`import` vs `open`.** `import webgpu/webgpu` gives qualified access;
  `open webgpu/webgpu` allows bare `wgpuFoo(...)` calls.
- **`UInt32` and `Char32` share `uint32_t`**, so `"{x}"` on a `UInt32` prints a
  character. Irrelevant for bindings (values go to C, not `log`), but don't
  write tests that interpolate a `UInt32`. Validate widths via `sizeof` +
  widening to `Int` (see test 627).
- **Build flags are still manual.** A program importing `webgpu` currently only
  compiles/links if you pass `-I$WGPU/include -L$WGPU/lib -lwgpu_native` etc.
  Auto-wiring that is task **#501** (below).
- **List realloc invalidates raw pointers.** `wgpuQueueWriteBuffer` copies
  synchronously (safe); never resize a List whose storage is mapped/in flight.

## The manual probe recipe (until #501 wires the build)

```sh
rae build --target compiled --emit-c --out /tmp/w/out.c prog.rae
gcc -O0 -o /tmp/w/app /tmp/w/out.c /tmp/w/rae_runtime.c \
  third_party/raylib/rae_raylib.c third_party/tinyexpr/rae_tinyexpr.c \
  -I/tmp/w -Ithird_party/raylib -Ithird_party/tinyexpr -I/opt/homebrew/include \
  -DRAE_HAS_RAYLIB -DRAE_HAS_SDL3 -DRAE_HAS_WEBGPU \
  -I$HOME/.local/wgpu-native/include -L$HOME/.local/wgpu-native/lib -lwgpu_native \
  -Wl,-rpath,$HOME/.local/wgpu-native/lib -framework Metal -framework QuartzCore \
  -framework Foundation /opt/homebrew/lib/libraylib.a -L/opt/homebrew/lib -lSDL3 \
  -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL
```

Tests: `cd compiler && perl -e 'alarm shift; exec @ARGV' 600 make test`. Do not
assume — run it. New FFI tests live in `compiler/tests/cases/` (see 626/627/628
for the pattern: `main.rae` + `config.cmd` (`run`) + `expected.txt`).

## Next tasks, in order

- **#501 — build integration + device/queue bootstrap. ✅ LANDED.** The build's
  `uses_webgpu` detection (`main.c`) now triggers on any module path containing
  `webgpu` or any `cheader` mentioning `wgpu`/`webgpu`, so importing the bindings
  auto-links wgpu-native (no manual flags). `lib/webgpu/context.rae` (hand-written)
  exposes the bootstrap + `webgpuDevice/Queue/Adapter/Instance/Poll` as opaque
  `Ptr` via `rae_wgpu_ctx_*` runtime accessors (declared in `rae_runtime.h` under
  `RAE_HAS_WEBGPU`). The async device REQUEST stays C; everything downstream is
  Rae. Headless test: `compiler/tests/cases/629_webgpu_bootstrap` (bootstrap →
  Rae `WGPUBufferDescriptor` → `wgpuDeviceCreateBuffer` → zero-copy
  `wgpuQueueWriteBuffer` from a `List`'s `.data`). See docs/webgpu-bindings.md
  "Context bootstrap & auto-linking". Surface/swapchain-view exposure is deferred
  to when #502/#503 need it (headless path needs no surface). NOTE: `List(Int32)`
  monomorphization is currently broken (lowers to `rae_List_void`); use
  `List(Int)` for now, or fix the Int32 list codegen when a binding needs it.
- **#502 — proof port: delete `drawRecords`.** Reimplement the instanced GBuffer
  draw in Rae over the bindings (Rae owns the draws storage buffer, uploads via
  `wgpuQueueWriteBuffer` zero-copy from a List's `.data`, issues
  `wgpuRenderPassEncoderDrawIndexed`). Remove `rae_ext_gbuffer_drawRecords` +
  its stub. Behaviour on example 114 must be identical.
- **#503–#504 — migrate the GBuffer geometry pass, then the rest** (lighting,
  ssao, taa, shadow, sky, gpu2d) to Rae over the bindings. WGSL shader source
  stays (as Rae string constants + `wgpuDeviceCreateShaderModule`).
- **#505 — audit + gate**: no bespoke high-level graphics C helper remains; add a
  gate rejecting new renderer-specific C functions.
- **#506 — re-sequence grass** (#486+) onto the bindings.

## Keep the raw layer raw

Do not "improve" the generated bindings into an ergonomic API. Handles stay
`Ptr`, pointers stay `Ptr`, structs stay `c_struct`. Build convenience
(typed handles, `List`-taking upload helpers, a render-graph) in a **separate**
`gpu.rae`, layered on top — so any WebGPU feature we don't use yet (compute,
indirect draw, query sets, timestamps) is reachable through the existing
bindings with **no new C**.

## Improvements to the generator you may want

- Typed handle aliases instead of bare `Ptr` (needs a Rae newtype-over-pointer;
  weigh against keeping the raw layer dead simple).
- Callbacks are currently `Ptr`; passing a Rae function to C is a separate
  feature (needed for `wgpuDeviceCreateComputePipelineAsync` etc.).
- ~120 skipped header lines are reported by `bindgen` (non-literal flag combos,
  a couple platform struct fields, non-integer macros) — none block the renderer,
  but revisit if one is needed.
