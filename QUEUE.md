# Queue

- [x] Fix VM stack overflow crash in Advanced Pong (vm.c, vm.h)
- [x] Fix "unknown opcode" crash during hot-reload exit (vm_patch.c)
- [x] Fix C backend struct ordering and function signature resolution (c_backend.c)
- [x] Implement implicit project imports (main.c)
- [x] **Phase 1: Buffer Infrastructure**
    - [x] Add `VAL_BUFFER` to `vm_value.h` and implement its lifecycle (alloc/free).
    - [x] Add `OP_BUF_ALLOC`, `OP_BUF_FREE`, `OP_BUF_GET`, `OP_BUF_SET`, `OP_BUF_COPY` opcodes to VM.
    - [x] Expose these as `priv` built-in functions in the compiler.
    - [x] Implement C-backend mapping for raw buffers.
- [x] **Phase 2: List2 (Non-Generic Prototype)**
    - [x] Implement `List2Int` in Rae using `Buffer`.
    - [x] Implement `add`, `get`, `length`, and `remove` (with shifting) in Rae.
    - [x] Create benchmark comparing `List` (C) vs `List2Int` (Rae).
- [x] **Phase 3: Heterogeneous Lists (Any Type)**
    - [x] Implement `Any` type support in compiler and VM.
    - [x] Implement `List2` (using `Any`) in Rae.
    - [x] Add automatic boxing/unboxing in C backend.
- [x] **Phase 4: Generic List2<T>**
    - [x] Implement `List2<T>` in Rae.
    - [x] Add compiler support for generic type erasure in C backend.
    - [x] Verify `List2<Int>` and `List2<String>` work in both VM and Compiled modes.
- [x] **Phase 5: Production List Transition**
    - [x] Rename `List2` to `List`.
    - [x] Move `Buffer` and `List` implementation to `core.rae`.
    - [x] Remove C-backed `List` implementation from compiler and runtime.
    - [x] Verify all existing tests and examples still pass.
- [ ] **Phase 6: Standard Library Implementation**
    - [ ] **Compiler Infrastructure**
        - [x] Implement Type-Based Dispatch (Overloading) in VM compiler.
        - [x] Implement Type-Based Dispatch (Overloading) in C backend.
        - [x] Implement Name Mangling (`Type_method`) in C backend.
        - [ ] Implement Conditional Auto-Imports (import only if symbol used).
    - [ ] **Module: core**
        - [ ] Standardize `nowMs()`, `sleep(ms)`, `nextTick()`.
        - [ ] Move `List(T)` methods to standard naming (`add`, `get`, `length`, etc.).
    - [ ] **Module: math**
        - [ ] Implement basic math overloads (`abs`, `min`, `max`, `clamp`).
        - [ ] Standardize randomness (`seed`, `randomFloat`, `randomInt`).
    - [ ] **Module: string**
        - [ ] Implement `length`, `compare`, `concat`, `sub`, `contains`, `toFloat`, `toInt`.
    - [ ] **Module: io**
        - [ ] Standardize `log`, `logS`.
        - [ ] Implement `readLine`, `readChar`.
    - [ ] **Module: sys**
        - [x] Implement `exit`, `getEnv`, `args`, `readFile`, `writeFile`.
- [ ] **Phase 7: Tetris-Ready Features**
    - [x] **Struct-to-Struct FFI**
        - [x] Add `@c_struct` attribute or similar hint to Rae types.
        - [x] Update C backend to emit C struct literals and pass them by value to externs.
        - [x] Clean up `raylib.rae` to use `Color` and `Vector2` structs directly.
    - [x] **Language Improvements**
        - [x] Implement `else if` support in parser.
    - [x] **Enums**
        - [x] Add `enum` keyword to parser.
        - [x] Implement enum members as constant `Int` values in VM and C backend.
        - [ ] Support enum exhaustive matching in `match` statements.
    - [ ] **Infrastructure & Quality**
        - [ ] Restructure all tests to use individual folders (Implicit Project structure).
        - [ ] Enhance string interpolation: ensure automatic `.toString()` works for all types and support complex expressions.
        - [ ] Refactor existing examples to use `enum` instead of raw `Int` constants (e.g. `raylib_3d`).
    - [x] **Tetris Prototype**
        - [x] Implement a clean, high-quality Tetris in 2D using Raylib and Enums.
    

