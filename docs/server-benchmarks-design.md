# Server benchmarks: design

**Status:** design, awaiting the maintainer's decisions (§8). Nothing is
implemented. Written 2026-10-08.

A small suite that measures Rae servers against the same servers in Rust and
JavaScript, before real web backends and game servers are written in Rae.
Three phases: HTTP, WebSocket, and a game-style room.

## 1. What Rae can do today (the honest starting point)

| capability | today | consequence |
|---|---|---|
| TCP sockets | **none**: no `socket`/`listen`/`accept` anywhere in `lib/` or the runtime | phase 1 cannot start until a `net` module exists (gap G1) |
| readiness polling (kqueue / epoll) | the runtime uses kqueue/inotify for one thing, `lib/FileNotify` (runtime C behind a handle API) | that file is the model for a socket poller (G2); there is no general event loop over file descriptors |
| threads | `spawn f(...)` → `Task(T)` on real OS threads; `Channel(T)` (MPSC, non-blocking, any value-type payload since #969); `Parallel.workerCount()` | enough to run one event loop per core and hand accepted sockets (plain `Int` handles) to workers over a `Channel(Int)` |
| bytes | `List(UInt8)`, `Buffer(T)`, `String` is byte-indexed (`byteAt`, `sub`, `indexOf`, `startsWith`, `split`) | an HTTP parser can be written in plain Rae; a reusable read/write byte buffer that does not allocate per request is missing (G3) |
| JSON | `lib/Json` (a parsed document + value builders) | enough for `{"message":"Hello, World!"}` and the shootout messages; serialisation speed is part of what phase 1 measures |
| time | `nowNs()`, `sleep(ms:)`, `lib/Time` (log formatting) | an HTTP `Date` header (IMF-fixdate) and a sub-millisecond tick wait are missing (G4) |
| hashing | monocypher (argon2, blake2b) in the runtime | no SHA-1 or base64, which the WebSocket handshake needs (G5) |
| FFI | `unsafe extern("c_symbol")`, `cheader`, generated bindings (Box3D track B, `lib/webgpu`) | the libc socket API could be bound, but see fork F1: its macros, `errno`, `sockaddr` unions and variadic `fcntl` would leak unsafe C shapes into every server |

**The `view`/`mod`-not-in-structs rule shapes connection state.** A server
cannot keep a `mod Socket` or a `view Request` in a struct. So:

- a socket is a **value handle** (`SocketId`, an `Int` the runtime maps to the
  file descriptor), stored freely — the way GPU resources are ids;
- a connection's state (`SocketId`, its read buffer `List(UInt8)`, write
  buffer, parse state, room membership) is an owned **value** held by one
  owner: the event loop's `List(Connection)` or, ECS-style, components on a
  connection entity (an `IntMap` from socket to `EntityId`);
- a handler borrows it for one call (`func handleReadable(connection: mod
  Connection, ...)`) and returns; nothing outlives the call by reference;
- each worker thread owns its own connections outright; threads exchange only
  `Int` socket handles over a `Channel(Int)`, never shared state.

This is the same ownership shape as the rest of Rae (an App or World owns,
systems borrow) and needs no new language feature.

## 2. Directory layout and the A/B/C buckets

The repository already has a convention: `benchmarks/<name>/` with one folder
per language, a `run.sh`, a gitignored `build/`, and committed `results/` and
`site/` (`benchmarks/list_access`). External sources live in a cache outside
the repository (`~/.cache/rae/box3d-oracle`, docs/physics-performance-plan.md
§9). The suite follows both, in a new single-word folder:

