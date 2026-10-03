#!/usr/bin/env bash
# tsan-check.sh — run the threaded fixtures under ThreadSanitizer and fail on
# any report (docs/concurrency-model.md §5, docs/physics-performance-plan.md
# §5 item 1).
#
# The cases: every threaded fixture, plus 106_mobile_ui's real workers (below).
# Each fixture case is the fixture's own program — the C the compiler emits plus the
# runtime — built with clang -fsanitize=thread and run with RAE_MEM_STATS=1
# (so the allocation counters, which every thread bumps, are exercised too).
# A case fails if TSan reports anything, if the program exits non-zero, or if
# its output differs from the fixture's expected.txt. One line per case, a
# summary line; exit 1 on any failure. `make tsan` runs it alone; a full suite
# run (watch-tests.sh / make test) runs it as one pre-suite case.
#
# RAE_TSAN_FILTER="541_channel_worker 941_file_notify" scopes the run.
# A machine whose C compiler has no TSan runtime prints SKIP and passes.
set -u
cd "$(dirname "$0")/.." || exit 1     # -> compiler/
RAE="${RAE_BIN:-bin/rae}"
[ -x "$RAE" ] || { echo "error: $RAE not built (make -C compiler build)" >&2; exit 1; }
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
CC="${RAE_TSAN_CC:-clang}"
TIMEOUT_S="${RAE_TSAN_TIMEOUT:-120}"
TMP="$(mktemp -d -t rae_tsan_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# Every fixture that starts a thread: spawn'd workers (one pthread each),
# Channel producers, the file-notification thread, the parallelLoop worker pool.
CASES="511_spawn_raytracer_bands 512_spawn_string_workers 513_spawn_own_list_copy \
541_channel_worker 646_list_of_tasks 848_channel_struct_payload 941_file_notify \
944_parallel_loop_results"
if [ -n "${RAE_TSAN_FILTER:-}" ]; then CASES="$RAE_TSAN_FILTER"; fi

# The toolchain probe: a compiler without the TSan runtime cannot run this gate.
printf 'int main(void){return 0;}\n' > "$TMP/probe.c"
if ! "$CC" -fsanitize=thread "$TMP/probe.c" -o "$TMP/probe" >/dev/null 2>&1; then
  echo "SKIP: tsan-check ($CC has no -fsanitize=thread runtime)"
  exit 0
fi

FRAMEWORKS=""
if [ "$(uname)" = "Darwin" ]; then
  FRAMEWORKS="-framework Foundation -framework ImageIO -framework CoreGraphics"
fi

