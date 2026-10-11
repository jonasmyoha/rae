# Server benchmarks

Rae servers next to Rust, Node and Bun servers that do the same work. The
design, the decisions and the reasons are in
[docs/server-benchmarks-design.md](../../docs/server-benchmarks-design.md).
Phase 1 is HTTP ([spec/Http.md](spec/Http.md)), phase 2 WebSocket
([spec/WebSocket.md](spec/WebSocket.md)), phase 3 a 30 Hz game room
([spec/GameRoom.md](spec/GameRoom.md)).

## Run

```sh
make bench-servers                      # fetch the load tools once, then run everything (~40 min)
sh benchmarks/servers/check.sh --rae    # build the six Rae servers and check them (seconds)
sh benchmarks/servers/check.sh 8080     # check any HTTP server on port 8080
sh benchmarks/servers/check.sh --websocket 8080   # check any WebSocket server on port 8080
sh benchmarks/servers/check.sh --gameRoom 8080    # check any game room server on port 8080
```

`run.sh` refuses to start when the 1-minute load average is above 0.3 × the
core count (`BENCH_ALLOW_LOAD=1` overrides). These knobs shorten a scratch
run, and `BENCH_RESULTS=/tmp/x` keeps the committed baseline untouched:

- `BENCH_SECONDS`, `BENCH_RUNS`, `BENCH_WARMUP`;
- `BENCH_WORKERS`, `BENCH_IMPLS`, `BENCH_CASES`;
- `BENCH_PHASES` (`http webSocket gameRoom`), `BENCH_WS_RUNS` (3),
  `BENCH_WS_STEP` (1000), `BENCH_WS_MAX_CLIENTS` (15000), `BENCH_WS_LIMIT_MS`
  (250), `BENCH_WS_BROADCASTS` (100 per step);
- `BENCH_ROOM_CLIENTS` (`100 1000 2000`), `BENCH_ROOM_RUNS` (3),
  `BENCH_ROOM_SECONDS` (10).

Examples: `BENCH_SECONDS=2 BENCH_RUNS=2 BENCH_RESULTS=/tmp/x sh run.sh`, and
`BENCH_PHASES=webSocket BENCH_WS_RUNS=1 BENCH_WS_MAX_CLIENTS=2000
BENCH_RESULTS=/tmp/x sh run.sh`. A run of one phase keeps the other phase's
numbers in the results. `BENCH_PHASES=gameRoom BENCH_ROOM_RUNS=1
BENCH_ROOM_SECONDS=3 BENCH_ROOM_CLIENTS="100 1000" BENCH_RESULTS=/tmp/x sh
run.sh` is a two-minute look at the game room.

Dependencies: the Rust toolchain, Node with npm, Bun, `python3` and `curl`.
The HTTP load tools are pinned in `external.lock`: `oha`, and `wrk` for the
pipelined case only. `fetch.sh` builds them into `~/.cache/rae/servers`. The
WebSocket load client is ours (`loadClient/`, Rust, built by `run.sh`), and
`run.sh` installs Node's uWebSockets.js from the lockfile (`npm ci`) when it
is missing.

## What runs

| implementation | source | concurrency |
|---|---|---|
| `rae-eventLoop` | `rae/eventLoop/http/Main.rae` | `WORKERS` threads, each a poll loop over a `List(Connection)` |
| `rae-ecs` | `rae/ecs/http/Main.rae` | `WORKERS` threads, each an `HttpServer` World run by `lib/net/ecs`'s systems |
| `rust` | `rust/src/bin/http.rs` | hyper 1 on tokio (current-thread, or `WORKERS` worker threads); static headers, no writev, mimalloc |
| `node` | `javascript/node/Http.js` | uWebSockets.js; `WORKERS` worker threads fed by an acceptor app |
| `bun` | `javascript/bun/Http.js` | `Bun.serve` with its native `routes` table; `WORKERS` processes with `reusePort` |

