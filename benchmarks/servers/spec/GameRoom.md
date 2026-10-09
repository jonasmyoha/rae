# Phase 3: game room

Our own spec (no external standard exists). It is committed so that what is
measured cannot drift. `check.sh --gameRoom` tests every rule below.

## The room

- One room. A client joins by opening a WebSocket on `GET /ws` (as in
  spec/WebSocket.md: RFC 6455, no extensions, ping answered, close answered)
  and leaves by closing it.
- A joining player gets the next player id (1, 2, 3, ...) and the position
  (0, 0). A player that left is gone from the next snapshot.
- Every number below is little-endian. Positions are integers.

## Input: client → server, 32 bytes, a binary frame

| bytes | field |
|---|---|
| 0–3 | `sequence`, u32 |
| 4–7 | `moveX`, i32 |
| 8–11 | `moveY`, i32 |
| 12–15 | `buttons`, u32 (carried, unused) |
| 16–23 | `clientTimeNs`, u64 (carried, unused) |
| 24–31 | zero |

- Clients send one input per tick period (30 a second each).
- Every input received before a tick is applied by that tick, in arrival
  order: `moveX` and `moveY` are clamped to −1000…1000, added to the
  position, and the position is clamped to −1 000 000…1 000 000. The
  player's `lastSequence` becomes the input's `sequence`.
- A binary frame that is not 32 bytes, and any text frame, is ignored.

## The tick

- A fixed **30 Hz** tick, scheduled at `start + k × 33 333 333 ns` for tick
  index k. The server waits for the deadline (Rae: `Time.waitUntil`, G4) and
  does not drift: a late tick does not move the next one.
- A tick that starts more than a whole period late skips the periods it
  missed; their indices are never sent, and the server counts them.
- Each tick applies the inputs, then sends **one snapshot to every player**,
  encoded once.
- **Backpressure:** a player whose connection still holds more unsent bytes
  than one snapshot is skipped for that tick (it gets the next one, not a
  backlog).

## Snapshot: server → every client, a binary frame

A 24-byte header, then 16 bytes per player (any order):

| bytes | header field |
|---|---|
| 0–3 | `tick`, u32: the tick index k |
| 4–7 | `playerCount`, u32 |
| 8–11 | `latenessUs`, u32: this tick's actual start minus its scheduled start |
| 12–15 | `previousTickUs`, u32: the previous tick's duration, from its start until its snapshot was queued on every connection |
| 16–19 | `missedTicks`, u32: periods skipped so far |
| 20–23 | zero |

| bytes | per player |
|---|---|
| 0–3 | `playerId`, u32 |
| 4–7 | `x`, i32 |
| 8–11 | `y`, i32 |
| 12–15 | `lastSequence`, u32 (0 before any input) |

A snapshot is `24 + 16 × N` bytes, so a tick sends N snapshots of size ∝ N:
the fan-out cost grows as N².

## Workers

One worker, as in phase 2: the room is one place.

## Load (run.sh, loadClient gameRoom)

`loadClient gameRoom` connects N clients (N = 100, 1 000, 2 000 by default,
`BENCH_ROOM_CLIENTS`), each sending its 30 Hz inputs at its own phase and
reading every snapshot (raw sockets and a frame scanner: no allocation per
snapshot on the client). After 1 s of warmup it measures for
`BENCH_ROOM_SECONDS` (10):

- **tick lateness** and **tick duration** (p50 / p99 / max, from the headers
  one client receives);
- **missed ticks** (the header's count over the window);
- **snapshot inter-arrival jitter**: |interval − 33.3 ms| between a client's
  consecutive snapshots, over all clients (p50 / p99 / max);
- **dropped snapshots**: the share of the window's ticks a client did not
  receive (backpressure or loss);
- wrong snapshots (a size that does not match `playerCount`, or a
  `playerCount` other than N);
- and from the runner: CPU, peak RSS, and Rae's allocations per tick.

**N = 5 000 needs a second machine.** A tick at 5 000 players is 5 000
snapshots of 80 KB: 400 MB, 12 GB/s at 30 Hz, server and load client on one
loopback. On the M1 Max (macOS 27) every server, the Rust reference included,
collapses above about 2 500 players: the kernel starts refusing network
buffer memory (`netstat -m`: hundreds of thousands of "requests for memory
denied" a minute), sockets stall, and clients miss most snapshots with
gaps of seconds while both processes sit idle. So the same-machine runner
stops at 2 000; `BENCH_ROOM_CLIENTS="5000"` still runs it, and a client on a
second machine is the way to measure it.