```text
benchmarks/servers/                      A  committed
  README.md                              A  how to run, what is measured
  spec/Http.md  spec/WebSocket.md  spec/GameRoom.md
                                         A  our own short specs (§3)
  external.lock                          A  pinned manifest: name, repo URL, commit SHA / version, licence
  fetch.sh                               A  clones/builds the pinned externals into the cache
  run.sh                                 A  the runner (§5); `make bench-servers` calls it
  check.sh                               A  the correctness checker (§5)
  rae/http/Main.rae                      A  phase 1 server (thin: lib/net + lib/http do the work)
  rae/webSocket/Main.rae                 A  phase 2
  rae/gameRoom/Main.rae                  A  phase 3
  rust/  Cargo.toml  Cargo.lock  src/bin/{http,webSocket,gameRoom}.rs
                                         A  our references (§4); crates are B
  javascript/node/  package.json  package-lock.json  Http.js  WebSocket.js  GameRoom.js
                                         A  Node references; node_modules is B
  javascript/bun/   Http.js  WebSocket.js  GameRoom.js
                                         A  Bun references (built-in server, no deps)
  loadClient/  Cargo.toml  Cargo.lock  src/main.rs
                                         A  our WebSocket + game-room load/measure client (fork F4)
  results/  summary.json  metadata.json  A  the latest dated baseline (fork F6)
  build/                                 C  gitignored (benchmarks/**/build/ already is)
~/.cache/rae/servers/                    B  fetched: reference repos at pinned SHAs, built load tools
```

- **A, committed:** everything needed to reproduce a number and nothing that a
  script can regenerate — the specs (so external drift cannot change what we
  measure), the Rae servers, our Rust/JS references with their lockfiles, the
  runner, fetcher, checker, and the manifest of external pins.
- **B, fetched:** third-party code we only run: the load generators, and
  (read-only, for comparison while writing our references) the shootout and
  TechEmpower repositories. Pinned by SHA in `external.lock`, cloned by
  `fetch.sh` into the cache. Cargo crates and npm packages are also B: the
  lockfiles (A) pin them, the toolchains fetch them.
- **C, generated:** `build/` (binaries, generated C), raw per-run results
  (`build/raw/*.json`), charts. Never committed — except the summary in
  `results/` if fork F6 says so.
- Nothing external is copied into the repository, so no third-party licence
  is carried. If that changes, the copied file gets the one-line notice the
  repository's per-file copyright rule requires.
- Reusable Rae code does not live under `benchmarks/`: sockets, the HTTP
  codec and the WebSocket codec are stdlib modules (`lib/net/`, `lib/http/`,
  `lib/webSocket/` — tickets G1–G6), so the benchmark servers stay a page each
  and real servers use the same code.

## 3. The benchmarks (our specs, short)

Each spec is a page in `spec/`; the essentials:

**Phase 1 — HTTP/1.1** (TechEmpower test types 1 and 6, their rules):

- `GET /plaintext` → `200`, body `Hello, World!`, headers `Content-Type:
  text/plain`, `Content-Length: 13`, `Server`, `Date`. Must handle
  **pipelining** (16 requests sent back-to-back on one connection before any
  reply; replies in order).
- `GET /json` → `200`, body `{"message":"Hello, World!"}` serialised **per
  request** (no cached bytes), `Content-Type: application/json`,
  `Content-Length`, `Server`, `Date`.
- Keep-alive on; anything else → `404`. Runs at 256 connections (plaintext
  also at 16-deep pipelining).

*Server specifics, briefly:* keep-alive means one TCP connection carries many
requests, so the benchmark measures request handling, not connection setup.
Pipelining sends many requests without waiting, which rewards a server that
parses everything it has read and answers in one write.

**Phase 2 — WebSocket** (the websocket-shootout protocol):

- Upgrade on `GET /ws`; text frames carrying JSON.
- `{"type":"echo","payload":X}` → the same message back to the sender.
- `{"type":"broadcast","payload":X}` → `{"type":"broadcast","payload":X}` to
  **every** connected client, then `{"type":"broadcastResult","payload":X,
  "listenCount":N}` to the sender.
- Measured: broadcast round-trip latency (p50/p99) as clients ramp up in
  steps (1k, 2k, … until p99 passes 250 ms), and the client count reached.

*Briefly:* a WebSocket starts as an HTTP request that is "upgraded" to a
long-lived two-way connection of small frames. Broadcast is the expensive
case: one message in, N messages out.

**Phase 3 — game room** (ours; no external standard exists):

