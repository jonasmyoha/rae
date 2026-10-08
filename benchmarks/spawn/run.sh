#!/bin/sh
# Today's thread-per-spawn, measured (docs/lightweight-spawn-design.md §8 step
# 1), next to tokio tasks (and Go goroutines, when a Go toolchain is present:
# not yet written, see README). Writes results/raw.csv, results/summary.json
# and results/metadata.json. Every benchmark runs in its own process under
# /usr/bin/time -l, which gives its peak RSS.
#
# BENCH_ALLOW_LOAD=1 skips the load-average gate. BENCH_QUICK=1 uses small
# sizes (a smoke run; the results are not a baseline).
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
RESULTS="${BENCH_RESULTS:-$HERE/results}"
RAE_BIN="$RAE_ROOT/compiler/bin/rae"
mkdir -p "$BUILD" "$RESULTS"

run_with_timeout() {
  seconds=$1
  shift
  perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"
}

LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores." >&2
  echo "Wait for the machine to go idle, or set BENCH_ALLOW_LOAD=1 to override." >&2
  exit 1
fi

echo "building ..."
run_with_timeout 300 make -C "$RAE_ROOT/compiler" build >/dev/null
mkdir -p "$BUILD/rae"
run_with_timeout 300 "$RAE_BIN" build --profile release --emit-c --out "$BUILD/rae/spawn.c" \
  --entry "$HERE/rae/Main.rae" >"$BUILD/rae/build.log" 2>&1 || { cat "$BUILD/rae/build.log"; exit 1; }
run_with_timeout 300 cc -std=c11 -O2 -DNDEBUG "$BUILD/rae/spawn.c" "$BUILD/rae/rae_runtime.c" -I"$BUILD/rae" \
  -framework Foundation -framework ImageIO -framework CoreGraphics -o "$BUILD/rae/spawn"
TOKIO=""
if command -v cargo >/dev/null 2>&1; then
  run_with_timeout 900 cargo build --quiet --release --locked --manifest-path "$HERE/rust/Cargo.toml" \
    --target-dir "$BUILD/rust"
  TOKIO="$BUILD/rust/release/spawn"
else
  echo "tokio: skipped, cargo is not installed (https://rustup.rs)"
fi
if command -v go >/dev/null 2>&1; then
  echo "go: a toolchain is present, but the Go version is not written yet (README): skipped"
else
  echo "go: skipped, not installed (brew install go)"
fi

if [ "${BENCH_QUICK:-0}" = "1" ]; then
  SPAWNS=10000; SUM=1000000; SORT=200000; SAMPLES=2000
  RAE_SLEEPS="1000 2000"; TOKIO_SLEEPS="1000 10000"; RAE_HELD="1000"
else
  SPAWNS=100000; SUM=16000000; SORT=2000000; SAMPLES=10000
  RAE_SLEEPS="1000 2000 5000 10000 15000 20000 50000"; TOKIO_SLEEPS="10000 100000 1000000"
  RAE_HELD="10000 15000 16000 17000 20000 30000"
fi
PERFORMANCE_CORES=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo "$CORES")
RAW="$RESULTS/raw.csv"
printf 'config,implementation,benchmark,parameter,metric,value\n' > "$RAW"

# measure <config> <implementation> <benchmark> <parameter> <timeout>
# <command...>: its RESULT lines
# and its peak RSS (rssMb) into raw.csv. Non-zero on a failed run, whose exit
# status and last output lines go to raw.csv and the console.
measure() {
  config=$1; implementation=$2; benchmark=$3; parameter=$4; seconds=$5
  shift 5
  if run_with_timeout "$seconds" /usr/bin/time -l "$@" >"$BUILD/out.txt" 2>"$BUILD/time.txt"; then
    status=0
  else
    status=$?
  fi
  sed -n "s/^RESULT,/$config,/p" "$BUILD/out.txt" >> "$RAW"
  rss=$(awk '/maximum resident set size/ {print $1}' "$BUILD/time.txt")
  if [ -n "$rss" ]; then
    printf '%s,%s,%s,%s,rssMb,%s\n' "$config" "$implementation" "$benchmark" "$parameter" \
      "$(awk "BEGIN{print $rss / 1048576}")" >> "$RAW"
  fi
  if [ "$status" != 0 ]; then
    reason=$(grep -v "^RESULT" "$BUILD/out.txt" "$BUILD/time.txt" 2>/dev/null | grep -iv "resident\|page\|context\|signals\|messages\|block\|swaps\|instructions\|cycles\|footprint\|real\|user\|sys\|^ *[0-9]" | tail -2 | tr ',\n' '; ' | cut -c1-200)
    printf '%s,%s,%s,%s,failed,%s\n' "$config" "$implementation" "$benchmark" "$parameter" "exit $status: $reason" >> "$RAW"
    echo "  $implementation $benchmark $parameter ($config): FAILED (exit $status) $reason"
    return 1
  fi
  echo "  $implementation $benchmark $parameter ($config): ok"
}

