# ============================================
# SUMU TASK QUEUE — RAE LANGUAGE ROADMAP
# Generated: 2026-01-27
# Style: Small, well-defined, autonomous tasks
# ============================================

## A. Roadmap & Language Goals

[x] Create docs/ROADMAP.md
    [x] Paste Jan 26 feature analysis as baseline
    [x] Add sections: Now / Next / Later
    [x] Add section: Non-goals (for now)
    [x] Add product feasibility table

[x] Create docs/GOALS.md
    [x] Define Rae language philosophy
    [x] Beginner-friendly but not a toy
    [x] Games + tools first
    [x] Deterministic builds and strict formatting
    [x] First-class C interop

[x] Create docs/STDLIB_GUIDE.md
    [x] Naming conventions (length(), contains(), etc)
    [x] Ownership rules in stdlib APIs
    [x] Error handling conventions
    [x] Allocation / cleanup expectations

[x] Create docs/FFI.md
    [x] Document current extern syntax
    [x] Document current limitations
    [x] Outline future FFI generator design

---

## B. Example Validation — 2D Tetris

[ ] Create examples/tetris2d/
    [ ] Minimal game loop
    [ ] Grid logic using List
    [ ] Input handling
    [ ] Scoring system
    [ ] Pause / restart states

[ ] Add enum-based game state
    [ ] Menu
    [ ] Playing
    [ ] Paused
    [ ] GameOver

[ ] Add README.md for tetris2d
    [ ] Build instructions
    [ ] Run instructions
    [ ] VM vs C backend notes

[ ] Add CI smoke test for tetris2d
    [ ] Compiles successfully
    [ ] Runs headless or minimal frame loop

---

## C. Stdlib — Map / HashMap (Critical)

[ ] Design Map(K,V) API
    [ ] new()
    [ ] length()
    [ ] isEmpty()
    [ ] has(key)
    [ ] get(key) -> opt V
    [ ] set(key, value)
    [ ] remove(key) -> opt V
    [ ] clear()

[ ] Implement Map(String, V)
    [ ] Hash function for String
    [ ] Collision handling
    [ ] Resize / rehash logic

[ ] Implement Map(Int, V)
    [ ] Hash function for Int
    [ ] Shared core logic with String map

[ ] Add Map iteration support
    [ ] keys()
    [ ] values()
    [ ] entries()

[ ] Write Map tests
    [ ] Insert / lookup
    [ ] Remove
    [ ] Collision behavior
    [ ] Resize behavior
    [ ] VM + C backend parity

[ ] Add simple Map perf sanity test
    [ ] Insert 10k entries
    [ ] Lookup 10k entries

---

## D. Stdlib — Set

[ ] Design Set(T) API
    [ ] new()
    [ ] add(value)
    [ ] remove(value)
    [ ] has(value)
    [ ] length()

[ ] Implement Set using Map(T, Unit)
[ ] Add Set tests

---

## E. Stdlib — Strings

[ ] Add String.split(sep)
[ ] Add String.trim()
[ ] Add String.trimLeft()
[ ] Add String.trimRight()
[ ] Add String.startsWith()
[ ] Add String.endsWith()
[ ] Add String.contains()
[ ] Add String.indexOf()
[ ] Add String.lastIndexOf()

[ ] Write String tests
    [ ] ASCII cases
    [ ] UTF-8 sanity cases
    [ ] Empty string cases

---

## F. Stdlib — StringBuilder

[ ] Design StringBuilder API
    [ ] add(String)
    [ ] addChar(Char)
    [ ] addInt(Int)
    [ ] toString()

[ ] Implement StringBuilder
[ ] Add tests for large concatenations

---

## G. Raylib Bindings — Textures & Images

[ ] Add texture bindings
    [ ] LoadTexture
    [ ] UnloadTexture
    [ ] DrawTexture
    [ ] DrawTextureEx

[ ] Add image bindings if required
    [ ] LoadImage
    [ ] UnloadImage

