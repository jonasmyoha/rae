# QUEUE: Networking & Persistence

## Step 1: Language Foundation (Pre-requisites)
- [x] **Defaults:** Enforce zero-initialization for all locals and struct fields in VM and C backend.
    - [x] Add test case `358_default_init` covering all types (Int, Float, Bool, String, enums, structs).
    - [x] VM: Ensure `AST_STMT_LET` without value emits `OP_DEFAULT_VALUE` or equivalent zeroing.
    - [x] VM: Ensure struct allocation (object literals or defaults) zeroes all fields.
    - [x] C Backend: Ensure `let` declarations without value emit `{0}` initialization.
    - [x] C Backend: Ensure struct declarations/allocations use `{0}`.
    - [x] Create example `14_defaults` demonstrating the feature.
- [x] **The `defer` Keyword:** Implement `defer` in the parser, VM, and C backend for scope-based cleanup.
    - [x] Parser: Support `defer <stmt>` syntax.
    - [x] VM Compiler: Implement a "defer stack" during compilation to emit cleanup code at every scope exit point (return, end of block).
    - [x] VM: Ensure `OP_DEFER` or similar correctly executes the deferred statement.
    - [x] C Backend: Implement `defer` using a similar cleanup-stack approach or a goto-based cleanup pattern.
    - [x] Add test case `359_defer_logic` covering multiple defers and early returns.
    - [x] Create example `15_defer_cleanup` showing file handle management.
- [x] **Spawn Syntax:** Implement the `spawn` call-site keyword and ensure it triggers background execution for `spawn`-modified functions.
    - [x] Parser: Add `spawn` as a valid call-site prefix.
    - [x] VM Compiler: Emit `OP_SPAWN` for calls prefixed with `spawn`.
    - [x] VM: Implement `OP_SPAWN` using `sys_thread` to run the function in the background.
    - [x] C Backend: Implement `spawn` call using `pthread` or equivalent.
    - [x] Add test case `360_spawn_concurrency`.
- [ ] **JSON Gen:** Implement compiler-generated `toJson()` and `fromJson()` for all `type` definitions.
- [ ] **Binary Gen:** Implement `toBinary()` and `fromBinary()` for compact packet serialization.
- [x] **String Utils:** Add `split`, `trim`, `find`, and `replace` to `lib/string.rae`.

## Step 2: Robust Local Persistence
- [ ] **File Ops:** Add `sys.rename`, `sys.delete`, and `sys.exists` to the stdlib.
- [ ] **File Locking:** Add `sys.lockFile` and `sys.unlockFile` (POSIX `flock` wrapper).
- [ ] **Crypto:** Integrate a tiny crypto library (Monocypher) for `sys.encrypt` and `sys.decrypt`.
- [ ] **Tetris Save:** Implement atomic highscore saving in `examples/94_tetris2d`.

## Step 3: HTTP & Web (Mongoose)
- [ ] **Link Mongoose:** Update root Makefile and C runtime to include Mongoose.
- [ ] **HTTP Client:** Create `lib/http.rae` with `get()` and `post()`.
- [ ] **Web Highscores:** Update Tetris to submit/fetch scores from a local Vite dev server.

## Step 4: The Rae Server
- [ ] **Server API:** Implement `http.serve(port, handler)` in Rae.
- [ ] **Highscore Backend:** Write a standalone highscore server in Rae that manages a score file.

## Step 5: Real-time Multiplayer (ENet)
- [ ] **Link ENet:** Add ENet dependency to the compiler and runtime.
- [ ] **UDP API:** Create `lib/net.rae` for UDP packet sending/receiving.
- [ ] **P2P Pong:** A proof-of-concept where two local binaries exchange paddle/ball state.

## Step 6: Platform Verification
- [ ] **Desktop (Steam):** Verify C-linking and networking on Windows/macOS/Linux.
- [ ] **iOS (iPhone):** Verify static linkage and socket permissions in a simulated iOS environment.
- [ ] **TLS Setup:** (Optional) Add mbedTLS to Mongoose for production HTTPS support.