- [ ] Compiled backend: `List(Task(T))` element access lowers to `RaeAny` instead of `RaeTask*` — can't hold task handles in a `List` (generic-container element monomorphization). Workaround: use individual `Task` locals. Found building examples/40_raytracer.
- [ ] Compiled backend: `List(Struct).get()` returns boxed `RaeAny` and isn't auto-unboxed to the struct (must use low-level `rae_ext_rae_buf_get`). Generalize typed-struct-list element get(). Workaround in raytracer: flat `List(Float)` struct-of-arrays.
- [x] Compiled backend: passing a call/binary/object rvalue directly to a `view`/`mod` param dangled — the temp was a GCC statement-expression local (`({ T t = ...; &t; })`) whose lifetime ended before the callee dereferenced it (UB; read stale stack). Fixed in c_call.c by materializing into a one-element array compound literal `((T[1]){ rvalue })`, which has enclosing-block lifetime. This was the root cause of the interactive raytracer's "camera ignores yaw/pitch / always looks at origin": `buildCamera(target: vadd(camPos, fwd))` read a garbage look-at point.

- [ ] Language: add real `const` declarations (compile-time, immutable). Today "constants" are zero-arg functions (e.g. `func PI()`/`func TAU()` in lib/math.rae) because Rae has no immutable value binding (`let` is reassignable). Proposed: `const NAME: Type = <const-expr>` at module (and local) scope; UPPER_SNAKE naming; const-expr folding (literals + ops on consts); reassignment is a compile error; folded/inlined in both backends (VM constant pool, C `static const`/literal). Then convert PI()/TAU() to `const PI`/`const TAU` and drop the call `()`.
- [x] Compiler: spawn can now thread POD (cascade-drop-free) structs by value (was on the remaining list) — enables a by-value Camera struct in worker threads.

- [x] Language: `let` (immutable) / `var` (mutable) / `const` (compile-time, folded) implemented; module-level const folds to literals; naming de-enforced in parser. See docs/naming-conventions.md.
- [ ] Lint: optional compiler/linter WARNINGS for naming-convention violations (Types/enums PascalCase; functions/vars/consts/enum-cases camelCase). Must be warnings, not errors; exempt/suppressible for extern (C-binding) names. Needs a general warning framework first.

- [x] Examples: raytracer step 5 (examples/44_raytracer_5_lights) — emissive "light" material (kind 3), rae_ui-style scene lit by glowing spheres + dim ambient sky, preview/final resolution toggle (P), live camera HUD. Suite 55/0.
- [ ] Examples: raytracer step 6 — GPU raytracer (deferred; needs a compute/GPU path).
- [x] Resolved: interactive raytracer camera "ignored yaw/pitch / always looked at origin" in the compiled backend. Root cause was the dangling rvalue->view arg codegen bug (see the c_call.c item above): `buildCamera(target: vadd(camPos, fwd))` read a garbage look-at point. Fixed at the compiler level; camera now tracks yaw/pitch. (Earlier hypotheses about stale builds / left-drag were wrong.)

- [x] Live (bytecode VM): `spawn` ran the worker on a real OS thread, but the VM value model (ref-counted ValueBuffer/Object, shared string pool) is not thread-safe — concurrent workers raced on shared buffers (e.g. 4 raytracer tile workers sharing `world`) and SIGSEGV'd. Now Live runs `spawn` synchronously (inline sub-VM, returns a completed Task); compiled keeps real threads. All raytracer steps 1-5 run in Live.

