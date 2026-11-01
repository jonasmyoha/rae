# QUEUE: Networking & Persistence

A planning list, not a task queue: the queue is the root `QUEUE.md`. Rae's
networking is designed in **`docs/server-benchmarks-design.md`** (servers:
`lib/net`, `lib/http`, `lib/webSocket`, `lib/net/ecs`) and
**`docs/network-connections.md`** (clients through the system's stack). Read
those before adding anything here.

**No GPL library, ever.** The old Step 3 linked Mongoose, which is GPL-2.0 or
a paid license; it was dropped. A native library Rae links must carry a
permissive license (MIT, BSD, zlib, Apache-2.0, ISC, CC0 or public domain),
and Rae itself comes first (AGENTS.md: the runtime C rule;
`docs/tech-stack-and-dependencies.md` for the order of preference).

## Step 1: Language Foundation
- [x] **Defaults:** zero-initialization of locals and struct fields (case 358, example `14_defaults`).
- [x] **`defer`:** scope-based cleanup (case 359, example `15_defer_cleanup`).
- [x] **`spawn`:** background execution, joined through `Task(T)` (case 360 and the spawn cases).
- [x] **JSON Gen:** the compiler derives `toJson()` / `fromJson()` for every `type` (cases 665, 1028, 1029).
- [ ] **Binary Gen:** `toBinary()` / `fromBinary()` for compact packets (not started).
- [x] **String Utils:** `split`, `trim`, `indexOf`, `replace` in `lib/String.rae`.

(The VM items of the old list are history: the bytecode VM was removed, #957.)

## Step 2: Robust Local Persistence
- [x] **File Ops:** `Sys.rename`, `Sys.delete`, `Sys.exists`.
- [x] **File Locking:** `Sys.lockFile` / `Sys.unlockFile`.
- [x] **Crypto:** Monocypher in every compiled program: `lib/crypto` and `Sys.encrypt` / `decrypt` (`docs/crypto.md`).
- [x] **Database:** SQLite through `lib/sql` (`docs/sql.md`).
- [x] **Secrets:** the Apple Keychain through `lib/Keychain` (`docs/keychain.md`).
- [ ] **Tetris Save:** atomic highscore saving in `examples/94_tetris2d` (write a temp file, `Sys.rename` it over).

## Step 3: HTTP & Web (Rae's own, no Mongoose)
- [x] **HTTP server:** `lib/http` (the codec) and `lib/net/ecs` (the ECS server, keep-alive, pipelining, promoted requests answered by `spawn`ed tasks), over `lib/net`'s sockets and poller.
- [x] **WebSocket server:** `lib/webSocket` + `lib/net/ecs/WebSocketSystems`.
- [x] **Wake the poller from a worker:** `pollerWake` (`lib/net/Poller`).
- [x] **HTTP client on Apple:** `lib/net/HttpGet` (GET over `lib/net/NetworkConnection`, Network.framework, TLS with the system trust store); WebSocket client too.
- [ ] **HTTP client off Apple:** the same API over a TLS library (license above) or the platform's own stack; and POST.
- [ ] **Web Highscores:** Tetris submits / fetches scores from a Rae server (Step 4).

## Step 4: The Rae Server
- [x] **Server API:** `lib/net/ecs` (`httpServerFrame`, a router, `respond`, `promote`); the benchmarks in `benchmarks/servers`.
- [ ] **Highscore Backend:** a standalone highscore server in Rae over `lib/net/ecs` and `lib/sql`.
- [ ] **Server TLS:** HTTPS / wss:// served by Rae (a license-clean TLS library; until then a TLS-terminating proxy in front).

## Step 5: Real-time Multiplayer
- [ ] **UDP API:** datagrams in `lib/net` (one-call shims, the policy in Rae, as `lib/net/Tcp`).
- [ ] **Reliability layer:** decide between Rae's own over UDP and ENet (MIT) behind one binding; write the decision in a doc first.
- [ ] **P2P Pong:** two local binaries exchange paddle / ball state.

## Step 6: Platform Verification
- [ ] **Desktop:** networking on Windows (no sockets backend yet: `lib/net` answers `unsupported`), macOS, Linux.
- [ ] **iOS:** `lib/net/NetworkConnection`, `lib/Keychain` and `lib/sql` run on a device (they compile for arm64; not yet run there).
