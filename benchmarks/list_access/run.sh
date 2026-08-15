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

echo "Building Rae compiler..."
run_with_timeout 300 make -C "$RAE_ROOT/compiler" build >/dev/null

echo "Compiling benchmarks..."
run_with_timeout 300 "$RAE_BIN" build --target compiled --profile release --emit-c \
  --out "$BUILD/rae_generated.c" "$HERE/rae/main.rae"
run_with_timeout 300 cc -std=c11 -O2 -DNDEBUG \
  -include "$HERE/c/opaque_index.h" "$BUILD/rae_generated.c" \
  "$HERE/c/opaque_index.c" \
  "$RAE_ROOT/compiler/runtime/rae_runtime.c" \
  -I"$RAE_ROOT/compiler/runtime" -I/opt/homebrew/include \
  /opt/homebrew/lib/libraylib.a -framework CoreVideo -framework IOKit \
  -framework Cocoa -framework OpenGL -framework ImageIO -framework CoreGraphics \
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
