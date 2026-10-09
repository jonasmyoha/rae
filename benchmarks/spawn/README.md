# Spawn benchmarks

`spawn` measured: step 1 of
[docs/lightweight-spawn-design.md](../../docs/lightweight-spawn-design.md) §8
measured thread-per-`spawn` (§10), and step 3a's scheduler runs the same
modes on the worker pool (§12).

- `results/` holds the scheduler's numbers.
- `results/threadPerSpawn/` holds the same build with `RAE_SPAWN_THREADS=1`
  (every spawn a thread), taken in the same session for the before/after.

Since step 3b (0.1.252, §13) a sleeping task suspends on the pool too, so
`sleeps` and `held` measure resumable tasks in `results/` and threads in
`results/threadPerSpawn/`.

## Run

```sh
sh benchmarks/spawn/run.sh                  # the baseline (~10 min)
BENCH_QUICK=1 BENCH_RESULTS=/tmp/x sh benchmarks/spawn/run.sh   # smoke run
```

It refuses to start when the load average is above 0.3 × the core count
(`BENCH_ALLOW_LOAD=1` overrides). Every benchmark runs as its own process
under `/usr/bin/time -l`, which gives its peak RSS. The results go to
`results/`:

- `raw.csv` (one row per number);
- `summary.json` (grouped, with speedups);
- `metadata.json`.

## What runs

`rae/Main.rae` (release build), one mode per process:

| mode | what |
|---|---|
| `spawnJoin <n>` | spawn + `get` one at a time, then in batches of 256 |
| `sleeps <n>` | n tasks that each `sleep(ms: 100)`, spawned in a row |
| `held <n>` | n tasks all alive at once: each waits for a deadline after the last spawn |
| `latency <n>` | spawn → task running, and spawn → `get` returned (percentiles) |
| `sum <n> <leaf>` | fork-join sum, split in two down to `leaf`, against sequential |
| `sort <n> <leaf>` | fork-join merge sort, the same way |
| `work <n> <leaf>` | fork-join over a range of compute-bound items: nothing to copy, so the scheduler alone (design §14), at 1 and 8 workers |

A spawn cannot share read-only data today. A `view` argument makes the call
run synchronously, so the fork-join halves get their own copy. The `sum`
mode also times one plain copy of the input, and a `parallelLoop` sum. The
`parallelLoop` sum may read shared data on the worker pool (`RAE_WORKERS`), so
it is the reference for what sharing gives.

With a thread per spawn, `leaves = W` is today's "W workers". The finer
leaves (64, 1024, 4096) show what splitting further costs.

`rust/` holds the same modes on **tokio** tasks, with `WORKERS` worker
threads (the fork-join modes use 1024 leaves at 1/2/4/8 workers):

- the sum shares the input through an `Arc`, which is what tokio code does;
- the sort copies its halves, like the Rae version.

**Go** is not installed on the measuring machine, so no Go version is written
yet: `run.sh` says so and skips it. When Go is available, a `go/main.go`
with the same modes is the obvious next comparison.
