# Raylib Integration Plan for Pong

Goal: Create a playable Pong game in Rae using Raylib for graphics.

## Phase 1: Infrastructure & FFI Bootstrap
- [ ] Install Raylib locally (tooling script).
- [ ] Create `rae_raylib.c/h` wrapper to simplify Raylib types for Rae (e.g., passing individual Ints instead of structs if needed).
- [ ] Implement `extern func` declarations in Rae for core Raylib functions:
    - `InitWindow`, `WindowShouldClose`, `CloseWindow`
    - `BeginDrawing`, `EndDrawing`, `ClearBackground`
    - `DrawRectangle`
- [ ] Update `rae build` to support linking against Raylib (via flags or environment).

## Phase 2: Minimal Window Example
- [ ] Create `examples/raylib_basic.rae`.
- [ ] Verify window opens and closes cleanly via "Run Compiled" in devtools.

## Phase 3: Game Loop & Input
- [ ] Add `IsKeyDown` FFI.
- [ ] Implement paddle movement logic.
- [ ] Add `DrawRectangle` calls for paddles.

## Phase 4: Physics & Scoring
- [ ] Implement ball movement and collision detection.
- [ ] Add score tracking and display.

## Phase 5: Polish
- [ ] Sound effects (if supported by Raylib integration).
- [ ] Better visuals.
