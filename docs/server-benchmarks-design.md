# Server benchmarks: design

**Status:** design DECIDED (§10), implementation queued. Written
2026-10-08, revised the same day. Every Rae server is written in TWO styles, a
plain event loop and ECS, to measure what the architecture costs (maintainer
decision, §5); in the ECS style connections are entities and requests are
events (§7). **Rae first:** the server stack is Rae code over a thin,
syscall-shaped C shim; C holds only what the platform forces (§1.2,
maintainer decision). There is no `async`/`await`; slow work is `spawn`ed
(§1.3).

A small suite that measures Rae servers against the same servers in Rust and
JavaScript, before real web backends and game servers are written in Rae.
Three phases: HTTP, WebSocket, and a game-style room. It also answers a
second question: Rae uses ECS as its general architecture (UI, renderer,
physics); does it hold up, and at what cost, for a server?

## 1. What Rae can do today (the honest starting point)

| capability | today | consequence |
|---|---|---|
| TCP sockets | **none**: no `socket`/`listen`/`accept` anywhere in `lib/` or the runtime | phase 1 cannot start until a `net` module exists (gap G1) |
| readiness polling (kqueue / epoll) | `lib/FileNotify` is Rae over one-call kqueue shims too (runtime audit row 10) | the poller (G2, landed) is a kqueue shim plus Rae bookkeeping (§1.2), `lib/net/Poller.rae`; epoll is not there yet (F8) |
| threads | `spawn f(...)` → `Task(T)` on real OS threads; `Channel(T)` (MPSC, non-blocking, any value-type payload since #969); `Parallel.workerCount()` | enough to run one event loop (or one World) per core and hand accepted sockets (plain value handles) to workers over a channel; a `Task` can be asked "done?" without joining (`isDone` / `tryGet`, gap T1, done 2026-10-08) |
| ECS | `lib/ecs`: `EntityAllocator` (`allocEntity` / `freeEntity`), `ComponentTable(T)` (sparse set, O(1) lookup), query loops over one to three tables, zero-field tag components, `EventQueue(T)`, an ordered `Schedule` | everything the ECS-style servers need already exists; only the server components and systems are new (G8) |
| bytes | `List(UInt8)`, `Buffer(T)`, `String` is byte-indexed (`byteAt`, `sub`, `indexOf`, `startsWith`, `split`) | an HTTP parser can be written in plain Rae; the reusable read/write byte buffer is `lib/net/ByteBuffer.rae` (G3, landed) |
| JSON | `lib/Json` (a parsed document + value builders) | enough for `{"message":"Hello, World!"}` and the shootout messages; serialisation speed is part of what phase 1 measures |
| time | `nowNs()`, `sleep(ms:)`, `lib/Time` (log formatting) | an HTTP `Date` header (IMF-fixdate, plain Rae arithmetic) and a deadline wait for a 30 Hz tick are missing (G4) |
| hashing | monocypher (argon2, blake2b) in the runtime | SHA-1 and base64 for the WebSocket handshake are plain Rae: `lib/crypto/Sha1.rae`, `lib/text/Base64.rae` (G5, landed) |
| FFI | `unsafe extern("c_symbol")`, `cheader`, generated bindings (Box3D track B, `lib/webgpu`) | Rae can call C directly, but four things in the socket API cannot be called safely or portably from Rae (§1.2); those go in a thin shim, and nothing else |

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

### 1.2 Rae first: C only for what the platform forces

Rae is dogfooded: code is written in Rae whenever it can be, and C is used
only where the platform ABI leaves no choice (the rule AGENTS.md states for
the whole runtime, following the renderer's C-surface gate). For sockets,
exactly four things force C:

- **`errno`** is a macro over a hidden per-platform function (`__error()` on
  macOS, `__errno_location()` on Linux) returning a pointer to thread-local
  storage;
- **`fcntl`** is variadic, and calling a variadic C function through a plain
  extern is undefined behaviour on Apple Silicon (variadic arguments travel on
  the stack);
- **address structs** (`sockaddr_in`/`sockaddr_in6`, the `addrinfo` list
  from `getaddrinfo`) have per-OS layouts (BSD has a length byte Linux lacks);
- **constants** (`O_NONBLOCK`, `SOL_SOCKET`, `SO_REUSEADDR`, `TCP_NODELAY`,
  `EAGAIN`, …) are macros with different values on macOS and Linux.

So the C side is a **syscall-shaped shim**:

- each function makes ONE system call and returns its result, or the
  negated `errno` value;
- address resolution answers a plain value, and the platform's constants
  are exported as a table;
- it holds no loops, no retry policy, no option choices and no error
  mapping.

Everything else is Rae in `lib/net`:

- the listen sequence (socket, reuse option, bind, listen, non-blocking);
- retrying an interrupted call;
- connect with a timeout (start, wait for writable, read the socket error);
- which options a server sets (no Nagle delay, no SIGPIPE);
- turning error numbers into `NetStatus`;
- the accept-abort rule;
- the poller's readiness bookkeeping.

The first version of G1 (0.1.202) put that logic in `runtime_net.c`; T2
moved it to Rae behind the same API and fixture (2026-10-08). The bindings
and the constant indices live in `lib/net/NetSystem.rae`, and the policy in
`lib/net/Tcp.rae`. The C platform section went from 193 code lines with 6
loops and 37 branches to 149 lines with 1 loop (walking `getaddrinfo`'s own
list) and 7 branches. About 45 of those lines are the constant table.

Pure algorithms are written in Rae: SHA-1, base64, the IMF-fixdate `Date`
header and the HTTP/WebSocket codecs. Where a platform has a clearly faster
native version, that is used on that platform with the Rae code as the
fallback: SHA-1 calls CommonCrypto on Apple (7 ms for 16 MB, against 100 ms
in Rae; AGENTS.md).

### 1.3 No `async`/`await`: `spawn` is the marker

`docs/concurrency-model.md` §1 already decides this: normal calls are
synchronous and waited, `spawn` is the only marker for concurrent work,
synchronisation is an explicit `task.get()`, and there is no `await` keyword
and no function colouring. A server needs nothing more:

- network I/O never blocks: the poller says which sockets are ready, and the
  loop (or the ECS systems) handles exactly those;
- slow work (a database query, a file, a heavy computation) is `spawn`ed, and
  its result is picked up on a later frame. In the ECS style that is the
  `AwaitingResult` component (§7).

Picking it up without blocking needs one addition (gap T1): a `Task` that
can be asked whether it has finished (`isDone`), and a non-blocking
`tryGet() ret opt T`. Today only a `Channel` can be polled, and only
`task.get()` reads a Task, which joins.

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
  common.sh  measure.py  pipeline.lua    A  (as built) shared build/start helpers, the measuring
                                            half of run.sh, wrk's 16-deep pipelining script
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
                                         A  our WebSocket + game-room load/measure client (F4)
  results/  summary.json  metadata.json  A  the latest dated baseline (F6)
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
  `results/` (F6).
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
| **Rust** | `hyper` 1.x on `tokio` (F3) | `tokio-tungstenite` |
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

- both styles side by side (two Rae implementations of every phase, always),
  and the ECS/eventLoop ratio for every metric (requests/s, p50/p99, RSS,
  tick duration and jitter);
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
loop's shape. The ECS server is a World per worker, run once per poll (one
"frame"), built from the three kinds of thing Rae's UI and game worlds
already use:

- **entities** for things that LIVE across frames, have state that changes,
  and are referred to later (widgets, bodies; here: connections);
- **events** for things that HAPPEN: produced and consumed within one frame,
  in an `EventQueue(T)` resource (`lib/ui` keeps `uiActions:
  EventQueue(UiAction)`, the physics world keeps `contactBegan`, `hits`, …);
- **resources** for singletons (fields of the World).

The test for entity versus event is **lifetime, not count**. A request is
parsed, routed, answered and encoded in the frame its bytes arrived, so it is
an event. 4 096 requests in one frame (256 connections × 16 pipelined) are a
4 096-element list. An `EventQueue` is a dense list walked front to back, the
cache-friendly part of ECS, with no sparse lookup and no create/destroy per
item: the queue's double buffer, swapped at the end of each frame, is the
pool.

**Entities**

- **connection**: one per accepted socket, created on accept, destroyed on
  close. It lives for seconds to minutes and is referred to across frames.
- **room** (phase 3): one entity; a **player** is its connection entity plus
  game components.

**Events** (`EventQueue(T)` resources, one per type, cleared each frame):

| event | carries | produced by → consumed by |
|---|---|---|
| `HttpRequestEvent` | connection `EntityId`, sequence number, the parser's `HttpRequest` (method enum, path and header byte ranges into the connection's `ReadBuffer`), a bad-request flag | httpParseSystem → dispatchSystem |
| `WebSocketMessage` | connection `EntityId`, message kind, payload byte range | frameDecodeSystem → message systems |
| `PlayerInputEvent` | connection `EntityId`, the decoded 32-byte input | inputSystem → tickSystem |