- [ ] Strategy: execution targets & deployment decided — see docs/execution-targets-and-deployment.md. No TypeScript target; WASM (via the C backend) is the primary app/game target; C backend stays for native/desktop/real-time/hardware; Live VM kept on probation.
- [ ] Prototype: WASM generation via the C backend (clang/wasi-libc or Emscripten). Compile the raytracer to .wasm, run from a tiny JS harness writing into a SharedArrayBuffer drawn to a <canvas>. Measure WASM-vs-native speed. (Highest value-per-effort; gates the rest.)
- [ ] Prototype: WASM threads (SharedArrayBuffer + workers) + SIMD128 for the tiled raytracer — confirm real parallelism in the sandbox.
- [ ] Prototype: iPhone PoC app with OTA updates — Capacitor-style WKWebView shell running the Rae→WASM app, WASM/JS bundle hosted remotely and updated over-the-air without App Store resubmission (native shell fixed, WASM layer updatable). Confirm the App-Store-legal split.
- [ ] Prototype (if needed): one Rae→C native plugin driven at control-rate (e.g. audio) to validate the partition-by-data-rate bridge; Rae generates both ends.
- [ ] Later: WebGPU path from the WASM app (GPU compute / GPU raytracer step 6).

- [ ] Strategy: cross-platform tech stack & dependency policy decided — see docs/tech-stack-and-dependencies.md. Preference ordering Rae > C > any-lang-behind-clean-C-API > Rust > C++ > Obj-C; own small/controllable, buy hard domains, all foreign code behind Rae-owned APIs. Relax strict no-C++/no-Rust. Intended stack: SDL3 (platform) + WebGPU-everywhere/WGSL (render) + miniaudio (audio) + Rae-owned layers.
- [ ] Deprecate raylib: build Rae Platform/Render/2D/Audio layers, migrate examples off raylib, then drop the dependency. May keep ONE raylib example only if it stays cheap to build; otherwise remove entirely.

