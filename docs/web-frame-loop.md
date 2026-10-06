# The browser frame loop: Asyncify, SIMD and a frame callback

**Status (2026-10-06):** measured. The real browser slowdown was the missing
WebAssembly SIMD, and that is fixed (`-msimd128`, §2). The frame-callback API
that would replace Asyncify is designed here but **not implemented**: Rae has
no function values, so every form of it is a new language or compiler rule,
and that needs the maintainer's decision (§4).

## 1. How a Rae app runs in the browser today

A Rae app owns its loop (AGENTS.md "UI rendering loops"):

```rae
loop not Gpu2d.pollClose() {
  # input, step, render
}
```

A browser cannot block the main thread, so `rae build --target wasm` links
with `-sASYNCIFY`. `pollClose` calls `rae_browser_next_frame`, an
`EM_ASYNC_JS` that awaits `requestAnimationFrame`. Asyncify unwinds the
WebAssembly stack to return to the browser, and rewinds it on the next frame,
so the Rae loop never notices. The price is instrumentation: every function
that may be on the stack at a yield gets extra code to save and restore its
locals. Asyncify cannot see through function pointers, so it assumes any
indirect call may yield. The physics step runs its `parallelLoop` bodies
through function pointers, so the whole step is instrumented.

## 2. What it costs: measured

The 122_physics_playground_port browser build in headless Chrome 154
(WebGPU on Metal), single-threaded, release, at 5 000 boxes with autofire
(RAE_PLAYGROUND_PROFILE through `index.html?RAE_…`). 300 frames and 466
fixed steps each, ms per frame, two interleaved runs each:

| build | .wasm | physics per frame | whole frame |
|---|---|---|---|
| as it was (Asyncify, no SIMD) | 2.57 MB | 34.3-35.4 | 44.9-52.3 |
| `-sASYNCIFY_IGNORE_INDIRECT=1` (indirect calls never yield) | 2.20 MB | 34.0 | 45.1 |
| `-msimd128` (Asyncify as before) | 2.52 MB | **19.3-20.2** | **35.0** |

- **The slowdown was the missing SIMD, not Asyncify.** The WASI build
  (`compiler/tools/wasm_build.sh`) always passed `-msimd128`; the browser
  build did not, so `lib/Float4` fell back to scalar code in the solver. With
  it the browser's step is ~12.7 ms (19.8 / 1.55 steps a frame), 1.7x faster.
  Every current browser runs WASM SIMD (Chrome 91, Firefox 89, Safari 16.4),
  so `-msimd128` is now always on (`compiler/src/main.c`).
- **Asyncify's own cost is small here:** ~4% of the physics, ~15% of the
  rest of the frame, ~17% of the code size. Dropping the instrumentation of
  indirect calls (`ASYNCIFY_IGNORE_INDIRECT`) would recover that, but it is
  only safe while no function reached through a pointer yields. A
  `parallelLoop` body that waits on a GPU readback would then crash on the
  web and nowhere else, so it is not on by default.

## 3. What a frame callback has to solve

`emscripten_set_main_loop_arg(frame, state, 0, 1)` (or a
`requestAnimationFrame` loop) calls a C function once per browser frame and
needs no Asyncify. In Rae terms:

- **Ownership between frames.** The loop's locals (renderer, UI world, app,
  physics world) live in `main`'s frame today. With a callback they must
  outlive `main`'s return: one value owns all of it, the runtime holds it
  between frames, and it is dropped once when the loop ends. That keeps the
  no-globals rule (the state is owned and passed, never module-level).
- **The frame function.** One Rae function `frame(state: mod T) ret Bool`
  (false when the app is done). Rae has no function values, so the compiler
  has to know this function by a rule, not by being handed it.
- **Natively nothing changes.** The same program on the C backend runs
  `loop frame(state: state) { }` with the same state type, so an app is
  written once.

## 4. Options (for the maintainer)

**A. An intrinsic that names the frame function.** `main` builds the state
and ends with:

```rae
ret Gpu2d.runFrames(state: own app, frame: playgroundFrame)
```

`frame:` takes a function name (only a name, checked to have the signature
`func(state: mod T) ret Bool`). Natively it lowers to the loop; on the web
it lowers to a heap box of the state plus `emscripten_set_main_loop_arg`
over a generated C trampoline. This is the smallest change: one intrinsic,
no general function values, and the call site says exactly which function
runs each frame. Its cost is one place in the language where a function
name is an argument.

**B. An entry convention.** A program that defines
`func setup() ret T` and `func frame(state: mod T) ret Bool` in its entry
module gets a generated loop instead of calling `main`. There is no new
argument kind, but the behaviour comes from what a function is named, which
is less explicit than A (a reader has to know the rule).

**C. Keep Asyncify** (with SIMD, as now). No language change; the ~15% frame
and ~17% size cost stay, and every app keeps its own loop exactly as
written.

**Recommendation: A**, because it is explicit at the call site and keeps one
loop shape for native and web. It is not worth doing before the renderer's
GPU frame (docs/physics-performance-plan.md §10) and WASM threads (backlog
#245), which cost more in the browser than Asyncify does.

## 5. Porting an example (after the decision)

122_physics_playground_port's `runWindowed` becomes `setup` (open the
window, build the renderer, UI world, app and physics world into one
`PlaygroundState`) and `playgroundFrame(state: mod PlaygroundState)
ret Bool` (today's loop body; returns false where the loop would exit). The
headless modes (determinism, scripted, scaling) keep their plain loops.