Events carry **no heap of their own**: byte ranges point into the
connection's `ReadBuffer`, which stays untouched until the frame's events are
consumed. Producing one copies a few plain fields into a list that already
has capacity: zero allocations per request after warmup, and the report
checks it (`RAE_MEM_STATS`).

**When a request outlives its frame** (it waits on a database or an upstream
service; not in this benchmark, but every real backend has it), it stops
being an event. The dispatch system *promotes* it to state on its connection:
an `AwaitingResult` component holding the request's sequence number and
ranges, whose `ReadBuffer` bytes are kept, plus the `Task` of the `spawn`ed
slow work (§1.3). A later system polls the task with `tryGet` (T1), completes
the request and encodes the response. Only the long-lived requests pay for
table storage.

**Components** on connection entities (all values; buffers are owned
`List(UInt8)`-backed structs):

| component | meaning | phases |
|---|---|---|
| `Socket {socketId}` | the OS handle | all |
| `Readable`, `Writable` | **tags**, set by the poll system for this frame only | all |
| `Sending` | tag: the `WriteBuffer` holds unsent bytes (the write system walks this, not every connection) | all |
| `ReadBuffer`, `WriteBuffer` | bytes received / still to send | all |
| `HttpParse` | incremental parser state (a request split across reads) | 1, 2 (upgrade) |
| `KeepAlive {idleSinceNs}` | idle timeout bookkeeping | 1, 2 |
| `AwaitingResult` | a request promoted out of its frame (above) | real servers |
| `Closing` | tag: flush, then destroy | all |
| `WebSocketSession` | handshake done; frame decoder state (a frame split across reads) | 2, 3 |
| `RoomMember {roomId}`, `PlayerState` | game state | 3 |

