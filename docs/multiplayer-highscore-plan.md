# Highscore & Networking Plan

## Goal
Establish a robust foundation for persistence and connectivity in Rae, starting with local "atomic" file swaps and ending with a real-time P2P multiplayer proof-of-concept.

---

## 1. Technical Decisions
- **HTTP & Web Server:** Use **Mongoose** (minimalistic, single-header C library).
- **Real-time Networking:** Use **ENet** (UDP with reliable/unreliable channels, industry standard for games).
- **File Persistence:** Implement "Atomic Swaps" (Rename existing -> Write new to .tmp -> Rename .tmp to final -> Delete old).
- **Concurrent Access:** Implement POSIX file locking (`flock`) to allow multiple binaries to safely write to the same score file.
- **Crypto:** Use a minimal C library (e.g., **Monocypher**) for encrypting local save files.

---

## 2. Phase 1: Language Prerequisites
Before networking can be implemented, the following language gaps must be closed:

1.  **Strict Default Initialization:** All fields/variables must initialize to `0`, `false`, or `""` (empty string) by default if not specified.
2.  **JSON Serialization:** A built-in or compiler-generated way to turn any `type` into a JSON string and back.
3.  **Binary Serialization:** Similar to JSON, but for compact binary packets (crucial for ENet/UDP).
4.  **The `defer` Keyword:** Essential for robust cleanup (e.g., `let f = open(); defer close(f);`). This ensures files are closed and locks are released even if a function returns early.
5.  **Threads over Async:** We will pursue a `spawn` model (Threads) rather than `async/await`. This aligns better with a C backend and high-performance game loops.

---

## 3. Phase 2: Local Robust Highscores
- Implement `sys.rename`, `sys.delete`, and `sys.lock`.
- Implement an atomic save function in Tetris.
- Add encryption to the local highscore file to prevent easy cheating.

---

## 4. Phase 3: Web & Real-time (The "Connected" Phase)
- **Client:** Tetris fetches leaderboards from a Vite/Node.js server using Mongoose.
- **Server:** Re-implement the highscore server in Rae using Mongoose.
- **UDP POC:** A P2P Pong mode where two players sync paddle/ball state via ENet.

---

## 5. Concurrency Model (The "Spawn" Model)
Rae uses a "reversed" async/await model to keep game loops simple:
- **Definition:** Functions that *can* run asynchronously must be marked with the `spawn` modifier: `func fetchScores() spawn { ... }`.
- **Synchronous Call:** Calling it normally `fetchScores()` runs it synchronously (blocking), similar to `await` in other languages.
- **Asynchronous Call:** To run it in the background, use the `spawn` keyword at the call site: `spawn fetchScores()`.
- **Advantage:** This ensures that by default, game logic is deterministic and sequential unless the developer explicitly asks for a background task.

## 6. Platform Compatibility (iPhone & Steam)
To ensure the highscore and multiplayer features work on iOS and Desktop:
- **Zero External Dependencies:** Only use C libraries that link statically and use standard POSIX sockets.
- **Portability:** Mongoose, ENet, and Monocypher are confirmed to work on both ARM (iPhone) and x86/ARM (Desktop/Steam).
- **TLS Note:** For production iOS HTTPS, we will integrate `mbedTLS` as a backend for Mongoose to satisfy Apple's ATS requirements.

## 7. Missing Architectural Pieces
- **Constructors:** We don't need heavy OOP constructors. We need an `init` convention or a special `init` function that the compiler calls after `{}` allocation.
- **Destructors:** To be determined. For now, `defer` handles 90% of the use cases for manual resource management.
- **Spawn/Threads:** We need a `sys.spawn(() => { ... })` primitive.
