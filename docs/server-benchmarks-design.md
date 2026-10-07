# Server benchmarks: design

**Status:** design, awaiting the maintainer's decisions (§10). Nothing is
implemented. Written 2026-10-08; revised the same day: every Rae server is
written in TWO styles, a plain event loop and ECS, to measure what the
architecture costs (maintainer decision, §5).

A small suite that measures Rae servers against the same servers in Rust and
JavaScript, before real web backends and game servers are written in Rae.
Three phases: HTTP, WebSocket, and a game-style room. It also answers a
second question: Rae uses ECS as its general architecture (UI, renderer,
physics); does it hold up, and at what cost, for a server?

## 1. What Rae can do today (the honest starting point)

| capability | today | consequence |
|---|---|---|
| TCP sockets | **none**: no `socket`/`listen`/`accept` anywhere in `lib/` or the runtime | phase 1 cannot start until a `net` module exists (gap G1) |
| readiness polling (kqueue / epoll) | the runtime uses kqueue/inotify for one thing, `lib/FileNotify` (runtime C behind a handle API) | that file is the model for a socket poller (G2); there is no general event loop over file descriptors |
| threads | `spawn f(...)` → `Task(T)` on real OS threads; `Channel(T)` (MPSC, non-blocking, any value-type payload since #969); `Parallel.workerCount()` | enough to run one event loop (or one World) per core and hand accepted sockets (plain value handles) to workers over a channel |
| ECS | `lib/ecs`: `EntityAllocator` (`allocEntity` / `freeEntity`), `ComponentTable(T)` (sparse set, O(1) lookup), query loops over one to three tables, zero-field tag components, `EventQueue(T)`, an ordered `Schedule` | everything the ECS-style servers need already exists; only the server components and systems are new (G8) |
| bytes | `List(UInt8)`, `Buffer(T)`, `String` is byte-indexed (`byteAt`, `sub`, `indexOf`, `startsWith`, `split`) | an HTTP parser can be written in plain Rae; a reusable read/write byte buffer that does not allocate per request is missing (G3) |
| JSON | `lib/Json` (a parsed document + value builders) | enough for `{"message":"Hello, World!"}` and the shootout messages; serialisation speed is part of what phase 1 measures |
| time | `nowNs()`, `sleep(ms:)`, `lib/Time` (log formatting) | an HTTP `Date` header (IMF-fixdate) and a sub-millisecond tick wait are missing (G4) |
| hashing | monocypher (argon2, blake2b) in the runtime | no SHA-1 or base64, which the WebSocket handshake needs (G5) |
| FFI | `unsafe extern("c_symbol")`, `cheader`, generated bindings (Box3D track B, `lib/webgpu`) | the libc socket API could be bound, but see fork F1: its macros, `errno`, `sockaddr` unions and variadic `fcntl` would leak unsafe C shapes into every server |

**The `view`/`mod`-not-in-structs rule shapes connection state** in both
styles:

- a socket is a **value handle** (`SocketId`, an `Int` the runtime maps to the
  file descriptor), stored freely, the way GPU resources are ids;
- a connection's state (its `SocketId`, read and write buffers, parse state,
  room membership) is an owned **value** with one owner: the event loop's
  connection list, or the World's component tables;