passed=0; failed=0
for name in $CASES; do
  dir="tests/cases/$name"
  work="$TMP/$name"
  mkdir -p "$work"
  if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target compiled --emit-c \
      --project "$dir" --out "$work/out.c" "$dir/Main.rae" > "$work/emit.log" 2>&1; then
    echo "FAIL: tsan $name (emit failed)"; sed 's/^/    /' "$work/emit.log" | tail -5
    failed=$((failed + 1)); continue
  fi
  # -O1 -g: TSan's recommended level; -fno-omit-frame-pointer for its stacks.
  # shellcheck disable=SC2086
  if ! perl -e 'alarm shift; exec @ARGV' 300 "$CC" -std=c11 -O1 -g -fno-omit-frame-pointer \
      -fsanitize=thread -ffp-contract=off -w "$work/out.c" "$work/rae_runtime.c" \
      -I"$work" -I/opt/homebrew/include $FRAMEWORKS -lm -lpthread \
      -o "$work/app" > "$work/cc.log" 2>&1; then
    echo "FAIL: tsan $name (TSan build failed)"; sed 's/^/    /' "$work/cc.log" | tail -5
    failed=$((failed + 1)); continue
  fi
  (cd "$dir" && RAE_MEM_STATS=1 TSAN_OPTIONS="halt_on_error=0 exitcode=66 second_deadlock_stack=1" \
    perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" "$work/app") > "$work/stdout" 2> "$work/stderr"
  rc=$?
  if grep -q "ThreadSanitizer" "$work/stderr"; then
    reports=$(grep -c "WARNING: ThreadSanitizer" "$work/stderr")
    echo "FAIL: tsan $name ($reports TSan report(s))"
    grep -A12 "WARNING: ThreadSanitizer" "$work/stderr" | head -40 | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  if [ "$rc" != "0" ]; then
    echo "FAIL: tsan $name (exit $rc)"; tail -5 "$work/stderr" | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  # Trailing blank lines differ between `rae run` (what expected.txt holds)
  # and the bare binary; nothing else may.
  trim() { grep -v '^@@RAE_' "$1" | perl -0pe 's/\n+\z/\n/'; }
  if [ -f "$dir/expected.txt" ] && ! diff -q <(trim "$work/stdout") <(trim "$dir/expected.txt") >/dev/null; then
    echo "FAIL: tsan $name (output differs from expected.txt)"
    diff <(trim "$work/stdout") <(trim "$dir/expected.txt") | head -10 | sed 's/^/    /'
    failed=$((failed + 1)); continue
  fi
  passed=$((passed + 1))
done

# 106_mobile_ui's real workers (the Spotify poller thread, the artwork fetch
# workers, EventLoop.wake from both): the app built like the example gate
# (SDL3 + WebGPU) plus TSan, run headless for 6 s with Spotify ON so the poller
# runs. Skipped where SDL3 or wgpu-native is missing; excluded by a filter.
APP106=""
WGPU="${WGPU_NATIVE:-$HOME/.local/wgpu-native}"
if [ -z "${RAE_TSAN_FILTER:-}" ] || echo " $RAE_TSAN_FILTER " | grep -q " 106_mobile_ui "; then
  if [ -f "$WGPU/lib/libwgpu_native.dylib" ] && [ -f /opt/homebrew/lib/libSDL3.dylib ]; then
    APP106=1
  else
    echo "SKIP: tsan 106_mobile_ui (needs SDL3 and wgpu-native)"
  fi
fi
if [ -n "$APP106" ]; then
  work="$TMP/106_mobile_ui"; mkdir -p "$work"
  if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target compiled --emit-c \
      --project ../examples/106_mobile_ui --out "$work/out.c" ../examples/106_mobile_ui/Main.rae \
      > "$work/emit.log" 2>&1; then
    echo "FAIL: tsan 106_mobile_ui (emit failed)"; failed=$((failed + 1))
  elif ! perl -e 'alarm shift; exec @ARGV' 600 "$CC" -std=c11 -O1 -g -fno-omit-frame-pointer \
      -fsanitize=thread -ffp-contract=off -w -o "$work/app" "$work/out.c" "$work/rae_runtime.c" \
      $([ -f "$work/monocypher.c" ] && echo "$work/monocypher.c") \
      -I"$work" -I/opt/homebrew/include -L/opt/homebrew/lib -DRAE_HAS_SDL3 -DRAE_HAS_WEBGPU \
      -I"$WGPU/include" -L"$WGPU/lib" -lwgpu_native -Wl,-rpath,"$WGPU/lib" \
      -framework Metal -framework QuartzCore -lSDL3 $FRAMEWORKS > "$work/cc.log" 2>&1; then
    echo "FAIL: tsan 106_mobile_ui (TSan build failed)"; tail -5 "$work/cc.log" | sed 's/^/    /'
    failed=$((failed + 1))
  else
    (cd .. && RAE_UI_SCREEN=home RAE_AUTO_EXIT_SEC=6 RAE_SDL_HEADLESS_MS=6000 RAE_MEM_STATS=1 \
      TSAN_OPTIONS="halt_on_error=0 exitcode=66" \
      perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" "$work/app") > "$work/stdout" 2> "$work/stderr"
    rc=$?
    if grep -q "ThreadSanitizer" "$work/stderr"; then
      echo "FAIL: tsan 106_mobile_ui ($(grep -c 'WARNING: ThreadSanitizer' "$work/stderr") TSan report(s))"
      grep -A12 "WARNING: ThreadSanitizer" "$work/stderr" | head -40 | sed 's/^/    /'
      failed=$((failed + 1))
    elif [ "$rc" != "0" ]; then
      echo "FAIL: tsan 106_mobile_ui (exit $rc)"; tail -5 "$work/stderr" | sed 's/^/    /'
      failed=$((failed + 1))
    else
      passed=$((passed + 1))
    fi
  fi
fi

if [ "$failed" = "0" ]; then
  echo "PASS: tsan-check ($passed threaded programs, no ThreadSanitizer report)"
  exit 0
fi
echo "FAIL: tsan-check ($failed of $((passed + failed)) threaded programs)"
exit 1