- N clients join one room over the phase 2 WebSocket; each sends a 32-byte
  input message at 30 Hz.
- The server runs a fixed **30 Hz tick**: apply inputs, then send every
  client a state snapshot (16 bytes per player, so the snapshot grows with N).
- Measured at N = 100, 1 000, 5 000: tick duration (server-side work per
  tick), **tick jitter** (actual tick start minus scheduled start, p50/p99/
  max), client-observed snapshot inter-arrival jitter, missed ticks, CPU and
  RSS. The fan-out cost (N snapshots of size ∝N per tick, so N² bytes/s) is
  the point.

## 4. Reference implementations

The shootout's servers date from 2016 (old libraries); TechEmpower's are tuned
far beyond what a reader can follow. **Write our own**, small and idiomatic,
and commit them with lockfiles:

| | HTTP | WebSocket / room |
|---|---|---|
| **Rust** | `hyper` 1.x on `tokio` (or `axum`, fork F3) | `tokio-tungstenite` |
| **Node** | built-in `node:http` | `ws` |
| **Bun** | `Bun.serve` | `Bun.serve` websockets (built-in) |

Each is one file per phase, the same behaviour as the spec, the language's
normal way — not micro-tuned. Optional later: `uWebSockets.js` as a "how fast
can JS go" ceiling.

## 5. The runner

- `make bench-servers` → `benchmarks/servers/run.sh [phase] [impl...]`.
  **Not** part of `make test`, `make test-examples` or any gate.
- Steps per implementation: build (Rae: `rae build --release`; Rust: `cargo
  build --release`; Node/Bun: none), start the server on a fixed port, wait
  for it, **run `check.sh` against it first** (a failing check skips the
  timing), warm up, run N timed runs, stop it, write `build/raw/<impl>.json`.
- `check.sh` validates every spec rule (status, body, required headers,
  pipelined order, echo/broadcast/broadcastResult shapes, tick rate). It is
  the only piece that could join the normal suite later — once Rae servers
  exist, as an opt-in gate (fork F7).
- Load generators: `oha` for HTTP (`cargo install`, JSON output with
  latency percentiles; fork F2 on pipelining) and our `loadClient` (Rust) for
  phases 2–3.
- **Fairness on macOS** (no cgroups; thread affinity is only a hint):
  - same machine, nothing else heavy running: the runner refuses to start
    when the 1-minute load average is above a threshold (overridable), and
    records it;
  - the server is run in two fixed configurations, **1 worker thread** and
    **all performance cores**, and every implementation is configured the same
    way (tokio worker threads, Node single process vs `cluster`, Bun single
    vs multiple processes, Rae worker count);
  - the load generator gets its own fixed thread count and runs on the same
    machine (documented as a limit: it competes for cores; a second machine or
    Linux `taskset` is the upgrade path);
  - 5 s warmup, then 5 runs of 15 s; report the median with min/max;
  - metrics: requests/s, latency p50/p99/p99.9, peak RSS (sampled with `ps`
    every 100 ms), CPU seconds; phase 3 adds tick duration, jitter, missed
    ticks;
  - `metadata.json` records machine, OS, toolchain and dependency versions,
    the Rae commit, and the load average.
- **Dependencies** (checked by `run.sh` and `fetch.sh`, each missing one named
  with its install command, then exit non-zero): Rust toolchain (installed
  here), `oha` (`cargo install oha`), Node (installed), Bun (installed); Go
  only if fork F4 picks `websocket-bench`. On this machine today: cargo, node
  and bun are present; oha, wrk, bombardier and go are not.

## 6. Rae capability gaps (proposed tickets)

Each is a separate queue task when approved; sizes are relative (the queue
runs every task as `{difficulty:4}` and splits big ones). None is a
benchmark-only hack: each is stdlib a real server needs.

