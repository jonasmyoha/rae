#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
RESULTS="$HERE/results"
RAE_BIN="$RAE_ROOT/compiler/bin/rae"
mkdir -p "$BUILD" "$RESULTS" "$HERE/site"

run_with_timeout() {
  seconds=$1
  shift
  perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"
}

# A benchmark on a busy machine measures contention, not performance. This has
# already produced one committed set of numbers where EVERY language, including
# the untouched C and Rust controls, looked 3-250% slower than the run before —
# the machine was at load 61 under an unrelated test suite. Refuse by default;
# BENCH_ALLOW_LOAD=1 overrides when you know what you are doing.
LOAD=$(uptime | sed 's/.*load averages*:[ ]*//' | awk '{print $1}')
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if [ "${BENCH_ALLOW_LOAD:-0}" != "1" ] && awk "BEGIN{exit !($LOAD > $CORES * 0.3)}"; then
  echo "refusing to benchmark: load average $LOAD on $CORES cores." >&2
  echo "Wait for the machine to go idle, or set BENCH_ALLOW_LOAD=1 to override." >&2
  exit 1
fi

echo "Building Rae compiler..."
run_with_timeout 300 make -C "$RAE_ROOT/compiler" build >/dev/null

echo "Compiling benchmarks..."
run_with_timeout 300 "$RAE_BIN" build --target compiled --profile release --emit-c \
  --out "$BUILD/rae_generated.c" "$HERE/rae/Main.rae"
# This benchmark is pure compute — no window, no GPU — so it links only what
# the Rae runtime itself needs. (It used to link raylib and the GL/Cocoa stack
# from the pre-SDL3 era, which both failed on a machine without raylib and
# pulled a renderer into a List-indexing measurement.) The runtime's Objective-C
# helpers are why Foundation is still required.
run_with_timeout 300 cc -std=c11 -O2 -DNDEBUG \
  -include "$HERE/c/opaque_index.h" "$BUILD/rae_generated.c" \
  "$HERE/c/opaque_index.c" \
  "$BUILD/rae_runtime.c" \
  -I"$BUILD" \
  -framework Foundation -framework ImageIO -framework CoreGraphics \
  -o "$BUILD/rae_list_access"
run_with_timeout 120 cc -std=c11 -O3 -DNDEBUG "$HERE/c/list_access.c" -o "$BUILD/c_list_access"
run_with_timeout 180 rustc -C opt-level=3 -C debuginfo=0 "$HERE/rust/list_access.rs" \
  -o "$BUILD/rust_list_access"

echo "Running benchmarks..."
: > "$RESULTS/raw.csv"
printf 'language,scenario,elapsed_ns,checksum\n' >> "$RESULTS/raw.csv"
for executable in "$BUILD/rae_list_access" "$BUILD/c_list_access" "$BUILD/rust_list_access"; do
  run_with_timeout 300 "$executable" | sed -n 's/^RESULT,//p' >> "$RESULTS/raw.csv"
done
run_with_timeout 300 node "$HERE/javascript/list_access.js" \
  | sed -n 's/^RESULT,//p' >> "$RESULTS/raw.csv"
run_with_timeout 600 python3 "$HERE/python/list_access.py" \
  | sed -n 's/^RESULT,//p' >> "$RESULTS/raw.csv"

python3 - "$RAE_ROOT" "$RESULTS/metadata.json" <<'PY'
import json, os, platform, subprocess, sys
root, output = sys.argv[1:]
def command(*args):
    try: return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()
    except Exception as error: return f"unavailable: {error}"
metadata = {
    "platform": platform.platform(),
    "machine": platform.machine(),
    "processor": (command("sysctl", "-n", "machdep.cpu.brand_string")
                  if platform.system() == "Darwin" else platform.processor()),
    "rae_commit": command("git", "-C", root, "rev-parse", "HEAD"),
    "rae_worktree": "dirty" if command("git", "-C", root, "status", "--porcelain") else "clean",
    "c_compiler": command("cc", "--version").splitlines()[0],
    "rust_compiler": command("rustc", "--version"),
    "javascript_runtime": command("node", "--version"),
    "python_runtime": command("python3", "--version"),
    "optimization": {
        "rae": "release (-O2 -DNDEBUG)",
        "c": "-O3 -DNDEBUG",
        "rust": "-C opt-level=3",
        "javascript": "Node default optimizing JIT",
        "python": "CPython default interpreter"
    },
    "data_size": 65536,
    "passes": 128,
    "measured_repetitions": 7,
    "discarded_warmups": 2
}
with open(output, "w") as stream: json.dump(metadata, stream, indent=2)
PY

python3 "$HERE/generate_site.py"
echo "Results: $HERE/site/index.html"
