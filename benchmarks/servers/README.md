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
    effort (the idiomatic ones are in the git history, results of
    2026-10-08 and 2026-10-11 before this note):
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

### Baseline, 2026-10-08

Apple M1 Max (8 performance cores), macOS, the Rae of this commit. metadata.json
says 0.1.222-dev+dirty because the build came before the bump to 0.1.223.
Medians of 5 × 15 s runs at 256 connections, in requests/s, with p99 latency
in ms where measured:

| case | rae-eventLoop | rae-ecs | rust (hyper) | node | bun |
|---|---|---|---|---|---|
| 1 worker, plaintext | 133 762 (3.70) | 137 801 (3.00) | 123 805 (2.77) | 46 504 (9.27) | 93 978 (4.47) |
| 1 worker, json | 128 422 (3.47) | 133 424 (2.91) | 122 297 (2.44) | 45 573 (6.58) | 89 908 (3.83) |
| 1 worker, pipelined ×16 | 1 226 861 | 1 244 420 | 1 073 868 | 80 297 * | 18 024 * |
| 8 workers, plaintext | 151 062 (2.23) | 152 896 (2.19) | 154 040 (2.47) | 154 990 (5.53) | 87 295 (3.45) |
| 8 workers, json | 150 858 (2.36) | 151 626 (2.32) | 153 710 (2.61) | 152 625 (7.37) | 86 200 (3.73) |
| 8 workers, pipelined ×16 | 1 860 308 | 1 898 463 | 1 741 135 | 259 204 * | 15 314 * |

How to read it:

- **ECS against the event loop: 1.01–1.04×** in requests/s, and inside the
  run-to-run spread in five of the six cases. In the sixth (1 worker, json),
  ECS is 4% faster.
  - ECS uses 1.3–1.5× the RSS (about 15–24 MB against 12–17 MB) for its
    component tables.
- **Allocations per request:** 0 for plaintext and pipelined, 2 for json in
  both styles: the per-request `toJson` serialisation (G7 measures that cost).
- **Source size (code lines):** the ECS server is 82, because its machinery is
  `lib/net/ecs`. The event-loop server is 285, because it carries its own
  loop. Rust is 63, Node 30, Bun 40.
- **8 workers:** every implementation except Bun stops near 150k requests/s.
  That is the 4-thread load generator on the same machine, not the servers.
  Per-core numbers there need a second machine.
- **\* marks the pipelining errors:**
  - Node answers pipelined requests with many errors (190 859 at 1 worker).
  - Bun answers 16-deep pipelining very slowly on macOS (about 18k
    requests/s, with some errors).
  - Both are recorded as measured.

### WebSocket baseline, 2026-10-11

