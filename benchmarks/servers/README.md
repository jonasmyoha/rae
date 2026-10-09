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
`run.sh` installs Node's `ws` from the lockfile (`npm ci`) when it is missing.

## What runs

| implementation | source | concurrency |
|---|---|---|
| `rae-eventLoop` | `rae/eventLoop/http/Main.rae` | `WORKERS` threads, each a poll loop over a `List(Connection)` |
| `rae-ecs` | `rae/ecs/http/Main.rae` | `WORKERS` threads, each an `HttpServer` World run by `lib/net/ecs`'s systems |
| `rust` | `rust/src/bin/http.rs` | hyper 1 on tokio (current-thread, or `WORKERS` worker threads) |
| `node` | `javascript/node/Http.js` | `node:http`; `node:cluster` with `WORKERS` processes |
| `bun` | `javascript/bun/Http.js` | `Bun.serve`; `WORKERS` processes with `reusePort` |

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
| `rust` | `rust/src/bin/webSocket.rs` | tokio-tungstenite, current-thread runtime, a writer task per connection |
| `node` | `javascript/node/WebSocket.js` | the `ws` package |
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
| `rust` | `rust/src/bin/gameRoom.rs` | tokio interval (missed ticks skipped), one `Bytes` snapshot shared by every writer task |
| `node` | `javascript/node/GameRoom.js` | `ws`, a setTimeout chain aimed at each deadline |
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

### WebSocket baseline, 2026-10-09

Same machine, one worker each, 3 ramps per server (load average 2.75 at the
start), re-run after the ECS write-order fix (0.1.250). Broadcast round trip
in ms, p50 / p99 (median of the ramps that got there), and the client count
each ramp reached with p99 under 250 ms (cap 15 000):

| clients | rae-eventLoop | rae-ecs | rust | node | bun |
|---|---|---|---|---|---|
| 1 000 | 4.5 / 16.0 | 4.6 / 14.9 | 13.9 / 28.6 | 17.1 / 39.5 | 4.9 / 16.1 |
| 5 000 | 21.4 / 23.6 | 21.5 / 25.6 | 70.3 / 142.3 | 84.8 / 195.3 | 23.6 / 28.2 |
| 10 000 | 42.6 / 45.0 | 43.3 / 47.1 | 142.5 / 283.7 | — | 47.4 / 52.6 |
| 15 000 | 128.2 / 978.4 (1 ramp) | 65.4 / 70.6 (2 ramps) | — | — | 140.5 / 637.0 |
| reached | 13 000 (13 000–14 000) | 15 000 (13 000–15 000) | 9 000 (8 000–9 000) | 6 000 | 14 000 (14 000–15 000) |
| peak RSS | 336 MB | 273 MB | 1 429 MB | 163 MB | 47 MB |

How to read it:

- **ECS against the event loop: no measurable difference** from 5 000 to
  13 000 clients (p50 ratio 1.00–1.02, overlapping ranges). The round trip
  grows linearly with the fan-out, about 4.3 ms per 1 000 clients, for both.
- The first baseline that morning had ECS at 2× the event loop. The cause was
  `writeSystem`'s order, not ECS: it wrote the `Sending` connections
  newest-first, so the senders' broadcastResults went out after every other
  client's copy. It now writes oldest-first, like the event loop's slot order.
- **Near the 15 000 cap the ramps get noisy** for every fast server. Single
  broadcasts stall for hundreds of ms (rae-eventLoop and Bun here; in the
  morning run it was rae-ecs that stopped early). Read "reached" above 13 000
  as the edge of this machine, not a ranking.
- Rust's RSS is its unbounded per-connection channels holding the broadcast
  backlog.
- Allocations per message (rae-eventLoop 101, rae-ecs 171) include every
  client's connection setup in the ramp, so they are not a per-message cost.
- Bun reported 7 wrong listenCounts at 1 000 clients (its `subscriberCount`
  during the ramp); every other count was right.

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