- **Shared code.** Both Rae servers use the same `lib/net` (sockets, poller,
  byte buffers) and `lib/http` (parser, encoder), all Rae over one-call
  syscall shims. They differ only in architecture.
- **Accepting.** In both, every worker's poller watches the one listening
  socket and accepts one connection per wake-up. That spreads the connections
  over the workers with no acceptor thread.

Phase 2, WebSocket (one worker each, see the spec):

| implementation | source | how |
|---|---|---|
| `rae-eventLoop` | `rae/eventLoop/webSocket/Main.rae` | the HTTP loop's shape; an upgraded connection's input is frames; output written once per poll |
| `rae-ecs` | `rae/ecs/webSocket/Main.rae` | `lib/net/ecs`'s upgrade and frame systems plus one message system |
| `rust` | `rust/src/bin/webSocket.rs` | `rust/src/ws.rs` on a current-thread tokio runtime: frames encoded once, output corked and written once per client per batch; the payload passed through as raw JSON |
| `node` | `javascript/node/WebSocket.js` | uWebSockets.js, broadcast as `publish` to one topic |
| `bun` | `javascript/bun/WebSocket.js` | `Bun.serve` websockets, broadcast as `publish` to one topic |

Both Rae servers use `lib/webSocket` (upgrade, frame decoder and encoder) and
the same few lines for the shootout's JSON: the `type` and the `payload` are
found in place (`jsonScanMember`), and the broadcast frame is built once from
the payload's bytes in the read buffer, not by re-serialising.

Phase 3, game room (one worker, one room; see the spec):

| implementation | source | how |
|---|---|---|
| `rae-eventLoop` | `rae/eventLoop/gameRoom/Main.rae` | the player lives on its connection; an input is applied when decoded; the tick encodes one snapshot and copies it to each connection |
| `rae-ecs` | `rae/ecs/gameRoom/Main.rae` | `PlayerState` table, `PlayerInputEvent` queue, membership / tick / snapshot systems on `lib/net/ecs` |
| `rust` | `rust/src/bin/gameRoom.rs` | `rust/src/ws.rs`; a clock thread sleeps to each deadline in halving steps and wakes the tick, which queues one shared snapshot on every player |
| `node` | `javascript/node/GameRoom.js` | uWebSockets.js, a setTimeout chain aimed at each deadline |
| `bun` | `javascript/bun/GameRoom.js` | `Bun.serve` websockets, the same setTimeout chain |

Both Rae servers wait for each tick's deadline with `Time.waitUntil` (G4) and
poll for I/O until about a millisecond before it.

## How it measures (`run.sh`, `measure.py`)

Every implementation runs in two configurations: 1 worker, and all
performance cores. For each configuration, each implementation:

1. is started and passes `check.sh` (a failing check skips its timing);
2. then, per case (`plaintext`, `json`, `pipelined`), gets a 5 s warmup and
   5 timed runs of 15 s;
3. has its runs interleaved with the other implementations'. The two Rae
   styles swap order every run, so drift on the machine favours neither.

Reported, as the median with min and max:

- requests/s;
- latency p50, p99 and p99.9 (not for `pipelined`, see spec);
- peak RSS of the server's process group (sampled every 100 ms);
- CPU seconds;
- for Rae, allocations per request (each worker prints its counts once a
  second when `RAE_BENCH_STATS=1`).

WebSocket: every implementation is started once and passes `check.sh
--websocket`, then gets 3 ramps of `loadClient`, interleaved like the HTTP
runs. A ramp adds 1 000 clients at a time; after each step 4 clients send 100
broadcasts in all (4 in flight) and the round trip is from sending one to its
sender receiving the broadcastResult, which is queued after the broadcast is
encoded for every client (spec/WebSocket.md has what that includes).
Reported per step (median, min and max over the ramps): broadcast RTT p50 and
p99. Also the client count reached with p99 under 250 ms, peak RSS, CPU
seconds, and for Rae the allocations per message, which here include every
client's connection setup (buffers, the upgrade).