| # | ticket | size | notes |
|---|---|---|---|
| G1 | `lib/net`: TCP listen/accept/connect/read/write/close on value handles (`SocketId`), non-blocking, `TCP_NODELAY`, `SO_REUSEADDR`; errors as values (`opt` / result enums), never `errno` | L | runtime C (`runtime_net.c`) behind a small handle API, the FileNotify pattern (fork F1) |
| G2 | `lib/net` poller: kqueue (macOS) / epoll (Linux) readiness — `wait(timeoutMs)` filling a caller-owned `List(Readiness)` (no allocation per wait) | M | the event loop each worker thread owns |
| G3 | a byte buffer for I/O: append, consume from the front, find a byte sequence, view a range as a `String` without copying, reuse without reallocating | M | read and write buffers per connection |
| G4 | time for servers: IMF-fixdate (`Date:` header, cached once a second), a monotonic sub-ms clock and a tick wait that sleeps until a deadline | S | `nowNs()` exists; `sleep(ms:)` is too coarse for a 30 Hz tick |
| G5 | SHA-1 and base64 (WebSocket handshake), in the runtime next to monocypher | S | |
| G6 | `lib/http` (HTTP/1.1 request parser, response writer, pipelining, keep-alive) and `lib/webSocket` (upgrade, frame codec, masking) in plain Rae on G1–G5 | L | the reusable server core; the benchmark servers are then thin |
| G7 | measure, then fix if needed: `Json` serialisation and `String` building cost per request (no ticket until phase 1 numbers show a problem) | — | listed so the result is read correctly |

## 7. Phasing

1. G1–G4 + G6 (HTTP part) → phase 1 servers (Rae, Rust, Node, Bun), checker,
   runner, first baseline.
2. G5 + G6 (WebSocket part) + `loadClient` → phase 2.
3. phase 3 on the same code: only the room logic and the tick are new.

## 8. Decisions for the maintainer

1. **F1 — sockets: runtime C module or bound libc?** (a) `runtime_net.c` with
   a handle API (like FileNotify; Rae never sees `errno` or `sockaddr`);
   (b) bind `<sys/socket.h>` + `<sys/event.h>` with the generated-bindings
   tool and write the rest in Rae. *Recommend (a):* the libc surface is
   macros, unions and `errno`, i.e. `unsafe` in every server.
2. **F2 — HTTP load tool:** (a) `oha` only (installs with cargo, JSON output,
   but no pipelining, so plaintext is measured without it); (b) `oha` plus
   `wrk` for the pipelined plaintext case (brew, Lua script). *Recommend (b)*
   if pipelining matters to you; (a) is simpler.
3. **F3 — Rust HTTP reference:** `hyper` directly (closest to raw speed) or
   `axum` (what people actually write). *Recommend hyper* for phase 1, since
   the comparison is about the server core.
4. **F4 — WebSocket load tool:** (a) our own `loadClient` in Rust, which phase 3
   needs anyway (jitter is measured client-side); (b) the shootout's Go
   `websocket-bench` for phase 2 (needs Go, 2016 code). *Recommend (a).*
5. **F5 — Rae server concurrency model:** (a) one event loop per core
   (readiness polling, non-blocking sockets; an acceptor hands sockets to
   workers over `Channel(Int)`); (b) a blocking thread per connection (much
   simpler, fine for phase 1, but cannot hold thousands of WebSocket clients).
   *Recommend (a)*, with (b) only if phase 1 must land before G2.
6. **F6 — commit results?** (a) like `benchmarks/list_access`: a dated
   `results/summary.json` + `metadata.json` baseline, regenerated by hand
   after relevant changes; (b) results stay local. *Recommend (a)* — it is the
   repository's existing practice, and a baseline makes regressions visible.
7. **F7 — checker in the normal suite?** After phase 1 lands, run `check.sh`
   against the Rae HTTP server only (seconds, no load) as one pre-suite case;
   or keep everything under `make bench-servers`. *Recommend adding it* once
   it exists: it guards `lib/net`/`lib/http` correctness, not speed.
8. **F8 — Linux numbers:** is a Linux machine (epoll, `taskset`) in scope
   later, or is macOS the only target for now? It decides whether G2 does
   epoll in the first pass.