**Resources** (fields of the World): the listener, the poller and its
readiness list, the socket-to-entity `IntMap`, the event queues, the cached
`Date` header, the tick clock, the room registry, and a buffer pool for the
rare per-request heap that does outlive a frame (G8).

**Systems, in this order, once per frame:**

1. **pollSystem**: wait for readiness and set the `Readable` / `Writable`
   tags. Every later system queries a tag table, so the work per frame is
   proportional to the *active* connections, not to all of them (AGENTS.md's
   O(n) rule; idle WebSocket clients cost nothing).
2. **acceptSystem**: accept pending sockets; allocate an entity with `Socket`,
   `ReadBuffer`, `WriteBuffer`, `HttpParse`, `KeepAlive`.
3. **readSystem** (`Readable`): read into `ReadBuffer`; on end-of-stream, tag
   `Closing`. Then **awaitingResultSystem**: answer each `AwaitingResult`
   whose task `tryGet` hands a reply, drop the bytes up to that request and
   tag the connection `Readable`, so the requests pipelined behind it are
   parsed this frame. Until then the connection parses nothing further, which
   keeps its answers in request order.
4. **httpParseSystem** (`ReadBuffer` + `HttpParse`): send an `HttpRequest`
   event for every complete request, in arrival order. An `Upgrade:
   websocket` request answers the handshake and swaps components: it removes
   `HttpParse` and adds `WebSocketSession`. The connection's protocol state
   machine is simply which components it has.