Same machine, one worker each, 3 ramps per server, load average 2.88 at the
start (run.sh's gate is 3.0), rae 0.1.268. Broadcast round trip in ms,
p50 / p99 (median of the ramps that got there), and the client count each
ramp reached with p99 under 250 ms (cap 15 000):

| clients | rae-eventLoop | rae-ecs | rust | node | bun |
|---|---|---|---|---|---|
| 1 000 | 4.3 / 15.5 | 4.3 / 14.2 | 13.3 / 25.9 | 15.9 / 36.4 | 4.5 / 17.6 |
| 5 000 | 20.5 / 23.2 | 20.6 / 22.9 | 69.7 / 132.6 | 81.5 / 186.2 | 22.7 / 25.1 |
| 10 000 | 41.4 / 44.1 | 41.3 / 45.2 | 140.0 / 237.8 (2 ramps) | — | 46.2 / 49.1 |
| 15 000 | 63.4 / 69.8 (2 ramps) | 62.7 / 65.5 | — | — | 69.4 / 72.9 |
| reached | 15 000 (13 000–15 000) | 15 000 (all 3) | 9 000 (8 000–10 000) | 6 000 | 15 000 (14 000–15 000) |
| peak RSS | 336 MB | 275 MB | 1 494 MB | 158 MB | 47 MB |
| allocations / message | 101 | 171 | — | — | — |

How to read it:

- **ECS against the event loop: no measurable difference** at any step
  (p50 ratio 0.99–1.04). The round trip grows linearly with the fan-out,
  about 4.1 ms per 1 000 clients, for both, and both now reach the 15 000
  cap with p99 under 70 ms.
- **The ECS write order matters.** The first 2026-10-09 baseline had ECS at
  2× the event loop. The cause was `writeSystem` writing the `Sending`
  connections newest-first, so the senders' broadcastResults went out after
  every other client's copy. It writes oldest-first since 0.1.250, like the
  event loop's slot order.
- **Near the cap single broadcasts can stall** for hundreds of ms. One
  rae-eventLoop ramp stopped at 13 000 (p99 707 ms at 14 000) and one Bun
  ramp at 14 000 (p99 928 ms at 15 000). Read "reached" above 13 000 as the
  edge of this machine (the load client shares its cores), not a ranking.
- **Rust's RSS** is its unbounded per-connection channels holding the
  broadcast backlog.
- **Allocations per message** include every client's connection setup in
  the ramp, so they are not a per-message cost.
- **Every listenCount was right** in this run (the 2026-10-09 run had 7
  wrong Bun counts at 1 000 clients).

The previous baseline (2026-10-09, load 2.75, rae 0.1.250) gave the same
picture within a few percent; it is in git history.

### Game room baseline, 2026-10-09

Same session, one worker each, 3 runs of 10 s per room size. Medians of the
runs; each cell is p50 / p99 in ms:

| players | metric | rae-eventLoop | rae-ecs | rust | node | bun |
|---|---|---|---|---|---|---|
| 100 | tick lateness | 0.43 / 0.64 | 0.37 / 0.63 | 1.13 / 2.04 | 0.70 / 1.36 | 0.61 / 1.21 |
| | tick duration | 0.03 / 0.05 | 0.03 / 0.07 | 0.02 / 0.04 | 0.48 / 0.72 | 0.41 / 0.51 |
| | arrival jitter | 0.15 / 0.63 | 0.21 / 0.65 | 0.43 / 1.60 | 0.55 / 1.28 | 0.43 / 0.93 |
| 1 000 | tick lateness | 0.50 / 0.66 | 0.47 / 0.65 | 1.02 / 2.03 | 0.79 / 1.68 | 0.67 / 1.22 |
| | tick duration | 0.68 / 0.85 | 0.68 / 0.85 | 0.13 / 0.23 | 6.21 / 7.58 | 5.59 / 7.05 |
| | arrival jitter | 0.46 / 3.24 | 0.37 / 3.79 | 0.58 / 2.15 | 0.60 / 3.30 | 0.65 / 4.03 |
| 2 000 | tick lateness | 0.48 / 0.65 | 0.46 / 0.66 | 19.2 / 36.7 | 0.61 / 5.04 | 0.65 / 1.23 |
| | tick duration | 1.23 / 1.55 | 1.21 / 1.46 | 0.23 / 0.35 | 18.1 / 21.2 | 16.3 / 19.3 |
| | arrival jitter | 0.34 / 4.45 | 0.34 / 3.54 | 3.09 / 20.8 | 0.65 / 4.66 | 0.49 / 2.89 |
| | missed ticks | 0 | 0 | 25 | 0 | 0 |
| | peak RSS | 115 MB | 122 MB | 510 MB | 145 MB | 39 MB |

No server dropped a snapshot or sent a wrong one at these sizes.

How to read it:

- **Tick lateness:** both Rae servers start every tick within 0.7 ms of its
  deadline (`Time.waitUntil`, p99) at every size. Node and Bun stay near 1 ms
  until Node's 2 000-player ticks.
- Rust's tokio interval runs on millisecond timers. At 2 000 players its
  current-thread runtime falls behind: ticks start 19–37 ms late and 25 are
  missed in 10 s.
- **Tick duration** is the snapshot's encode and fan-out:
  - Rae copies the snapshot into each connection's buffer, about 0.6 ms per
    1 000 players at 1 000;
  - Rust shares one `Bytes` buffer and only queues it, so its tick is shorter,
    but the writes happen after it;
  - Node and Bun send per socket inside the tick, so their tick is 9–15× Rae's.
- **ECS against the event loop: no measurable difference** in tick duration
  at any size (ratio 1.23, 1.00 and 0.98). The ECS style's extra machinery
  (an input event queue, membership, a component table) costs nothing visible
  next to the N² fan-out.
- Allocations per tick (rae-eventLoop 3 / 29 / 52, rae-ecs 5 / 45 / 100)
  include the players' connection setup during the run.
- Above about 2 500 players this machine's loopback runs out of kernel
  network memory for every server, so 5 000 needs a second machine
  (spec/GameRoom.md).

