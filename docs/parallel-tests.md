# Running the unit cases in parallel (#824)

`compiler/tools/run_tests_parallel.sh` is a prototype that runs the 450 unit
cases N at a time. It does NOT re-implement any test logic: each case is one
`tools/run_tests.sh <case>` invocation (same discovery, same `config.cmd`
handling, same `expected.txt` comparison), writing its own log; the verdicts
are aggregated afterwards. The example smoke tests (`run_examples.sh`) still
run sequentially after the cases — they open real SDL/GPU windows and take
screenshots, and were never the target of this.

```bash
bash compiler/tools/run_tests_parallel.sh                 # JOBS = CPU count
JOBS=4 RAE_SKIP_EXAMPLES=1 bash compiler/tools/run_tests_parallel.sh
```

## Measured (10-core Apple Silicon, cases only, `RAE_SKIP_EXAMPLES=1`)

| runner | jobs | wall | speed-up |
|---|---|---|---|
| `run_tests.sh` (sequential) | 1 | 571 s | 1.0× |
| `run_tests_parallel.sh` | 4 | 449 s | 1.3× |
| `run_tests_parallel.sh` | 10 | 215 s | 2.7× |

Three consecutive 10-job runs produced byte-identical verdict sets, and the
4-job run the same set again — no flaky case surfaced in four parallel runs.
After the two runner fixes below, three further 10-job runs matched the
sequential verdict set EXACTLY (411 pass / 0 fail / 39 skipped, zero diff),
but took 459–503 s: they ran while other work had the machine at a load
average of 51–73 on 10 cores (a single case cost 7 s instead of 4 s), so the
215 s figure from the quiet-machine batch is the one to quote. Parallel
timings are only meaningful on an otherwise idle box.
A 20-job (over-subscribed) run was attempted twice but both attempts were
killed part-way by a concurrent agent's `pkill -f run_tests.sh` (see
"Reliability" below), so its number is not reported; expect little beyond the
10-job figure, since every case is one gcc of the runtime and is CPU-bound.

Why only 2.7× on 10 cores: each case pays a fixed ~3–4 s (bash startup + one
`rae run` that gcc-compiles the runtime), and the tail is dominated by the few
long cases, so wall time approaches (longest cases) + (startup skew), not
`571 / 10`. The next real win is not more jobs but caching the compiled
runtime object across cases — that is a compiler/build change, not a runner
change.

## Why parallel cases are safe (what was checked)

* `rae run` writes `$TMPDIR/rae_compiled_<pid>.{c,bin}` — pid-unique.
* A case's build cache lives in its own directory (`tests/cases/<x>/.rae/`).
* The ONE shared write was `stats/test_history.json` (plus a `git log` per
  passed test) at the end of every `run_tests.sh` call. With 450 concurrent
  writers that would race and dominate the clock, so `RAE_TEST_NO_HISTORY=1`
  now skips it per case and the parallel runner updates history once at the end.
* `run_tests.sh <name>` deliberately bypasses the compiled-target skip list so
  a developer can force-run one skipped case. Driven per case, that made the
  parallel runner "run" 33 deprecated Live-only cases the full loop never runs
  (they showed up as 18 extra passes + 15 fails). `RAE_TEST_APPLY_SKIPS=1`
  re-applies the list under a filter; the parallel runner sets it, so its
  verdict set is exactly the sequential one (411 pass / 0 fail).

## Two runner bugs found on the way (both in the runner, not the tests)

* **BSD `xargs -I` has a 255-byte limit** on the argument it substitutes into.
  A per-case `bash -c '<script>'` body longer than that makes macOS xargs stop
  with "command line cannot be assembled, too long" after the first few
  short-named cases — every remaining case then reads as "runner died". The
  runner passes the case name as `$1` (`xargs -n 1`) instead of `-I{}`.
* **A name-filtered `run_tests.sh` bypasses the compiled-target skip list**
  (see above), which is right for a human force-running one case and wrong for
  a driver — hence `RAE_TEST_APPLY_SKIPS=1`.

## Reliability — the real hazard is not the tests

Nothing in the cases themselves proved order- or timing-sensitive. The one
thing that DID break runs is the documented kill-before-run incantation
(`pkill -9 -f 'run_tests.sh'` in AGENTS.md): it matches the per-case
`bash tools/run_tests.sh <name>` children of a parallel run, so any agent or
human starting a suite while a parallel run is in flight kills it mid-way
(seen as runs dying at random points with "runner died" verdicts). This is the
same "only ONE test run at a time" rule as before — parallelism does not
relax it, it makes violating it more visible.

## Status

Wired in, opt-in at the CLI and default-on in devtools:

* `RAE_TEST_PARALLEL=1 make test` routes `make test` (and so
  `tools/watch-tests.sh`) through the parallel runner. Without the variable,
  `make test` is the sequential reference runner, unchanged. `make test
  TEST=<case>` always runs sequentially — the parallel script delegates a name
  argument straight to `run_tests.sh`, since one case has no parallel form.
* The devtools Test Runner has a "Parallel cases (fast)" toggle next to the
  examples toggle. It is ON by default (an absent localStorage key means on; only
  an explicit untick is remembered), and the server sets `RAE_TEST_PARALLEL=1`
  for a full run when it is on. Single-test runs ignore it. Untick it while
  something else is compiling — parallel work on a saturated machine only
  spreads the same wall time thinner (see the 459–503 s runs above).
* The sequential runner stays the reference the parallel one is diffed against.
  The remaining win is the fixed per-case cost (one gcc of the runtime), which
  is a build change, not a runner change.
