# Rae Language Feature Analysis & Roadmap
**Date:** January 26, 2026

## 1. Executive Summary

-   **Stability:** The compiler is currently very stable for the implemented feature set. All **146 compiler tests** and **30 example smoke tests** passed. The recent fixes to the C backend and VM stack management have resolved critical instability issues.
-   **Current Capabilities:** Rae is a functional, statically-typed language with a stable C interoperability layer, a custom bytecode VM, and a C backend. It supports basic generic structures (like `List(T)`), module systems, and key control flow constructs.
-   **Readiness:** The language is ready for **simple 2D games and basic CLI tools**. However, significant gaps in the standard library (collections, strings, async IO) and bindings (advanced Raylib, networking) prevent the development of "App Store quality" complex applications without substantial manual effort.

---

## 2. Feasibility Analysis by Product Category

Here is the breakdown of how achievable each product goal is with the current version of Rae:

| Product Category | Feasibility | Key Blockers / Missing Features |
| :--- | :--- | :--- |
| **1. Minimal 2D Tetris** | **High** | None. `raylib` bindings cover basic 2D drawing and input. `List` handles the grid logic. This can be built today. |
| **2. 3D Tetris (High Quality)** | **Medium** | Missing advanced Raylib bindings (Textures, Shaders, Audio, Models). Missing a Math library (Matrices/Quaternions) in Rae (exists in C but needs exposure). GC/Memory manual management might be tricky for 60fps without a mature allocator. |
| **3. Match-3 Game (High Polish)** | **Low-Medium** | **Critical:** No `Map`/`Dictionary` type for asset management (textures/sounds by name). No "Tweening" library for animations. Complex game logic is tedious without robust collections. |
| **4. Full Desktop App** | **Low** | No GUI library exists. Raylib is immediate mode (game-focused). Needs `raygui` bindings or a native widget library. Text shaping/handling is minimal. |
| **5. Custom Website (Wasm)** | **Experimental** | Achievable via the C backend -> Emscripten pipeline. Raylib supports web. However, no DOM manipulation or direct JS interop exists, so it would be a "Canvas-only" app (like a game or visualization). |
| **6. Video Editor** | **Very Low** | FFmpeg integration is extremely complex. Manual FFI for such a large library is unfeasible. Threading exists (`spawn`) but data sharing/synchronization primitives (mutexes, channels) are likely immature or missing. |

### 2.1. Language Feature Spotlight: Enums

**Status:** Implemented (Basic)

-   **Syntax:** Rae supports C-style enums using the `enum` keyword.
    ```rae
    enum Color {
      Red,
      Green,
      Blue
    }

    func main() {
      def c: Int = Color.Green
    }
    ```
-   **Implementation:** Enums are currently backed by integers (`Int`). The compiler maintains an `EnumTable` to resolve member names to integer values.
-   **Capabilities:**
    -   Can be used in `if/else` checks (e.g., `if c is Color.Red`).
    -   Supported in both the VM and C backend (compiles to `typedef enum`).
-   **Limitations:**
    -   **No Algebraic Data Types (ADTs):** Enums cannot hold data (payloads) like `enum Option { Some(T), None }`. This limits their use for complex state management compared to languages like Rust or Swift.
    -   **Type Safety:** Since they are treated as `Int`, strict type safety is looser than a dedicated nominal type system would enforce.
-   **Verdict:** Sufficient for state machines (Game State: `Menu`, `Playing`, `Paused`) and configuration flags, but insufficient for advanced functional patterns.

---

## 3. Gap Analysis: Features Needed vs. Nice-to-Have

To move from "Experimental Toy" to "Productive Tool," the following features are prioritized:

### A. Critical Features (The "Must Haves" for Games/Apps)

1.  **Expanded Standard Library (Collections):**
    *   **Map/HashMap:** Essential for almost any application (assets, state, caches). Currently missing.
    *   **String Manipulation:** More robust string splitting, searching, and building.
2.  **Expanded Raylib Bindings:**
    *   **Textures & Images:** Loading `png/jpg` and drawing sprites is fundamental for 2D/3D polish.
    *   **Audio:** `InitAudioDevice`, `LoadSound`, `PlaySound`. Games are silent right now.
    *   **Font Loading:** Custom TTF support for "App Store quality" UI.
3.  **Math Library:**
    *   Exposure of `Vector3`, `Matrix`, `Quaternion` math functions to Rae. doing 3D math manually in Rae is too slow/verbose; we need to call the C equivalents.
4.  **Asset Management Pattern:**
    *   A standard way to load/unload resources to prevent memory leaks, likely requiring destructors or a `defer` mechanism.

### B. "Nice to Have" (Boosts Productivity & Quality)

1.  **FFI Generator:** Automated tool to generate Rae `extern` definitions from C headers. This unlocks FFmpeg, full Raylib, SDL, etc., without hours of manual typing.
2.  **Coroutine/Async Support:** "App quality" often means non-blocking UI. `spawn` exists, but structured concurrency (async/await) makes UI/Network code readable.
3.  **Serialization (JSON):** Saving game state or config files is standard. A JSON parser/serializer is needed.
4.  **Tweening Library:** For those "juicy" animations in the Match-3/Tetris examples.
5.  **Web Build Pipeline:** A simple `rae build --target web` that wraps Emscripten would massively boost the "Custom Website" goal.

---

## 4. Recommendation for Next Steps

1.  **Immediate Win (Tetris):** Build the **2D Tetris** example. It validates the current `List` and logic stability.
2.  **Unlock Polish (Textures/Audio):** Manually bind the `Texture` and `Audio` modules of Raylib in `rae/lib/raylib.rae`. This unlocks the "Match-3" visual requirements.
3.  **Data Structures:** Implement a basic **Hash Map** in `rae/lib`. This is the single biggest blocker for writing complex logic cleanly.

**Conclusion:** Rae is stable enough for logic-heavy 2D games, but lacks the specific multimedia bindings and high-level collections required for polished, asset-rich commercial products. Prioritizing **Maps** and **Texture/Audio bindings** yields the highest ROI.
