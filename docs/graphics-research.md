# Rae Graphics Library Research (T009)

## Criteria
- **License** – permissive (MIT/Apache) to keep Rae examples redistributable.
- **Dependencies** – minimal external packages; ideally builds with the toolchains we already use for the compiler (Clang/GCC on macOS/Linux).
- **API ergonomics** – straightforward C API that can be wrapped or called from generated Rae code without heavy binding layers.
- **Rendering scope** – 2D primitives + textures are enough for Pong/Tetris; optional 3D is a bonus.
- **Footprint** – lightweight enough to bundle with the Rae examples repo.

## Candidates

### raylib
- MIT licensed, single dependency on OpenGL, ships prebuilts + CMake projects.
- Provides windowing, input, audio, math helpers, textures, 2D/3D drawing.
- Easy “single-header” style includes for quick demos (`#define RAYLIB_IMPLEMENTATION` if needed).
- Community examples already cover Pong/Tetris.
- Build: `cmake -B build && cmake --build build` (or use provided static libs).

### sokol_gfx (+ sokol_app)
- zlib licensed, minimal state-machine style API.
- Requires pairing with backend glue (`sokol_app` for window/input, `sokol_time` etc).
- Extremely small binary footprint, but expects you to manage pipelines/shaders manually.
- Build: single-header libs; compile with `-DSOKOL_IMPL`.

### NanoVG
- zlib licensed vector-drawing library atop OpenGL.
- Focused on path rendering (good for UI) but lacks window/input/audio helpers.
- Requires additional setup for window/context (GLFW/SDL).
- Better for UI overlays than full games.

## Recommendation
**raylib** is the best fit for the first Rae game demo:
- Batteries-included: handles window/input/audio so we can focus on Rae integration.
- Mature build scripts for macOS/Linux; we can vendor static libs or build from source.
- API maps cleanly to potential Rae FFI (`extern func rl_draw_rectangle(...)` etc).
- Plenty of starter assets/examples to validate our toolchain quickly.

## Integration Outline
1. Vendor raylib as a git submodule or ship download instructions in `docs/`.
2. Extend the Rae runtime/FFI layer with a thin `rae_raylib.c` wrapper exposing:
   - window lifecycle (`init_window`, `begin_drawing`, `end_drawing`, `close_window`)
   - basic drawing primitives (rectangles, circles, text)
   - input polling helpers.
3. Provide Rae declarations in a `raylib.rae` module so user code can call wrappers.
4. Hook `rae build` (from T008 plan) to copy/link the raylib static library when building the demo.

## Pong/Tetris Demo Next Steps
1. **Scaffold project** under `examples/pong-raylib/` with modules:
   - `app/main.rae` – initializes raylib wrapper, runs the game loop.
   - `game/state.rae` – ball/paddle positions, scoring.
   - `game/render.rae` – draws paddles/ball via wrappers.
2. **Build instructions**:
   ```bash
   cd examples/pong-raylib
   ./scripts/fetch_raylib.sh   # downloads/preps static lib
   ../../bin/rae build --project . --out build/pong
   ```
3. **Testing** – add a “headless” test that runs a few update ticks with raylib mocked to keep CI green.
4. **Stretch goal** – add a second example `examples/tetris-raylib/` once Pong stabilizes to showcase more complex state machines.

## Follow-up Tasks
- T016 – Vendor raylib + write C wrapper layer (2 days).
- T017 – Define Rae FFI surface + modules for graphics/input (1–2 days).
- T018 – Implement Pong example leveraging the multi-file build pipeline (1 day).
- T019 – Automate asset/lib download inside the devtools build pipeline (0.5 day).