- code borrows it for one call (`mod Connection`, or a query's `mod` binding)
  and returns; nothing outlives the call by reference;
- each worker thread owns its connections outright; threads exchange only
  socket handles over a channel, never shared state.

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
  run.sh                                 A  the runner (§8); `make bench-servers` calls it
  check.sh                               A  the correctness checker (§8)
  rae/eventLoop/http/Main.rae            A  phase 1, plain event-loop style (§6)
  rae/eventLoop/webSocket/Main.rae       A  phase 2
  rae/eventLoop/gameRoom/Main.rae        A  phase 3
  rae/ecs/http/Main.rae                  A  phase 1, ECS style (§7)
  rae/ecs/webSocket/Main.rae             A  phase 2
  rae/ecs/gameRoom/Main.rae              A  phase 3
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
  script can regenerate: the specs (so external drift cannot change what we
  measure), both styles of Rae server, our Rust/JS references with their
  lockfiles, the runner, fetcher, checker, and the manifest of external pins.
- **B, fetched:** third-party code we only run: the load generators, and
  (read-only, for comparison while writing our references) the shootout and
  TechEmpower repositories. Pinned by SHA in `external.lock`, cloned by
  `fetch.sh` into the cache. Cargo crates and npm packages are also B: the
  lockfiles (A) pin them, the toolchains fetch them.
- **C, generated:** `build/` (binaries, generated C), raw per-run results
  (`build/raw/*.json`), charts. Never committed, except the summary in
  `results/` if fork F6 says so.
- Nothing external is copied into the repository, so no third-party licence
  is carried. If that changes, the copied file gets the one-line notice the
  repository's per-file copyright rule requires.
- Reusable Rae code does not live under `benchmarks/`. The stdlib modules are:
  - `lib/net/` (sockets, poller, byte buffer);
  - `lib/http/` and `lib/webSocket/` (the protocol codecs, as plain functions
    over bytes);
  - `lib/net/ecs/` (the server components and systems, the way
    `lib/physics/ecs/` wraps the physics engine).

  Both styles call the SAME codecs and sockets, so the benchmark servers stay a
  page or two each and the only difference between the styles is the
  architecture (§5).

## 3. The benchmarks (our specs, short)

Each spec is a page in `spec/`; the essentials:

**Phase 1: HTTP/1.1** (TechEmpower test types 1 and 6, their rules):

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

**Phase 2: WebSocket** (the websocket-shootout protocol):

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

**Phase 3: game room** (ours; no external standard exists):

- N clients join one room over the phase 2 WebSocket; each sends a 32-byte
  input message at 30 Hz.
- The server runs a fixed **30 Hz tick**: apply inputs, then send every
  client a state snapshot (16 bytes per player, so the snapshot grows with N).
- Measured at N = 100, 1 000, 5 000:
  - tick duration (server-side work per tick);
  - **tick jitter** (actual tick start minus scheduled start, p50/p99/max);
  - client-observed snapshot inter-arrival jitter;
  - missed ticks, CPU and RSS.

  The fan-out cost is the point: N snapshots, each of size ∝ N, per tick, so N²
  bytes per second.

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
normal way, not micro-tuned. Optional later: `uWebSockets.js` as a "how fast
can JS go" ceiling.

## 5. Two Rae styles, one experiment

Every Rae server exists twice: **eventLoop** (§6) and **ecs** (§7). The
question is whether ECS, Rae's general architecture, costs anything
measurable when the program is a server, and what it buys.

**What is held equal, so that only the architecture differs:**

- the same `lib/net` sockets and poller, the same byte buffers, the same
  `lib/http` / `lib/webSocket` codecs (parse and encode are plain functions
  over bytes, called identically by both);
- the same thread model (one loop or one World per worker, the acceptor
  handing sockets out over a channel) and the same worker counts;
- the same compiler, profile (`--release`) and runner, and both run in the
  same session as the references.

**What differs:**

- where the state lives: a `List(Connection)` of structs vs component tables;
- how work is dispatched: per-event function calls vs systems that run over
  every entity with the right components, in a fixed order, once per poll.

**Reported per benchmark:**

- both styles side by side, and the ECS/eventLoop ratio for every metric
  (requests/s, p50/p99, RSS, tick duration and jitter);
- allocations per request or per tick (from `RAE_MEM_STATS`);
- the source size of each server, excluding the shared library code.

A difference under the run-to-run noise (the min-max spread of the five runs)
is reported as "no measurable difference", not as a ranking.

## 6. Style 1: the plain event loop

The shape most servers in most languages have, written in Rae: one loop per
worker owns a list of connections and calls a handler per event.

```rae
type Connection {
  socketId: SocketId
  input: ByteBuffer
  output: ByteBuffer
  parse: HttpParseState
}

func serveForever(listener: view Listener, poller: mod Poller) {
  var connections: List(Connection) = List.create(Connection, cap: 1024)
  var ready: List(Readiness) = List.create(Readiness, cap: 256)
  loop {
    pollWait(poller: poller, ready: ready, timeoutMs: 100)
    # for each ready socket: accept, or read + parse + respond + write
    ...
  }
}
```

(`SocketId`, `ByteBuffer`, `Listener`, `Poller`, `Readiness` and
`HttpParseState` are the proposed G1–G3 and G6 types; the sketch shows the
shape, not a final API.) A handler takes `connection: mod Connection` for one
event: read into `input`, parse every complete request, append each response
to `output`, write as much as the socket takes. A socket handle maps to its
list slot through an `IntMap`.

## 7. Style 2: ECS

A server is a loop that turns events into state changes, which is a game
loop's shape. The ECS server is a World per worker, with connections as
entities and the request pipeline as systems run once per poll.

**Entities**

- **connection**: one per accepted socket, destroyed on close.
- **request** (phase 1): fork F9 decides whether a request is its own entity
  or components on its connection entity. The recommendation is components on
  the connection, which avoids an entity per request.
- **room** and **player** (phase 3): a player is the connection entity plus
  game components.

**Components** (all values; buffers are owned `List(UInt8)`-backed structs):

| component | meaning | phases |
|---|---|---|
| `Socket {socketId}` | the OS handle | all |
| `Readable`, `Writable` | **tags**, set by the poll system for this iteration only | all |
| `ReadBuffer`, `WriteBuffer` | bytes received / still to send | all |
| `HttpParse` | incremental parser state (partial request line, headers) | 1, 2 (upgrade) |
| `PendingRequests` | the parsed requests in arrival order (pipelining keeps replies in order) | 1 |
| `KeepAlive {idleSinceNs}` | idle timeout bookkeeping | 1, 2 |
| `Closing` | tag: flush, then destroy | all |
| `WebSocketSession` | handshake done; frame decoder state | 2, 3 |
| `IncomingMessages` | decoded messages of this iteration | 2, 3 |
| `RoomMember {roomId}`, `PlayerInput`, `PlayerState` | game state | 3 |

**Resources** (fields of the World, not entities): the listener, the poller
and its readiness list, the socket-to-entity `IntMap`, the cached `Date`
header, the tick clock and the room registry.

**Systems, in this order, once per poll (one "frame"):**

1. **pollSystem**: wait for readiness and set the `Readable` / `Writable`
   tags. Every later system queries a tag table, so the work per frame is
   proportional to the *active* connections, not to all of them (AGENTS.md's O(n)
   rule; idle WebSocket clients cost nothing).
2. **acceptSystem**: accept pending sockets; allocate an entity with `Socket`,
   `ReadBuffer`, `WriteBuffer`, `HttpParse`, `KeepAlive`.
3. **readSystem** (`Readable`): read into `ReadBuffer`; on end-of-stream, tag
   `Closing`.
4. **httpParseSystem** (`ReadBuffer` + `HttpParse`): parse every complete
   request into `PendingRequests`. An `Upgrade: websocket` request answers the
   handshake and swaps components: it removes `HttpParse` and
   `PendingRequests` and adds `WebSocketSession`. The connection's protocol
   state machine is simply which components it has.
5. **route systems**, one per route (`plaintextSystem`, `jsonSystem`,
   `notFoundSystem`): each takes the requests for its path from
   `PendingRequests` and appends the encoded response to `WriteBuffer`, in
   request order.
6. phase 2: **frameDecodeSystem** (`WebSocketSession` + `ReadBuffer` →
   `IncomingMessages`), **echoSystem**, **broadcastSystem** (a query over
   every `WebSocketSession` writes into every `WriteBuffer`).
7. phase 3: **inputSystem** (messages → `PlayerInput`), then at the 30 Hz
   deadline **tickSystem** (inputs → `PlayerState`) and **snapshotSystem**
   (one snapshot per tick, encoded once, appended to every `RoomMember`'s
   `WriteBuffer`).
8. **writeSystem** (`WriteBuffer` non-empty): write what the socket takes;
   keep the rest (and register for `Writable`).
9. **timeoutSystem** and **closeSystem**: tag idle connections `Closing`;
   destroy `Closing` entities whose `WriteBuffer` has drained, freeing the
   socket and the entity.

A system takes the tables it touches as parameters, as `lib/water/Buoyancy`
does, so each one says in its signature what it reads and writes:

```rae
func readSystem(
  sockets: view ComponentTable(Socket)
  readables: view ComponentTable(Readable)
  readBuffers: mod ComponentTable(ReadBuffer)
  closings: mod ComponentTable(Closing)
) {
  loop let entityId: EntityId, readable: view Readable, input: mod ReadBuffer in query2(
    tableA: readables
    tableB: readBuffers
  ) {
    ...
  }
}
```

(The sketch shows the shape: the query-loop and table syntax are today's
`lib/ecs`; `Readable` and `ReadBuffer` are the proposed G8 components.)

**What ECS buys a server:**

- **No callbacks and no `async`.** "Waiting" is a component: a request that
  needs a slow resource gets `AwaitingResult`, a worker sends the answer back
  over a channel, and a later system completes it. Rae has neither callbacks
  nor `async`, so this is the natural way to write it.
- **Batching.** Every ready socket of one poll goes through each stage
  together, which is the "parse everything, answer in one write" behaviour
  that pipelining rewards.
- **Ownership fits.** The World owns everything, and systems borrow for one
  call. That is the `view`/`mod`-not-in-structs rule exactly, with no
  connection objects holding references.
- **Testable without a network.** Put bytes in a `ReadBuffer`, run the parse
  and route systems, check the `WriteBuffer`.
- **Observability.** The count of entities with `PendingRequests` is a
  metric, and the same tooling as the UI and game worlds applies.

**What it costs, or might (the measurement decides):**

- **Iteration.** Every frame runs every system. Systems over empty tag tables
  are cheap but not free.
- **Component churn.** Adding and removing tags and requests each frame costs
  table updates where the event loop only changes a field.
- **Indirection.** One connection's data is spread over several tables
  instead of one struct.
- **Ordering** is bookkeeping, not free: replies must leave in request order,
  hence `PendingRequests` as an ordered list per connection.
- **One slow system stalls the frame.** That is the same rule as any event
  loop: blocking work becomes "start, a component, finish later", never a
  wait inside a system.

## 8. The runner

- `make bench-servers` → `benchmarks/servers/run.sh [phase] [impl...]`, where
  `impl` is one of `rae-eventLoop`, `rae-ecs`, `rust`, `node`, `bun`. It is
  **not** part of `make test`, `make test-examples` or any gate.
- Steps per implementation:
  1. build (Rae: `rae build --release`; Rust: `cargo build --release`;
     Node/Bun: none);
  2. start the server on a fixed port and wait for it;
  3. **run `check.sh` against it first** (a failing check skips the timing);
  4. warm up, run N timed runs, stop the server;
  5. write `build/raw/<impl>.json`.
- `check.sh` validates every spec rule: status, body, required headers,
  pipelined order, the echo / broadcast / broadcastResult shapes, the tick
  rate. Both Rae styles must pass the same checks. It is the only piece that
  could join the normal suite later (fork F7).
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
    vs multiple processes, the Rae worker count in both styles);
  - the load generator gets its own fixed thread count and runs on the same
    machine (documented as a limit: it competes for cores; a second machine or
    Linux `taskset` is the upgrade path);
  - the two Rae styles run back to back, alternating their order between
    runs, so slow drift on the machine does not favour one of them;
  - 5 s warmup, then 5 runs of 15 s; report the median with min/max;
  - metrics: requests/s, latency p50/p99/p99.9, peak RSS (sampled with `ps`
    every 100 ms), CPU seconds; phase 3 adds tick duration, jitter, missed
    ticks; the Rae styles add allocations per request/tick;
  - `metadata.json` records machine, OS, toolchain and dependency versions,
    the Rae commit, and the load average.
