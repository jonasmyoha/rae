#!/bin/sh
# The server benchmark runner (docs/server-benchmarks-design.md §8), phase 1
# HTTP. `make bench-servers` calls it.
#
# For each worker configuration (1, and all performance cores) it starts every
# implementation, runs check.sh against it (a failing check skips its timing),
# then for each case warms up and makes BENCH_RUNS timed runs, interleaving
# the implementations and alternating the two Rae styles' order every run.
# Cases: plaintext and json with oha; pipelined plaintext (16 deep) with wrk.
# Raw runs go to build/raw/, the medians to results/summary.json and the
# machine and toolchains to results/metadata.json (committed baseline, F6).
#
# Knobs: BENCH_SECONDS (15), BENCH_RUNS (5), BENCH_WARMUP (5),
# BENCH_CONNECTIONS (256), BENCH_LOADGEN_THREADS (4), BENCH_IMPLS
# ("rae-eventLoop rae-ecs rust node bun"), BENCH_CASES ("plaintext json
# pipelined"), BENCH_WORKERS ("1 <performance cores>"), BENCH_ALLOW_LOAD=1
# (skip the load-average gate), BENCH_RESULTS (results/; a scratch run can
# point elsewhere so the committed baseline stays).
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
CACHE=${RAE_SERVERS_CACHE:-$HOME/.cache/rae/servers}

# ----- dependencies -----
missing=0
need() {
  if ! command -v "$1" >/dev/null 2>&1 && [ ! -x "$1" ]; then
    echo "missing: $1 ($2)" >&2
    missing=1
  fi
}
need cargo "install Rust: https://rustup.rs"
need node "brew install node"
need bun "brew install oven-sh/bun/bun"
need python3 "xcode-select --install"
need curl "xcode-select --install"
need "$CACHE/bin/oha" "sh benchmarks/servers/fetch.sh"
[ "$missing" = 0 ] || exit 1
if [ ! -x "$CACHE/bin/wrk" ]; then
  echo "note: wrk is not built (sh benchmarks/servers/fetch.sh): the pipelined case is skipped" >&2
fi

# ----- the load-average gate -----
LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores." >&2
  echo "Wait for the machine to go idle, or set BENCH_ALLOW_LOAD=1 to override." >&2
  exit 1
fi

# ----- build -----
echo "building the servers ..."
run_with_timeout 300 make -C "$RAE_ROOT/compiler" build >/dev/null
build_rae eventLoop
build_rae ecs
run_with_timeout 900 cargo build --quiet --release --locked \
  --manifest-path "$HERE/rust/Cargo.toml" --target-dir "$BUILD/rust"

PERFORMANCE_CORES=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo "$CORES")
export HERE BUILD CACHE LOAD PERFORMANCE_CORES RAE_BIN RAE_ROOT
run_with_timeout 14400 python3 -u "$HERE/measure.py"
