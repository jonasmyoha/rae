#!/bin/sh
# benchmarks/physics_scaling: the physics playground's pyramid stepped by both
# physics tracks — 122_physics_playground_port (the Rae port of Box3D) and
# 123_physics_playground_c (Box3D's C library through Rae's ECS) — at each
# body count (default 1k to 20k) and each worker count (default 1, 2, 4, 8),
# docs/physics-performance-plan.md §10e. Sleeping is off (the pyramid stays
# awake, as in Box3D's large-pyramid benchmark); each case settles 20 fixed
# 60 Hz steps and times the next 100. Writes results/raw.csv and
# results/metadata.json and regenerates site/index.html (the devtools
# Benchmarks tab shows it).
#
#   sh benchmarks/physics_scaling/run.sh
#   COUNTS=1000,5000 WORKERS="1 8" sh benchmarks/physics_scaling/run.sh
#
# Each track is built once (`rae run`, ~100 s, keeping the binary in build/
# with RAE_RUN_KEEP_BINARY); the other worker counts rerun that binary. The pool size is per process
# (RAE_WORKERS), so every worker count is its own run. Like
# benchmarks/physics it refuses a loaded machine (BENCH_ALLOW_LOAD=1
# overrides) and records the load it ran under.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
COUNTS="${COUNTS:-1000,2000,5000,10000,20000}"
WORKERS="${WORKERS:-1 2 4 8}"
t() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }

LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores (BENCH_ALLOW_LOAD=1 overrides)." >&2
  exit 1
fi

mkdir -p "$HERE/results" "$HERE/build"
RAW="$HERE/results/raw.csv"
echo "track,bodies,workers,built,mean_ms,median_ms,awake" > "$RAW"
cd "$RAE_ROOT"
for example in 122_physics_playground_port 123_physics_playground_c; do
  binary=""
  for workers in $WORKERS; do
    if [ -z "$binary" ]; then
      # The first worker count builds the example (release) and runs it
      binary="$HERE/build/$example"
      rm -f "$binary"
      RAE_RUN_KEEP_BINARY="$binary" RAE_WORKERS=$workers RAE_PLAYGROUND_SCALING="$COUNTS" t 1800 \
        compiler/bin/rae run --profile release "examples/$example/Main.rae" > "$HERE/results/run.log" 2>&1
      [ -x "$binary" ] || { echo "no binary kept for $example (see $HERE/results/run.log)" >&2; exit 1; }
    else
      RAE_WORKERS=$workers RAE_PLAYGROUND_SCALING="$COUNTS" t 1800 "$binary" > "$HERE/results/run.log" 2>&1
    fi
    grep '^SCALING,' "$HERE/results/run.log" | sed 's/^SCALING,//' >> "$RAW"
    echo "$example, $workers workers: $(grep -c '^SCALING,' "$HERE/results/run.log") body counts"
  done
done
rm -f "$HERE/results/run.log"

COMMIT=$(git -C "$RAE_ROOT" rev-parse HEAD)
DIRTY=$(git -C "$RAE_ROOT" diff --quiet && echo clean || echo dirty)
CPU=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)
PERF=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo "?")
VERSION=$(cat "$RAE_ROOT/compiler/VERSION")
cat > "$HERE/results/metadata.json" <<JSON
{
  "date": "$(date -u +%Y-%m-%dT%H:%MZ)",
  "processor": "$CPU",
  "performance_cores": "$PERF",
  "load_average": "$LOAD",
  "rae_version": "$VERSION",
  "rae_commit": "$COMMIT",
  "rae_worktree": "$DIRTY",
  "build": "release (-O2 -DNDEBUG)",
  "steps": "20 settling + 100 timed fixed steps at 60 Hz, 4 sub-steps",
  "sleeping": "off"
}
JSON
python3 "$HERE/generate_site.py"
echo "wrote $RAW and $HERE/site/index.html"