- **Dependencies:** `run.sh` and `fetch.sh` check for them, name each missing
  one with its install command, and exit non-zero.
  - Needed: the Rust toolchain, `oha` (`cargo install oha`), Node and Bun; Go
    only if fork F4 picks `websocket-bench`.
  - On this machine today, cargo, node and bun are present; oha, wrk,
    bombardier and go are not.

## 9. Rae capability gaps (proposed tickets) and phasing

Each is a separate queue task when approved; sizes are relative (the queue
runs every task as `{difficulty:4}` and splits big ones). None is a
benchmark-only hack: each is stdlib a real server needs.

| # | ticket | size | notes |
|---|---|---|---|
| G1 | `lib/net`: TCP listen/accept/connect/read/write/close on value handles (`SocketId`), non-blocking, `TCP_NODELAY`, `SO_REUSEADDR`; errors as values (`opt` / result enums), never `errno` | L | runtime C (`runtime_net.c`) behind a small handle API, the FileNotify pattern (fork F1) |
| G2 | `lib/net` poller: kqueue (macOS) / epoll (Linux) readiness, `wait(timeoutMs)` filling a caller-owned `List(Readiness)` (no allocation per wait) | M | used by both styles |
| G3 | a byte buffer for I/O: append, consume from the front, find a byte sequence, view a range as a `String` without copying, reuse without reallocating | M | read and write buffers per connection |
| G4 | time for servers: IMF-fixdate (`Date:` header, cached once a second), a monotonic sub-ms clock and a tick wait that sleeps until a deadline | S | `nowNs()` exists; `sleep(ms:)` is too coarse for a 30 Hz tick |
| G5 | SHA-1 and base64 (WebSocket handshake), in the runtime next to monocypher | S | |
| G6 | `lib/http` (HTTP/1.1 request parser, response encoder, pipelining, keep-alive) and `lib/webSocket` (upgrade, frame codec, masking) in plain Rae on G1–G5, as functions over bytes with no I/O and no architecture | L | shared by both styles |
| G8 | `lib/net/ecs`: the server components (§7 table) and systems (accept, read, write, timeout, close, frame decode) with the readiness tags, on `lib/ecs` | M | the reusable ECS server core; the benchmark's ECS servers add only route / room systems |
| G7 | measure, then fix if needed: `Json` serialisation and `String` building cost per request (no ticket until phase 1 numbers show a problem) | — | listed so the result is read correctly |