5. **dispatchSystem**: walks the `HttpRequest` events in order and calls the
   route's handler for each (`plaintextResponse`, `jsonResponse`,
   `notFoundResponse`). The handlers are plain functions, not systems, which
   append the encoded response to the connection's `WriteBuffer`. In
   `lib/net/ecs` the app's routes are a router type `R` with a
   `route(router: mod R, server:, event:)` function; the generic
   `dispatchSystem(R, ...)` calls it for each event, and it answers with
   `respond` (now) or `promote` (later). One system
   in event order is what keeps a mixed pipeline (`/json` then `/plaintext`
   on one connection) answered in request order: a system per route would
   answer in system order instead.
6. phase 2: **frameDecodeSystem** (`WebSocketSession` + `ReadBuffer` →
   `WebSocketMessage` events), then **messageSystem** (echo: append to the
   sender's `WriteBuffer`; broadcast: encode once, append to every
   `WebSocketSession`'s `WriteBuffer`, then the `broadcastResult`).
7. phase 3: **inputSystem** (messages → `PlayerInputEvent`), then at the
   30 Hz deadline **tickSystem** (input events → `PlayerState`) and
   **snapshotSystem** (one snapshot per tick, encoded once, appended to every
   `RoomMember`'s `WriteBuffer`).
8. **writeSystem** (`WriteBuffer` non-empty): write what the socket takes;
   keep the rest (and register for `Writable`).
9. **timeoutSystem** and **closeSystem**: tag idle connections `Closing`;
   destroy `Closing` entities whose `WriteBuffer` has drained, freeing the
   socket and the entity.
10. **end of frame**: consume the read bytes the events pointed into, and
    `frameAdvance` every event queue.

A system takes the tables and queues it touches as parameters, as
`lib/water/Buoyancy` does, so each one says in its signature what it reads
and writes:

```rae
func dispatchSystem(
  requests: view EventQueue(HttpRequest)
  readBuffers: view ComponentTable(ReadBuffer)
  writeBuffers: mod ComponentTable(WriteBuffer)
  dateHeader: view String
) {
  loop var i: Int = 0, i < eventCount(HttpRequest, this: requests), ++i {
    let request: HttpRequest = eventAt(HttpRequest, this: requests, i: i)
    ...
  }
}
```

(The sketch shows the shape. The landed systems, `lib/net/ecs/HttpSystems.rae`,
take the whole `HttpServer` where a system touches most of it, and the tables
where it touches a few, as `httpParseSystem` does.)

**What ECS buys a server:**

- **No callbacks and no `async`.** "Waiting" is a component (`AwaitingResult`)
  that a later system completes. Rae has neither callbacks nor `async`, so
  this is the natural way to write it.
- **Batching.** Every ready socket of one poll goes through each stage
  together, which is the "parse everything, answer in one write" behaviour
  that pipelining rewards.
- **Ownership fits.** The World owns everything, and systems borrow for one
  call. That is the `view`/`mod`-not-in-structs rule exactly, with no
  connection objects holding references.
- **Testable without a network.** Put bytes in a `ReadBuffer`, run the parse
  and dispatch systems, check the `WriteBuffer`.
- **Observability.** Requests per frame is an event count, open connections
  an entity count, and the same tooling as the UI and game worlds applies.

**What it costs, or might (the measurement decides):**

- **Iteration.** Every frame runs every system. Systems over empty tag tables
  and queues are cheap but not free.
- **Tag churn.** Setting and clearing the readiness tags each frame costs
  table updates where the event loop just handles the event.
- **Indirection.** One connection's data is spread over several tables
  instead of one struct.
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
  joins the normal suite once it exists (F7).
- Load generators: `oha` for HTTP (`cargo install`, JSON output with
  latency percentiles), `wrk` with a committed Lua script for the pipelined
  plaintext case only (F2), and our `loadClient` (Rust) for phases 2–3.
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
  - Needed: the Rust toolchain, `oha` (`cargo install oha`), `wrk` (`brew
    install wrk`, for pipelined plaintext only), Node and Bun. Go is not
    needed (F4).
  - On this machine, cargo, node and bun are present; `fetch.sh` builds oha
    and wrk into the cache (they are not installed system-wide).

## 9. Rae capability gaps (tickets) and phasing

Each is a queue task; sizes are relative (the queue runs every task as
`{difficulty:4}` and splits big ones). None is a benchmark-only hack: each is
stdlib a real server needs. Every one follows §1.2: Rae code, with C only in
a syscall-shaped shim where the platform forces it.

| # | ticket | size | notes |
|---|---|---|---|
| G1 | `lib/net/Tcp.rae`: TCP listen/accept/connect/read/write/close on `SocketId` value handles, non-blocking, errors as `NetStatus` values | — | **landed 0.1.202** with the policy in C; T2 redoes the split |
| T1 ✅ | `Task.isDone()` and a non-blocking `tryGet() ret opt T` (docs/concurrency-model.md) — **done** 2026-10-08 (0.1.216) | S | how a server picks up `spawn`ed slow work without blocking its frame |
| T2 | **landed** — `lib/net` as Rae over a syscall shim (§1.2): `runtime_net.c` shrinks to one-syscall functions + a constant table; the listen sequence, retries, connect timeout, options and status mapping move to Rae; same API, fixture 1022 unchanged | M | done before G2 builds on it |
| T3 | the runtime-wide rule: C only for platform ABI, in AGENTS.md, plus an audit list of logic in today's runtime C that could move to Rae | S | the rule §1.2 follows |
| G2 ✅ | **landed** 2026-10-08 (0.1.217): `lib/net/Poller.rae` (`createPoller`, `pollerWatch` / `pollerForget` per `SocketId`, `pollWait(poller:, ready: mod List(Readiness), timeoutMs:)`, `closePoller`) over three one-call kqueue shims (`pollerCreate` / `pollerChange` / `pollerWait`, events copied out of the per-OS `struct kevent` on the stack); EINTR retry and event translation in Rae; no allocation per wait; Linux answers `unsupported` until epoll (F8); fixture 1035. Was: `lib/net` poller: a kqueue shim (register/unregister/wait returning raw events) and Rae on top (`createPoller`, interest per `SocketId`, `pollWait` filling a caller-owned `List(Readiness)`, no allocation per wait); epoll later behind the same Rae API (F8) | M | used by both styles |
| G3 ✅ | **landed** 2026-10-08 (0.1.219): `lib/net/ByteBuffer.rae` (`createByteBuffer`, `appendBytes`, `appendText`, `consume`, `byteBufferIndexOf`, `byteBufferText`, `byteBufferAt`, `byteBufferClear`, `byteBufferReserve`) and `socketReadBuffer` / `socketWriteBuffer` in `lib/net/Tcp.rae`; consuming moves an offset, compaction is lazy (only when an append needs the room), zero allocations per request-sized cycle after warmup (fixture 1037). Was: a byte buffer for I/O, pure Rae: append, consume from the front, find a byte sequence, read a range as a `String`, reuse without reallocating | M | read and write buffers per connection |
| G4 ✅ | **landed** 2026-10-08 (0.1.220): `Time.httpDate` / `HttpDateCache` (IMF-fixdate from epoch seconds, made once per second), `Time.waitUntil(deadlineNs:)` over `nowNs()` and a one-call `nanosleep` shim (`sleep(ms:)` is whole milliseconds; a 33.333 ms period needs finer), chunked to beat macOS timer coalescing (33 ms waits: median lateness 4.3 ms → 0.3 ms, max under 0.9 ms), and `Ticker` (fixed rate, lateness never accumulates, overruns counted); fixture 1038. Was: time for servers: IMF-fixdate `Date` header in Rae (cached once per second), a monotonic clock and a deadline wait (a clock/sleep shim only if `nowNs()`/`sleep` cannot do it) | S | a 30 Hz tick needs sub-millisecond deadlines |
| G5 ✅ | **landed** 2026-10-08 (0.1.225): `lib/crypto/Sha1.rae` (`sha1`, `sha1Range`, `sha1Text`, `hexText`), `lib/text/Base64.rae` (`base64Encode` / `appendBase64`, strict `base64Decode` / `appendBase64Decoded`) and `lib/webSocket/Handshake.rae` (`webSocketAccept(key:)`), all Rae, except that `sha1` calls the platform's hardware SHA-1 where there is one (CommonCrypto on Apple, one shim in `runtime_crypto_platform.c`; the Rae code is the fallback and is checked against it). Fixture 1044 has the RFC 3174 vectors (one million `a` included), the RFC 6455 accept example, the RFC 4648 vectors, the block-padding boundaries and strict-decoding rejections. Measured on 16 MB (M1 Max, release): SHA-1 7 ms through CommonCrypto (the SHA instructions), against 100 ms for the Rae fallback and 77 ms for the same algorithm in plain C; base64 encode 19 ms against 9 ms, decode 29 ms (strict) against 19 ms (unchecked). A handshake hashes about 60 bytes. Was: SHA-1 and base64 in pure Rae (`lib/crypto/Sha1.rae`, `lib/text/Base64.rae` or similar), RFC test vectors | S | the WebSocket handshake |
| G6 (HTTP part ✅) | **HTTP landed** 2026-10-08 (0.1.221): `lib/http/Http.rae` — `httpParseNext` (incremental across reads, pipelining, keep-alive rules, method enum, target / headers / body as `ByteRange`s into the connection's `ByteBuffer`, headers in a reused list: no heap per request), `httpParseConsumed`, `httpPathIs`, `httpHeaderValue`, `httpAppendResponse` / `httpAppendBadRequest` (no allocation after warmup); malformed, oversized, chunked → `badRequest`; fixture 1039. The WebSocket part is still to do. Was: `lib/http` (HTTP/1.1 request parser, response encoder, pipelining, keep-alive) and `lib/webSocket` (upgrade, frame codec, masking) in plain Rae on G1–G5, as functions over bytes with no I/O and no architecture | L | shared by both styles |
| G8 (HTTP part ✅) | **HTTP landed** 2026-10-08 (0.1.222): `lib/net/ecs/HttpServer.rae` (the `HttpServer` World: `Socket`, `ReadBuffer`, `WriteBuffer`, `HttpParse`, `KeepAlive`, `AwaitingResult` components, the `Readable` / `Writable` / `Sending` / `Closing` tags, the `HttpRequestEvent` queue, a `BufferPool` of released connection buffers; `openConnection`, `destroyConnection`, and `respond` / `promote` for route handlers) and `lib/net/ecs/HttpSystems.rae` (the ten systems in §7's order, run by `httpServerFrame`); the app's router is a type `R` with a `route` function that `dispatchSystem` calls; zero allocations per pipelined request after warmup; fixture 1040 runs it with no network on detached connections. The frame-decode systems (WebSocket) are still to do. Was: `lib/net/ecs`: the server components and event types (§7), the systems (poll with readiness tags, accept, read, parse into `EventQueue(HttpRequest)`, write, timeout, close, frame decode), the end-of-frame consume + `frameAdvance`, the promotion to `AwaitingResult` with its `Task` (T1), and a World buffer pool, on `lib/ecs` | M | the reusable ECS server core; the benchmark's ECS servers add only route handlers and room systems |
| G7 | measure, then fix if needed: `Json` serialisation and `String` building cost per request (no ticket until phase 1 numbers show a problem) | — | listed so the result is read correctly |

**Phasing:**

0. T1, T2, T3 first (the rule, the corrected socket split, task polling).
1. G2, G3, G4, G6 (HTTP part) and G8 (HTTP systems), then phase 1. Phase 1
   is both Rae styles plus the Rust, Node and Bun servers, with the checker,
   the runner and the first baseline. **Phase 1 landed 2026-10-08 (0.1.223)**
   in `benchmarks/servers/`, with the baseline in `results/` and
   `check.sh --rae` as a pre-suite case (F7).
2. G5, G6 (WebSocket part), G8 (frame systems) and `loadClient`, then phase
   2.
3. Phase 3 on the same code: only the room logic, the tick and the snapshot
   system are new.

## 10. Decisions (2026-10-08)

Settled with the maintainer: F1 (Rae over a syscall shim), F9 (requests are
events), F11 (no `async`), and "two Rae implementations of every phase,
eventLoop and ECS" are the maintainer's.
The rest are the design's defaults, which the maintainer asked to be taken as
decided.

1. **F1 Sockets: Rae over a syscall-shaped shim** (maintainer, revised
   2026-10-08; §1.2). C holds only `errno` capture, the variadic `fcntl`,
   address resolution and the platform constants, one system call per
   function. Every policy is Rae. Rae code still never sees `errno` or
   `sockaddr`: the shim hands back plain numbers, and `lib/net` maps them to
   `NetStatus`. (The first take, "a runtime C module like FileNotify", put
   policy in C and is superseded.)
2. **F2 Load tools: `oha` for HTTP, and `wrk` only for pipelined plaintext.**
   The spec requires pipelining, and `oha` cannot pipeline. `wrk` (brew) runs
   just that one case through a small committed Lua script. If `wrk` is
   missing, the runner skips the case and says why.
3. **F3 Rust HTTP: `hyper` 1.x on `tokio`.** The comparison is about the
   server core; `axum` can be added later as "what people write".
4. **F4 WebSocket load: our own `loadClient` in Rust.** Phase 3 needs it
   anyway, it sits on the same toolchain as the references, and Go is not
   needed.
5. **F5 Concurrency: one event loop or World per core.** Readiness polling,
   non-blocking sockets, and an acceptor handing sockets out over a channel.
   Both Rae styles use it (an ECS frame needs a poll to run on). *As built in
   phase 1:* there is no acceptor thread. Every worker's poller watches the one
   listening socket and accepts one connection per wake-up
   (`httpServerShareListener`, `acceptsPerFrame: 1`), which spreads the
   connections just as well and needs no channel. Both styles do the same.
6. **F6 Results: committed.** As `benchmarks/list_access` does: a dated
   `results/summary.json` and `metadata.json` baseline, regenerated by hand
   after changes that can move the numbers.
7. **F7 Checker: in the normal suite once it exists.** After phase 1,
   `check.sh` runs against both Rae HTTP servers as one pre-suite case
   (seconds, no load). It guards correctness, never speed.
8. **F8 Platform: macOS first, epoll second.** G2's API is shaped for both,
   but the first pass implements kqueue only. Linux and epoll come when a
   Linux machine is in use; `taskset`-pinned Linux runs then become the
   reference numbers.
9. **F9 Requests are EVENTS, not entities** (maintainer, §7). The rule is
   lifetime, not count: a request lives one frame, so it goes in an
   `EventQueue(HttpRequest)`; connections are the entities. A request that
   outlives its frame is promoted to an `AwaitingResult` component on its
   connection. A dispatch system walks the events in order, so pipelined
   replies keep request order.
10. **F10 Systems in a fixed order, called from the loop.** No `Schedule`.
    The readiness tags already skip idle work, and a schedule's bookkeeping
    would be measured as ECS cost when it is a separate feature.
11. **F11 No `async`/`await`** (maintainer, §1.3). Network I/O is readiness
    polling; slow work is `spawn`ed, and its `Task` is polled with
    `isDone`/`tryGet` (T1) by the frame that owns the request.