[ ] Add texture example
    [ ] Load PNG
    [ ] Draw sprite
    [ ] Cleanup on exit

---

## H. Raylib Bindings — Audio

[ ] Add audio device bindings
    [ ] InitAudioDevice
    [ ] CloseAudioDevice

[ ] Add sound bindings
    [ ] LoadSound
    [ ] PlaySound
    [ ] UnloadSound

[ ] Add audio example
    [ ] Play sound on keypress

---

## I. Raylib Bindings — Fonts

[ ] Add font bindings
    [ ] LoadFont
    [ ] LoadFontEx
    [ ] UnloadFont
    [ ] DrawTextEx

[ ] Add font rendering example
    [ ] Render TTF text

---

## J. Math Library Exposure

[ ] Expose Vector2
[ ] Expose Vector3
[ ] Expose Vector4
[ ] Expose Matrix
[ ] Expose Quaternion

[ ] Bind common math operations
    [ ] add / sub / mul
    [ ] dot / cross
    [ ] normalize
    [ ] matrix multiply

[ ] Add math tests
    [ ] Known-value comparisons
    [ ] VM + C backend parity

---

## K. Resource Lifetime Management

[ ] Decide cleanup mechanism
    [ ] defer statement OR
    [ ] destructor / drop hook

[ ] Implement chosen mechanism
[ ] Add scope-based cleanup tests

[ ] Add assets lifecycle example
    [ ] Load once
    [ ] Unload on exit
    [ ] No leaks on reload

---

## L. Enums — Type Safety

[ ] Make enums nominal types
[ ] Prevent implicit enum <-> Int assignment
[ ] Update VM backend
[ ] Update C backend
[ ] Update enum tests

[ ] Write docs/ENUMS.md
    [ ] Current enum model
    [ ] Future ADT direction
    [ ] match exhaustiveness plan

---

## M. JSON Serialization

[ ] Implement JSON parser
    [ ] null
    [ ] bool
    [ ] number
    [ ] string
    [ ] array
    [ ] object

[ ] Implement JSON serializer
    [ ] Stable ordering option

[ ] Add JSON tests
    [ ] Round-trip tests
    [ ] Invalid input tests

[ ] Add JSON config example
    [ ] Load settings
    [ ] Apply at runtime

---

## N. Tweening / Animation Helpers

[ ] Design tween API
    [ ] Tween type
    [ ] update(dt)
    [ ] value()

[ ] Implement easing functions
    [ ] linear
    [ ] easeIn
    [ ] easeOut
    [ ] easeInOut

[ ] Add tween animation example

---

## O. Web / Wasm Pipeline

[ ] Document manual wasm build steps
    [ ] docs/WASM.md

[ ] Add rae build --target web prototype
    [ ] Emscripten wrapper
    [ ] HTML + JS loader output

[ ] Add wasm demo example
    [ ] Canvas-only raylib demo

---

## P. FFI Generator Tool

[ ] Design FFI generator spec
    [ ] Input headers
    [ ] Output extern declarations
    [ ] Supported C features (phase 1)

[ ] Implement minimal FFI generator CLI
[ ] Add fixture header for tests
[ ] Generate bindings from fixture
[ ] Validate generated code compiles

---

## Q. Concurrency Primitives

[ ] Document current spawn behavior
    [ ] docs/CONCURRENCY.md

[ ] Implement Mutex
[ ] Implement Channel
    [ ] send
    [ ] recv

[ ] Add concurrency tests
    [ ] Basic synchronization
    [ ] Contention sanity

---

## R. RUICS Port Preparation

[ ] Create docs/RUICS_PORT_PLAN.md
    [ ] Define library boundary
    [ ] List required stdlib features
    [ ] Define UI graph serialization format

[ ] Create examples/ruics_spike/
    [ ] Empty scaffold
    [ ] TODO markers for future port

---

## S. QA & CI

[ ] Add feature support matrix
    [ ] VM
    [ ] C backend

[ ] Expand stdlib test coverage
[ ] Run formatter on all tests/examples
[ ] Ensure formatter is deterministic