echo "spawn and join $SPAWNS tasks"
measure default rae spawnJoin "$SPAWNS" 300 "$BUILD/rae/spawn" spawnJoin "$SPAWNS" || true
[ -z "$TOKIO" ] || measure "workers=$PERFORMANCE_CORES" tokio spawnJoin "$SPAWNS" 300 env WORKERS="$PERFORMANCE_CORES" "$TOKIO" spawnJoin "$SPAWNS" || true

echo "concurrent sleep(ms: 100) tasks (stops at the first count that fails)"
for count in $RAE_SLEEPS; do
  measure default rae sleeps "$count" 120 "$BUILD/rae/spawn" sleeps "$count" || break
done
if [ -n "$TOKIO" ]; then
  for count in $TOKIO_SLEEPS; do
    measure "workers=$PERFORMANCE_CORES" tokio sleeps "$count" 120 env WORKERS="$PERFORMANCE_CORES" "$TOKIO" sleeps "$count" || break
  done
fi

echo "tasks all alive at once (each waits for a deadline after the last spawn)"
for count in $RAE_HELD; do
  measure default rae held "$count" 300 "$BUILD/rae/spawn" held "$count" || break
done

echo "spawn/get latency, $SAMPLES samples"
measure default rae latency "$SAMPLES" 300 "$BUILD/rae/spawn" latency "$SAMPLES" || true
[ -z "$TOKIO" ] || measure "workers=$PERFORMANCE_CORES" tokio latency "$SAMPLES" 300 env WORKERS="$PERFORMANCE_CORES" "$TOKIO" latency "$SAMPLES" || true

# Fork-join. Rae today: a thread per split, so `leaves` leaves run on that
# many threads, and W leaves is today's "W workers". Then finer leaves, which
# a lightweight spawn should make free. tokio: fine leaves (1024) on a pool of
# 1/2/4/8 workers. parallelLoop (the Rae worker pool, shared data) is the
# reference for what sharing gives, at RAE_WORKERS=1/2/4/8.
for benchmark in sum sort; do
  if [ "$benchmark" = sum ]; then count=$SUM; else count=$SORT; fi
  echo "fork-join $benchmark of $count"
  for leaves in 1 2 4 8 64 1024 4096; do
    measure "leaves=$leaves" rae "$benchmark" $((count / leaves)) 600 env RAE_WORKERS=8 "$BUILD/rae/spawn" "$benchmark" "$count" $((count / leaves)) || true
  done
  if [ "$benchmark" = sum ]; then
    for workers in 1 2 4 8; do
      measure "poolWorkers=$workers" rae sum "$count" 600 env RAE_WORKERS="$workers" "$BUILD/rae/spawn" sum "$count" "$count" || true
    done
  fi
  if [ -n "$TOKIO" ]; then
    for workers in 1 2 4 8; do
      measure "workers=$workers" tokio "$benchmark" $((count / 1024)) 600 env WORKERS="$workers" "$TOKIO" "$benchmark" "$count" $((count / 1024)) || true
    done
  fi
done

python3 - "$RESULTS" "$RAE_ROOT" "$LOAD" "$PERFORMANCE_CORES" "$HERE" <<'PYTHON'
import csv, json, os, platform, subprocess, sys
results, root, load, performance_cores, here = sys.argv[1:]

def command(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip().splitlines()[0]
    except Exception:
        return None

rows = list(csv.DictReader(open(os.path.join(results, "raw.csv"))))
summary = {}
for row in rows:
    key = "%s/%s/%s/%s" % (row["implementation"], row["benchmark"], row["config"], row["parameter"])
    value = row["value"]
    try:
        value = float(value)
    except ValueError:
        pass
    summary.setdefault(key, {})[row["metric"]] = value
for numbers in summary.values():
    if isinstance(numbers.get("sequentialMs"), float) and isinstance(numbers.get("forkJoinMs"), float):
        numbers["forkJoinSpeedup"] = numbers["sequentialMs"] / numbers["forkJoinMs"]
    if isinstance(numbers.get("sequentialMs"), float) and isinstance(numbers.get("parallelLoopMs"), float):
        numbers["parallelLoopSpeedup"] = numbers["sequentialMs"] / numbers["parallelLoopMs"]
with open(os.path.join(results, "summary.json"), "w") as out:
    json.dump(summary, out, indent=2, sort_keys=True)
    out.write("\n")
metadata = {
    "machine": command("sysctl", "-n", "machdep.cpu.brand_string"),
    "cores": os.cpu_count(),
    "performanceCores": int(performance_cores),
    "os": platform.platform(),
    "loadAverageBefore": float(load),
    "raeCommit": command("git", "-C", root, "rev-parse", "HEAD"),
    "raeVersion": command(os.path.join(root, "compiler", "bin", "rae"), "--version"),
    "cc": command("cc", "--version"),
    "rustc": command("rustc", "--version"),
    "go": command("go", "version"),
    "threadLimits": {
        "kern.num_taskthreads": command("sysctl", "-n", "kern.num_taskthreads"),
        "kern.num_threads": command("sysctl", "-n", "kern.num_threads"),
    },
}
with open(os.path.join(results, "metadata.json"), "w") as out:
    json.dump(metadata, out, indent=2)
    out.write("\n")
print("wrote %s/summary.json" % results)
PYTHON
