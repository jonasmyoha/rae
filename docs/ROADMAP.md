# Rae Language Roadmap

## 1. Vision & Current Status (Jan 2026)

Rae is a functional, statically-typed language with a custom bytecode VM and a C11 backend. It prioritizes creative systems, games, and tools with first-class C interop and hot-reloading capabilities.

### Product Feasibility Baseline

| Product Category | Feasibility | Key Blockers / Missing Features |
| :--- | :--- | :--- |
| **1. Minimal 2D Tetris** | **High** | None. `raylib` bindings cover basic 2D. `List` handles grid. |
| **2. 3D Tetris (High Quality)** | **Medium** | Missing Textures, Shaders, Audio, Models, Math lib. |
| **3. Match-3 Game (High Polish)** | **Low-Medium** | No `Map` type, no Tweening library. |
| **4. Full Desktop App** | **Low** | No native widget library. Text shaping is minimal. |
| **5. Custom Website (Wasm)** | **Experimental** | Canvas-only via C backend + Emscripten. No DOM interop. |
| **6. Video Editor** | **Very Low** | FFmpeg integration complexity, threading maturity. |

---

## 2. Development Phases

### Now: Validation & Core Utility
*Focus: Making the language productive for 2D games.*

- [ ] **Example Validation:** Complete a high-quality **2D Tetris** to stress-test `List` and `raylib` integration.
- [ ] **Stdlib - Map/HashMap:** Implement a native `Map(K, V)` for asset and state management.
- [ ] **Raylib - Multimedia:** Bind `Texture`, `Image`, `Audio`, and `Font` modules.
- [ ] **Math - Native Speed:** Expose C-level `Vector`, `Matrix`, and `Quaternion` operations to Rae.

### Next: Safety & Robustness
*Focus: Moving beyond "basic" features into professional-grade tooling.*

- [ ] **Resource Management:** Implement `defer` or destructors for automated cleanup.
- [ ] **Enum Type Safety:** Transition enums to nominal types (prevent implicit `Int` conversion).
- [ ] **Data Interchange:** Built-in **JSON** parser and serializer.
- [ ] **Animation:** Create a standard **Tweening** library for "juicy" interactions.

### Later: Ecosystem & Scale
*Focus: Distribution and complex system architecture.*

- [ ] **Web/Wasm Pipeline:** Automated `rae build --target web` using Emscripten.
- [ ] **FFI Generator:** Tool to generate Rae `extern` declarations directly from C headers.
- [ ] **Concurrency Primitives:** Mature `Mutex` and `Channel` implementations for `spawn`.
- [ ] **UI Framework:** Port or implement a native UI library (RUICS) in Rae.

---

## 3. Non-Goals (For Now)

- **Garbage Collection:** Rae will remain focused on explicit references (`view`, `mod`) and manual/scoped memory management.
- **Complex OOP:** No plans for class inheritance; favoring composition and interfaces/traits.
- **Direct JS Interop:** Focus is on Wasm as the web target via the C backend.

---

## 4. Feature Analysis Baseline (Jan 26, 2026)

### Language Feature Spotlight: Enums
**Status:** Implemented (Basic)
- Supports C-style enums (`enum Color { Red, Green }`).
- Backed by `Int`, supported in VM and C backend.
- *Limitation:* No payloads (ADTs) yet.

### Stability
- 100% success on 146 compiler tests.
- 100% success on 30+ example smoke tests.
- VM stack management and C backend dependency ordering are stabilized.
