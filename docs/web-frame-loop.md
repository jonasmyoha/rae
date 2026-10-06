# The browser frame loop: Asyncify, SIMD and a frame callback

**Status (2026-10-06):** implemented, option B (§4). The real browser
slowdown was the missing WebAssembly SIMD, and that is fixed (`-msimd128`,
§2). An entry module may now declare `func setup() ret T` and
`func frame(app: mod T) ret Bool` instead of `main`; the browser then calls
`frame` once per animation frame, without the per-frame Asyncify yield (§6).
122/123_physics_playground use it.

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

**Decision (maintainer, 2026-10-06): B.** Rae keeps having no function
values: a function passed as an argument is a pattern the language does not
want, and an entry convention is the same kind of rule `main` already is. The
state is one owned value, the ECS way (a world or app that systems take as
`mod`), not a closure.

## 5. The entry convention

An entry module (the file `rae run`/`build` is given) that has no `main` and
declares

```rae
func setup() ret PlaygroundState { ... }
func frame(state: mod PlaygroundState) ret Bool { ... }
```

gets a generated C `main` (`emit_frame_entry` in `compiler/src/c_backend.c`):

- **Natively:** `state = setup(); while (frame(&state)) {}`, then the state
  is dropped. The same program, the same loop it would have written.
- **In the browser:** `setup` runs once, its result goes into a heap box, and
  `emscripten_set_main_loop_arg` calls a generated trampoline each animation
  frame. When `frame` returns false the loop is cancelled and the box dropped
  and freed.
- **Rules:** `setup` takes nothing and returns one value; `frame` takes one
  `mod` parameter of that type and returns `Bool` (false ends the program).
  Any other shape is a build error. A program with `main` is untouched: `main`
  wins, so a module may still have functions called `setup` or `frame`.
- **Headless modes** that run to an end (tests, benchmarks) stay plain
  loops inside `setup` and finish with `Sys.exit(code:)`.

122_physics_playground_port (and 123, the same file) moved its window loop
into this form: `PlaygroundState` owns the renderer, the UI world and
systems, the app, the physics world and the scene; `setup` builds it (the
renderer's GPU handles are moved in under `unsafe`, never copied); `frame` is
the old loop body and returns false where the loop used to `break`.

## 6. The browser build of a setup/frame program

The build sees the generated entry (its `RAE_FRAME_CALLBACK_ENTRY` marker)
and links with:

- `-DRAE_WEB_FRAME_CALLBACK`: `Gpu2d.pollClose` no longer awaits the next
  animation frame, and the end-of-frame present poll does not block (the
  browser paces the frames; natively it is still the blocking backpressure
  poll).
- `-sASYNCIFY_IGNORE_INDIRECT=1`: Asyncify stays only for the waits that are
  still blocking (requesting the WebGPU adapter and device during `setup`),
  which are direct calls.

**Limit:** a blocking GPU readback (a buffer map waited on with
`rae_wgpu_poll(1)`) cannot run inside `frame` in the browser, because a frame
callback has no stack Asyncify can return to the browser through; it fails
with "null function". A frame that needs one must request it and read it on a
later frame. Programs with `main` keep the Asyncify loop and can still block.

Measured as §2 (headless Chrome 154, 5 000 boxes, autofire, 300 frames, two
interleaved runs each; the wall time per frame follows from the fixed steps
taken at 60 Hz):

| build | .wasm | fixed steps / 300 frames | wall per frame | physics per frame |
|---|---|---|---|---|
| `main` loop, Asyncify yield per frame | 2.52 MB | 449-467 | ~25 ms (~40 fps) | 16.8-17.3 |
| `setup`/`frame` callback | 2.13 MB | 326-334 | ~18.6 ms (~54 fps) | 16.1-16.6 |

The frame's own work is the same (~18.5 ms). The difference is the yield:
the Asyncify loop unwinds to the browser and then waits for a fresh
animation frame after the work, so a frame that overruns 16.7 ms loses most
of the next one too. The callback returns, and the browser calls it again as
soon as it can. The .wasm is 15% smaller, since indirect calls are no longer
instrumented. (Physics per step is not comparable between the rows: the
callback takes ~1.1 steps a frame against ~1.5, and the first step of a frame
costs more.)