**Phasing** (each phase lands both Rae styles together):

1. G1–G4, G6 (HTTP part), G8 (HTTP systems) → phase 1 servers (Rae
   eventLoop, Rae ECS, Rust, Node, Bun), checker, runner, first baseline.
2. G5, G6 (WebSocket part), G8 (frame systems), `loadClient` → phase 2.
3. Phase 3 on the same code: only the room logic, the tick and the snapshot
   system are new.

## 10. Decisions for the maintainer

1. **F1: sockets as a runtime C module, or bound libc?**
   - (a) `runtime_net.c` with a handle API, like FileNotify; Rae never sees
     `errno` or `sockaddr`.
   - (b) Bind `<sys/socket.h>` and `<sys/event.h>` with the generated-bindings
     tool and write the rest in Rae.

   *Recommend (a):* the libc surface is macros, unions and `errno`, i.e.
   `unsafe` in every server.
2. **F2: the HTTP load tool.**
   - (a) `oha` only: it installs with cargo and has JSON output, but no
     pipelining, so plaintext is measured without it.
   - (b) `oha` plus `wrk` for the pipelined plaintext case (brew, Lua
     script).

   *Recommend (b)* if pipelining matters to you; (a) is simpler.
3. **F3: the Rust HTTP reference.** `hyper` directly (closest to raw speed)
   or `axum` (what people actually write). *Recommend hyper* for phase 1,
   since the comparison is about the server core.