## New-stack on-ramp: first 3 steps toward WASM + WebGPU + compute raytracer (browser-first)
- [x] Step W1 — WASM-generation spike + smoke gate. DONE. Made rae_runtime.c wasm-portable (signals/threads/flock/fork-exec guarded under __wasm__). New example examples/46_raytracer_wasm_web: the raylib-free raytracer core → C backend → wasm32-wasip1 via wasi-sdk; emits RGB to stdout via an fbPixel extern. compiler/tools/wasm_build.sh (Rae→C→.wasm) + compiler/tools/wasm_run.mjs (WASI shim) + compiler/tools/wasm_smoke.sh (the gate: builds + runs in Node WASI, asserts 388800 bytes + lit-scene avg). Browser harness web/index.html (minimal WASI shim → canvas putImageData; same shim validated headless). Verified: correct step-3 image rendered in WASM; tests + examples 56/0 unaffected by the runtime guards. Toolchain: wasi-sdk-33 at ~/.local/wasi-sdk (set WASI_SDK). Compiled wasm ≈ native speed (vs the VM's ~100×). NOTE: needs browser confirmation of web/index.html on your machine.
- [x] Step W2 — WASM SIMD in the browser. DONE (the band-parallel JS-orchestration demo, example 47, was REMOVED — it split work in JS, not in Rae; superseded by W2b which does it hack-free with real wasm threads). What remains from W2: wasm_build.sh adds -msimd128 (clang auto-vectorizes hot float loops) + -Wl,--allow-undefined (host-supplied env imports like fbPixel). The wasmThreads flag, src/client/wasm-worker.js, and runWasmThreaded() were deleted.
- [x] Step W2b — Hack-free WASM threading: the SAME Rae `spawn`/`Task` code as step 3, compiled to threaded WASM (no JS band-split). DONE — headless (Node) AND browser. Browser host (src/client/wasm-spawn-worker.js): a runner worker runs _start (blocks on pthread_join/atomic.wait, off the page main thread) + pre-warms a thread-worker pool; wasi.thread-spawn hands an idle pooled worker its (tid,startArg) via a shared control buffer + Atomics (avoids the new-Worker-mid-block deadlock; Emscripten-pthreads trick). Server sets COOP/COEP/CORP for SharedArrayBuffer; /api/examples/wasm?threads=1 builds threaded. app.js runWasmSpawn for wasmRealThreads. Verified in Chromium: crossOriginIsolated, 4 Rae threads, image checksum identical to Node host, dashboard intact. New example examples/48_raytracer_wasm_spawn: copies step-3 scene/camera/render, spawns 4 renderTile quadrant workers + .get() + writeTile, emits the assembled frame via fbPixel. wasm_build.sh gains WASM_THREADS=1 mode: --target=wasm32-wasip1-threads -pthread -DRAE_WASM_THREADS + -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=1GiB. Runtime: pthread_join un-guarded for threaded wasm (#if !defined(__wasm__) || defined(RAE_WASM_THREADS)); rae_spawn likewise. Rae `spawn` → pthread_create → the `wasi.thread-spawn` import; host launches a worker that re-instantiates over the SAME shared memory + runs `wasi_thread_start`. Validated headless via compiler/tools/wasm_run_threads.mjs (Node worker_threads): 4 threads, 388800 bytes, image correct + seam-free (verified as PNG), join via atomic.wait works. Examples suite 58/0; native run identical. PENDING browser: COOP/COEP headers + a Web-Worker thread-spawn host that runs _start on a worker (the page main thread can't block on atomic.wait), wired behind devtools.json `wasmRealThreads`.
### GPU arc: "one compute shader, everywhere" (WebGPU on web, SDL_GPU on desktop)
The spawn work proved *same threading code, web + desktop*. This arc is the GPU analogue: a single WGSL raytrace kernel that runs in the browser (WebGPU) and on desktop (SDL3/SDL_GPU). Dependency chain is G1 → G2 → G3.

- [>] Step G1 — WebGPU compute raytracer (browser GPU). IN PROGRESS. Move rayColor off the CPU into a WGSL compute shader (raytrace.wgsl): scene spheres + camera as uniform/storage buffers, one invocation per pixel, write an output buffer/storage texture; blit to the canvas. New example examples/49_raytracer_webgpu, devtools flag `webgpu: true` + a runWebGPU path in app.js (request adapter/device, upload scene+camera, dispatch, read back to ImageData). Rae owns the scene/camera setup and ships the WGSL as an asset; the browser host owns device/queue plumbing first, then we push that behind Rae `extern`s. Self-contained (viewer already wired: COOP/COEP + canvas); highest visual payoff — real-time, not one-shot. Effort: large.
- [ ] Step G2 — SDL3 desktop platform layer (CPU raytracer first). Add lib/sdl3.rae extern bindings + a RAE_HAS_SDL3 runtime block (vendored/brew SDL3), paralleling lib/raylib.rae / RAE_HAS_RAYLIB: createWindow, poll events, present a pixel buffer/texture. Port one raytracer example's window loop to SDL3 instead of raylib — proving SDL3 as an alternate desktop backend before any GPU. De-risks the SDL3 build/link/macOS-framework integration on its own. Effort: large.
- [ ] Step G3 — Unify: one WGSL kernel on WebGPU AND SDL_GPU + progressive accumulation. Run the same raytrace.wgsl on desktop via SDL_GPU (→ Vulkan/Metal), cross-compiled to SPIR-V (naga/tint offline or SDL_shadercross). Add a thin Rae GPU-compute abstraction (lib/gpu.rae: buffer / kernel / dispatch) backed by WebGPU on web and SDL_GPU on desktop — a backend-agnostic example. Layer in progressive accumulation (HDR accumulate + tonemap across frames) with interactive camera (reset-on-move) — folds the old "interactive raytracer" onto the GPU where it belongs. The convergence; depends on G1 + G2. Effort: very large.
- [ ] Strategic (parallel track): iPhone OTA proof-of-concept. Wrap the wasm app in a WKWebView/Capacitor shell, host the bundle remotely, demonstrate over-the-air updates (native shell fixed, wasm/JS layer updatable). Proves the deployment thesis (WASM-first, OTA-legal on iOS).
- [ ] (later) Native WebGPU (Dawn or wgpu-native) headless + incremental raylib removal — once G1–G3 validate the GPU path end-to-end.
