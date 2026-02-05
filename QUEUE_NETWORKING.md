# QUEUE: Networking & Persistence

## Step 1: Language Foundation (Pre-requisites)
- [ ] **Defaults:** Enforce zero-initialization for all locals and struct fields in VM and C backend.
- [ ] **The `defer` Keyword:** Implement `defer` in the parser, VM, and C backend for scope-based cleanup.
- [ ] **Spawn Syntax:** Implement the `spawn` call-site keyword and ensure it triggers background execution for `spawn`-modified functions.
- [ ] **JSON Gen:** Implement compiler-generated `toJson()` and `fromJson()` for all `type` definitions.
- [ ] **Binary Gen:** Implement `toBinary()` and `fromBinary()` for compact packet serialization.
- [ ] **String Utils:** Add `split`, `trim`, `find`, and `replace` to `lib/string.rae`.

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