4. **F4: the WebSocket load tool.**
   - (a) Our own `loadClient` in Rust, which phase 3 needs anyway (jitter is
     measured client-side).
   - (b) The shootout's Go `websocket-bench` for phase 2 (needs Go, 2016
     code).

   *Recommend (a).*
5. **F5: the concurrency model of both Rae styles.**
   - (a) One event loop or World per core: readiness polling, non-blocking
     sockets, and an acceptor handing sockets to workers over a channel.
   - (b) A blocking thread per connection: much simpler and fine for phase 1,
     but it cannot hold thousands of WebSocket clients, and it has no frame
     for ECS systems to run in.

   *Recommend (a).*
6. **F6: commit results?**
   - (a) Like `benchmarks/list_access`: a dated `results/summary.json` and
     `metadata.json` baseline, regenerated by hand after relevant changes.
   - (b) Results stay local.

   *Recommend (a):* it is the repository's existing practice, and a baseline
   makes regressions visible.
7. **F7: the checker in the normal suite?**
   - After phase 1 lands, run `check.sh` against both Rae HTTP servers (seconds,
     no load) as one pre-suite case.
   - Or keep everything under `make bench-servers`.

   *Recommend adding it* once it exists: it guards `lib/net` / `lib/http` /
   `lib/net/ecs` correctness, not speed.
8. **F8: Linux numbers.** Is a Linux machine (epoll, `taskset`) in scope
   later, or is macOS the only target for now? It decides whether G2 does
   epoll in the first pass.
9. **F9: ECS requests as entities, or components on the connection?**
   - (a) Components on the connection entity (`PendingRequests`, an ordered
     list), so there is no entity per request.
   - (b) An entity per request, linked to its connection: purer, but it costs
     an entity allocate/free per request.

   *Recommend (a).* (b) could be a third measured variant if the first results
   make it interesting.
10. **F10: how ECS-pure the ECS servers are.**
    - (a) Use the `lib/ecs` `Schedule` (dirty-skipping) to run the systems.
    - (b) Call the systems in a fixed order from the loop, as the physics and
      vehicle apps do.

    *Recommend (b)* for the benchmark: the readiness tags already skip idle
    work, and a schedule's bookkeeping would be measured as ECS cost when it
    is a separate feature.