Game room: every implementation is started once and passes `check.sh
--gameRoom`, then gets 3 runs of `loadClient gameRoom` at each room size
(100, 1 000, 2 000 players), interleaved the same way. Reported (median, min
and max over the runs): tick lateness, tick duration and snapshot
inter-arrival jitter (each p50, p99, max), missed ticks, the share of dropped
snapshots, peak RSS, CPU seconds, and for Rae the allocations per tick (which
include the players' connection setup during the run). The room stops at
2 000 players on one machine: see the spec for why 5 000 needs a second one.

`results/summary.json` also holds the ECS/eventLoop ratio for every case,
with "no measurable difference" when the two styles' min–max ranges overlap.
`results/metadata.json` records the machine, OS, toolchains, the Rae commit
and the load average. Raw runs and server logs stay in `build/raw/`.

**Limits.**

- The load generator runs on the same machine and competes for cores. It has
  a fixed 4 threads.
- macOS cannot pin threads.
- Bun's `reusePort` does not balance connections across processes on macOS.
- wrk's latency histogram is wrong under pipelining, so that case reports
  throughput only.

## Results

`results/summary.json` holds the full numbers. It is regenerated by hand
after changes that can move them (decision F6).

### How fair are these numbers? Read this first

These benchmarks live in the Rae repository and were written by the Rae
project. Treat the results as "Rae is competitive with typical
implementations in these languages", not "Rae is faster than Rust".

**What is fair:**

- **One spec for all:** every server implements the same written spec
  (spec/*.md).
- **Correctness first:** each must pass the same correctness checks
  (`check.sh`) before it is measured.
- **The same conditions:** all run on the same machine, with the runs
  interleaved, and share one load client.
- **Recorded:** library versions are pinned (lockfiles), and the
  toolchains, load average and commit are written into `metadata.json`.

**Where the bias is:**

- **Effort is unequal.**
  - **Rae:** the whole server stack was built in this repository: sockets,
    the poller, the HTTP parser, WebSocket framing and `lib/net/ecs`.
    Around 18 commits worked on it, and its numbers were looked at and
    improved in response. For example, when the ECS server measured 2×
    slower than the event loop, the cause was found and fixed (0.1.250).
  - **Rust, Node and Bun:** first written once, under the rule
    "idiomatic, the language's normal way, not micro-tuned"
    (docs/server-benchmarks-design.md §4). On 2026-10-11 they were
    replaced by **tuned** versions, to see how much of Rae's lead was
    effort (the idiomatic versions and their numbers are in git history;
    the results below are the tuned ones):
    - **Rust WebSocket and game room:** the idiomatic versions gave each
      connection an unbounded channel and a writer task
      (tokio-tungstenite), and the game room used tokio's millisecond
      interval, which fell 19–37 ms behind at 2 000 players. The tuned ones
      share `rust/src/ws.rs`: frames encoded once, output corked like
      uWebSockets (queued, then written once per client per batch of ready
      tasks), the broadcast payload passed through as raw JSON, and a
      clock thread that sleeps to each tick's deadline in halving steps.
    - **Rust HTTP:** hyper as its TechEmpower entries set it up: static
      header values, no writev, mimalloc.
    - **Node:** uWebSockets.js (the C++ server Bun is built on, as a
      native addon) in place of `node:http` and `ws`, the fastest way to
      serve from Node.
    - **Bun:** already on uWebSockets; HTTP routes go through Bun's
      native `routes` table, and the broadcast payload is serialised once.

    Still written by us, in the Rae repository: the gap in effort is
    smaller, the author is the same.
- **One author for all**, writing inside the Rae project, with the
  incentives that implies.
- **The same in reverse elsewhere:** the same would hold for a benchmark
  written in a Bun or Rust repository. A project's own benchmarks tend to
  favour it, through effort and familiarity rather than through a rigged
  measurement.

**How much of each server is our code:**

| | our code | what does the work |
|---|---|---|
| Bun 1.3.5 | 39–83 lines of JS | Bun's runtime is Zig; its HTTP / WebSocket server is uWebSockets (C++). Close to off-the-shelf Bun |
| Node 26.5 | 44–86 lines of JS | uWebSockets.js (uWebSockets, C++, as a native addon) |
| Rust 1.93 | 80 lines (HTTP); 88–166 lines plus the shared 340-line `ws.rs` | `hyper` 1.x and `tokio` for HTTP; for WebSocket our own framing on `tokio`, with `httparse` for the upgrade request |
| Rae | 99–613 lines per server, plus `lib/net`, `lib/http`, `lib/webSocket` | all written in this repository |

**What would make them fairer:**

- Rust, Node and Bun servers written or reviewed by people fluent in
  those stacks (the tuned versions are our best effort, not theirs).
- A second machine for the load generator: the 8-worker HTTP numbers
  measure it, not the servers.

**Independent benchmarks** (none peer-reviewed, but public and open to
scrutiny):

- **HttpArena** (github.com/MDA2AV/HttpArena, www.http-arena.com), the
  active successor to TechEmpower:
  - each entry is a directory with its own Dockerfile, added by a pull
    request and benchmarked on the same machine as every other (a 64-core
    Threadripper, each entry pinned to its own cores);
  - its profiles include plaintext, pipelined, JSON and WebSocket (our
    three phases' kind of work), plus TLS, HTTP/2 and /3, gRPC, database
    queries and static files;
  - standard and tuned variants are told apart, and an entry that skips
    the realistic request handling loses part of its score.
- the **TechEmpower Framework Benchmarks**: the long-time reference
  (2013–2025, Round 23 the last), **archived in March 2026**. Its results
  stay useful as history, but it takes no new entries.
- the **websocket-shootout** (Hashrocket): the protocol our WebSocket phase
  follows, with implementations from 2016.
- **vendors' own benchmarks** (Bun, uWebSockets): biased toward their
  authors, as this one is toward Rae.

**Future:** Rae may want to **take part in HttpArena**, with a Rae entry for
its plaintext, JSON and WebSocket profiles first (it would need to build in
a Linux container: see the queued Linux task). There Rae would be measured
on independent hardware against entries tuned by their own communities,
which is the comparison these numbers cannot give.

### Baseline with the tuned references, 2026-10-11

Apple M1 Max (8 performance cores), macOS 27, rae 0.1.268 (commit 68132e3b),
rustc 1.93.1, Node 26.5.1 with uWebSockets.js 20.71.0, Bun 1.3.5. One run of
every phase, the servers interleaved as always.

**The machine was not idle.** The load average was 5.3 at the start (and
about 11 at the end, the run's own load included), above run.sh's gate of
3.0, which was overridden (`BENCH_ALLOW_LOAD=1`): the desktop apps running
would not let it drop. Every server ran under the same load, so the
comparison holds, but absolute numbers may sit a few percent low. The
idiomatic references' numbers (2026-10-08 HTTP, 2026-10-09 game room,
2026-10-11 WebSocket, all on an idle machine) are in git history.

**The short version: Rae is in the same range as tuned Rust, Node and Bun.**
It leads single-core HTTP throughput by about 10%, ties on WebSocket fan-out,
and keeps game-room ticks on time; tuned Rust has the best HTTP p99, the
shortest game-room tick and the least memory.

#### HTTP

Medians of 5 × 15 s runs at 256 connections, in requests/s, with p99 latency
in ms where measured:

| case | rae-eventLoop | rae-ecs | rust (hyper) | node (uWS.js) | bun |
|---|---|---|---|---|---|
| 1 worker, plaintext | 129 202 (3.32) | 135 932 (3.69) | 121 893 (2.35) | 117 162 (3.30) | 99 063 (3.02) |
| 1 worker, json | 128 655 (3.30) | 134 519 (3.71) | 121 335 (2.35) | 116 001 (2.71) | 96 999 (3.04) |
| 1 worker, pipelined ×16 | 1 395 001 | 1 384 049 | 1 169 410 | 756 015 | 19 833 * |
| 8 workers, plaintext | 156 914 (4.63) | 153 129 (1.93) | 157 832 (2.06) | 149 389 (2.09) | 102 838 (3.02) |
| 8 workers, json | 147 765 (5.49) | 146 746 (2.12) | 150 239 (2.35) | 143 917 (2.45) | 98 119 (3.74) |
| 8 workers, pipelined ×16 | 2 247 832 | 2 400 198 | 2 348 722 | 2 153 392 | 17 153 * |

How to read it:

- **1 worker:** Rae 1.06–1.12× tuned Rust, 1.10–1.16× Node on
  uWebSockets.js, 1.3–1.4× Bun. Rust's p99 is the lowest (2.35 ms against
  Rae's 3.3–3.7 ms). Pipelined, Rae is 1.2× Rust and 1.8× Node.
- **Node moved most with tuning:** uWebSockets.js in place of `node:http`
  took it from 46k to 117k requests/s on one worker, and pipelined from 80k
  (with errors) to 756k (no errors).
- **8 workers:** everyone but Bun lands at 144k–158k requests/s plain and
  2.2M–2.4M pipelined, the ceiling of the 4-thread load generator on the same
  machine, not of the servers. rae-eventLoop's 8-worker p99 (4.6–5.5 ms) is
  the one clear tail-latency outlier; rae-ecs does not have it.
- **ECS against the event loop:** 0.98–1.07× in requests/s, as before.
- **\* Bun** answers 16-deep pipelining very slowly on macOS (about 18k
  requests/s), as in every earlier run; Bun's multi-process `reusePort` does
  not spread connections on macOS, so its 8-worker numbers are one process.
- No errors in any case this time.
- Source size (code lines): rae-eventLoop 285, rae-ecs 82, Rust 65, Node 46,
  Bun 37.

#### WebSocket

One worker each, 3 ramps per server. Broadcast round trip in ms, p50 / p99
(median of the ramps), and the client count each ramp reached with p99
under 250 ms (cap 15 000):

| clients | rae-eventLoop | rae-ecs | rust (tuned) | node (uWS.js) | bun |
|---|---|---|---|---|---|
| 1 000 | 4.5 / 14.9 | 4.6 / 13.8 | 5.3 / 16.0 | 4.9 / 15.1 | 4.8 / 15.1 |
| 5 000 | 21.8 / 23.4 | 21.8 / 23.9 | 23.2 / 24.4 | 22.9 / 26.0 | 23.9 / 26.5 |
| 10 000 | 43.5 / 45.5 | 44.6 / 47.4 | 45.2 / 50.1 | 46.1 / 49.7 | 48.3 / 50.4 |
| 15 000 | 66.6 / 72.4 | 67.3 / 70.0 | 70.0 / 77.2 | 73.7 / 182.1 (2 ramps) | 72.7 / 236.9 (2 ramps) |
| reached | 15 000 (14 000–15 000) | 15 000 (all 3) | 15 000 (14 000–15 000) | 14 000 (13 000–15 000) | 14 000 (13 000–15 000) |
| peak RSS | 336 MB | 273 MB | 318 MB | 64 MB | 46 MB |

How to read it:

- **All five are within about 10% of each other** up to 10 000 clients:
  each broadcast costs 4.3–4.8 ms per 1 000 clients. Rae is a few percent
  ahead; the idiomatic Rust (70 ms at 5 000, 1.5 GB) and `ws` Node (82 ms)
  were 3–4× behind, and that gap was all in how they were written.
- **What closed it for Rust** was corking: queueing a broadcast's frame on
  every connection and writing each one once per batch of ready tasks (as
  uWebSockets and Rae do), instead of one channel message, task wake-up and
  write per client per broadcast.
- **Near the cap single broadcasts stall** for hundreds of ms on every
  server (p99 max 270–430 ms at 14 000–15 000); "reached" above 13 000 is
  the edge of this machine, whose cores the load client shares.
- **Memory:** Bun and Node (uWebSockets underneath) use 4–7× less than Rae
  and Rust at 15 000 clients.
- Node and Bun each had 7–8 wrong listenCounts at the 1 000-client step
  (clients counted while their connection was still being set up); none
  later, and none for Rae or Rust.

#### Game room

One worker each, 3 runs of 10 s per room size. Medians of the runs; each cell
is p50 / p99 in ms:

| players | metric | rae-eventLoop | rae-ecs | rust (tuned) | node (uWS.js) | bun |
|---|---|---|---|---|---|---|
| 100 | tick lateness | 0.41 / 0.62 | 0.43 / 0.62 | 0.27 / 0.53 | 0.62 / 1.35 | 0.65 / 1.21 |
| | tick duration | 0.03 / 0.04 | 0.03 / 0.04 | 0.01 / 0.01 | 0.40 / 0.51 | 0.41 / 0.53 |
| | arrival jitter | 0.20 / 0.74 | 0.17 / 0.61 | 0.14 / 0.56 | 0.34 / 1.34 | 0.45 / 1.00 |
| 1 000 | tick lateness | 0.43 / 0.65 | 0.48 / 0.66 | 0.21 / 0.52 | 0.70 / 1.65 | 0.68 / 1.23 |
| | tick duration | 0.67 / 0.76 | 0.65 / 0.73 | 0.04 / 0.06 | 4.84 / 6.17 | 4.85 / 6.26 |
| | arrival jitter | 0.95 / 3.32 | 0.87 / 3.06 | 1.00 / 3.75 | 0.98 / 3.79 | 0.97 / 3.45 |
| 2 000 | tick lateness | 0.46 / 0.65 | 0.46 / 0.66 | 0.26 / 0.52 | 0.70 / 1.66 | 0.65 / 1.21 |
| | tick duration | 1.20 / 1.36 | 1.21 / 1.35 | 0.07 / 0.11 | 15.3 / 16.3 | 15.7 / 16.8 |
| | arrival jitter | 0.22 / 1.34 | 0.22 / 1.32 | 0.26 / 1.58 | 0.52 / 2.01 | 0.47 / 1.99 |
| | peak RSS | 112 MB | 122 MB | 47 MB | 60 MB | 59 MB |

No server missed a tick, dropped a snapshot or sent a wrong one.

How to read it:

- **Everyone keeps time now.** The idiomatic Rust started its ticks 19–37 ms
  late at 2 000 players (tokio's millisecond interval) and missed 25 in
  10 s; the tuned one, a clock thread sleeping to each deadline in halving
  steps, starts them 0.2–0.5 ms late, slightly earlier than Rae's
  `Time.waitUntil` (0.4–0.7 ms). Node and Bun stay under 1.7 ms.
- **What players feel**, the snapshot arrival jitter, is the same for Rae
  and Rust (p99 1.3–1.6 ms at 2 000) and a little higher for Node and Bun.
- **Tick duration** measures different work: Rust only queues one shared
  buffer per player (the writes come right after the tick); Rae copies the
  snapshot into each connection's buffer (about 0.6 ms per 1 000 players);
  Node and Bun send per socket inside the tick, so theirs is 7–13× Rae's.
- **Memory:** tuned Rust uses less than half of Rae's at 2 000 players
  (47 MB against 112–122 MB); Rae holds a copy of the snapshot per
  connection.
- **ECS against the event loop: no measurable difference.**
- Above about 2 500 players this machine's loopback runs out of kernel
  network memory for every server, so 5 000 needs a second machine
  (spec/GameRoom.md).
