#!/bin/bash
# Rae PARALLEL test runner (#824).
#
# Runs every unit case through the official per-case protocol of
# tools/run_tests.sh (same discovery, config.cmd handling, expected.txt
# comparison and skip list — this script never re-implements a comparison),
# but JOBS cases at a time. Each case is one `run_tests.sh <name>` invocation
# writing its own log; verdicts are echoed as they land and counted at the end,
# so the interleaving that makes a naive `make test &` unreliable never happens.
#
# Why parallel CASES are safe:
#   * `rae run` builds in its OWN scratch dir ($TMPDIR/rae_run_XXXXXX: the
#     generated C, the binary AND the runtime copies the C includes). It used
#     to write rae_compiled_<pid>.{c,bin} into $TMPDIR itself, which looked
#     pid-unique - but the runtime headers were copied next to them, shared by
#     every concurrent run, and a run could read a half-rewritten
#     rae_runtime.h (~1 in 100 under 20-way concurrency).
#   * a case's build cache lives in ITS OWN dir (tests/cases/<x>/.rae/).
#   * the one shared write — stats/test_history.json (+ a `git log` per passed
#     test) at the end of every run_tests.sh call — is disabled per case
#     (RAE_TEST_NO_HISTORY=1) and done ONCE here at the end.
#   * RAE_TEST_APPLY_SKIPS=1 makes the filtered per-case run skip EXACTLY what
#     the full loop skips (deprecated Live-only / hot-reload cases), so the
#     verdict set is identical to the sequential run.
# NOT part of the default run: the example smoke tests (run_examples.sh) open
# real SDL/GPU windows and take screenshots, and take minutes. They are OFF
# unless RAE_RUN_EXAMPLES=1 is exported (`make test-examples` does that); the
# default `make test` is the non-visual unit suite only. RAE_SKIP_EXAMPLES=1
# is still accepted and always wins.
#
# Usage:  bash tools/watch-tests.sh                        # visible in devtools
#         bash tools/run_tests_parallel.sh                  # direct; JOBS = CPU count
#         RAE_RUN_EXAMPLES=1 bash tools/run_tests_parallel.sh   # + the visual example gates
# Exit code: 1 if any case (or example) failed.
set -u
cd "$(dirname "$0")/.."
# `make test TEST=<case>` passes the case name as $1. One case has no parallel
# form: delegate straight to the sequential runner (it also skips the examples
# for a single-name run), so the Makefile's RAE_TEST_PARALLEL switch is safe to
# leave on for single-test runs too.
if [ -n "${1:-}" ]; then exec bash tools/run_tests.sh "$1"; fi
BIN="bin/rae"; TEST_DIR="tests/cases"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
[ -f "$BIN" ] || { echo "Error: $BIN not found. Run 'make build' first."; exit 1; }

OUT=$(mktemp -d -t rae_ptest_XXXXXX); export OUT
NAMES=$(find "$TEST_DIR" \( -name "Main.rae" -o -name "main.rae" \) | sort | while read -r f; do
  d=$(dirname "$f"); [[ "$d" == */helpers/* ]] && continue
  [ -f "$d/.raetesthelper" ] && continue
  basename "$d"
done)
TOTAL=$(printf '%s\n' "$NAMES" | wc -l | tr -d ' ')
# #919: the whole-tree format check comes BEFORE the suite (one extra "case").
TREE_CHECK=1
if [ -f tools/format-check-tree.sh ]; then
  if bash tools/format-check-tree.sh; then TREE_CHECK=0; fi
  echo
fi
# #1016: the banned-terms gate is one more pre-suite "case" (SKIP counts as a
# pass: a machine without the term list is not a failure).
TERMS_CHECK=1
if [ -f tools/banned-terms-gate.sh ]; then
  if bash tools/banned-terms-gate.sh; then TERMS_CHECK=0; fi
  echo
fi
# The stress cases (docs/stress-tests.md): one more pre-suite "case". A
# "footgun" case that stops reproducing FAILS here on purpose (the ratchet).
STRESS_CHECK=1
if [ -f ../stress/run.sh ]; then
  if bash ../stress/run.sh; then STRESS_CHECK=0; fi
  echo
fi
# The threaded fixtures under ThreadSanitizer (tools/tsan-check.sh): one more
# pre-suite "case"; any TSan report fails it (SKIP without a TSan runtime).
TSAN_CHECK=1
if [ -f tools/tsan-check.sh ]; then
  if bash tools/tsan-check.sh; then TSAN_CHECK=0; fi
  echo
fi
# Every benchmark program still compiles (tools/benchmarks-compile-check.sh):
# one more pre-suite "case"; their run.sh builds are not run on every change.
BENCH_CHECK=1
if [ -f tools/benchmarks-compile-check.sh ]; then
  if bash tools/benchmarks-compile-check.sh; then BENCH_CHECK=0; fi
  echo
fi
# The memory-boundary fixtures under AddressSanitizer (tools/asan-check.sh):
# one more pre-suite "case" (SKIP without an ASan runtime).
ASAN_CHECK=1
if [ -f tools/asan-check.sh ]; then
  if bash tools/asan-check.sh; then ASAN_CHECK=0; fi
  echo
fi
# lib/'s `when` branches type-checked for every real target
# (tools/check-targets.sh, docs/platform-conditional-code.md §3.4).
TARGETS_CHECK=1
if [ -f tools/check-targets.sh ]; then
  if bash tools/check-targets.sh; then TARGETS_CHECK=0; fi
  echo
fi
# The generated C compiles without warnings (tools/c-warnings-gate.sh).
WARNINGS_CHECK=1
if [ -f tools/c-warnings-gate.sh ]; then
  if bash tools/c-warnings-gate.sh; then WARNINGS_CHECK=0; fi
  echo
fi
# Both Rae HTTP benchmark servers against every spec/Http.md rule
# (benchmarks/servers/check.sh --rae, decision F7): correctness, never speed.
SERVER_CHECK=1
if [ -f ../benchmarks/servers/check.sh ]; then
  if sh ../benchmarks/servers/check.sh --rae; then SERVER_CHECK=0; fi
  echo
fi
echo "Running Rae tests (PARALLEL: $TOTAL cases, $JOBS jobs)..."
echo
START=$(date +%s)

# One case per slot. Each finished case echoes its verdict line at once (one
# short line — atomic on a pipe), so watch-tests.sh / the devtools Test Runner
# show live progress; FAIL details are printed in the summary pass below.
# The name is passed as $1, NOT via xargs -I{}: BSD xargs caps an -I
# substituted argument at 255 bytes, so a command mentioning {} several times
# silently fails for every case whose name is not very short.
printf '%s\n' "$NAMES" | xargs -P "$JOBS" -n 1 bash -c '
  n="$1"
  RAE_TEST_NO_HISTORY=1 RAE_TEST_APPLY_SKIPS=1 bash tools/run_tests.sh "$n" > "$OUT/$n.log" 2>&1 </dev/null
  echo $? > "$OUT/$n.rc"
  grep -m1 -E "^(PASS|FAIL|SKIP): " "$OUT/$n.log" || true' _

END=$(date +%s)
PASSED=0; FAILED=0; SKIPPED=0; PASSED_NAMES=""
for n in $NAMES; do
  log="$OUT/$n.log"; rc=$(cat "$OUT/$n.rc" 2>/dev/null || echo 1)
  if grep -qE "^PASS: " "$log"; then PASSED=$((PASSED+1)); PASSED_NAMES="$PASSED_NAMES $n.rae"
  elif grep -qE "^FAIL: " "$log"; then FAILED=$((FAILED+1)); echo; sed -n '/^FAIL: /,/^$/p' "$log"
  elif [ "$rc" = "0" ]; then SKIPPED=$((SKIPPED+1))   # no verdict + clean exit: the full loop skips it too
  else FAILED=$((FAILED+1)); echo; echo "FAIL: $n (runner died, rc=$rc)"; tail -5 "$log" | sed 's/^/    /'
  fi
done

if [ "$TREE_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$TERMS_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$STRESS_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$TSAN_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$ASAN_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$BENCH_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$SERVER_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$TARGETS_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
if [ "$WARNINGS_CHECK" = "0" ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
# #917: the format-CLI behavior checks (traversal, --check/--json, mtime,
# permissions, atomic write, over-cap) — one extra "case" folded into the run.
if [ -f tools/test-format-cli.sh ]; then
  echo
  if bash tools/test-format-cli.sh; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
fi

if [ -f tools/test-packages-cli.sh ]; then
  echo
  if bash tools/test-packages-cli.sh; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
fi
# The browser build (rae build --target wasm, Emscripten + EmdawnWebGPU):
# 109 and 110 as pages and an embeddable module, ~50 s. It prints SKIP and
# passes on a machine without emcc. It broke once unnoticed for every WebGPU
# app (wgpu-native's webgpu/wgpu.h; docs/physics-performance-plan.md §10g).
if [ -f tools/wasm_webgpu_smoke.sh ]; then
  echo
  if bash tools/wasm_webgpu_smoke.sh; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
fi
# The compiler's speed on one standard program (tools/compiler-speed.sh): a
# measurement, not a case, so it never changes the pass/fail count.
if [ -f tools/compiler-speed.sh ]; then
  echo
  bash tools/compiler-speed.sh || true
fi
# #44411932: native file-open dialog binding, env-driven headless path.
if [ -f tools/test-sdl-file-dialog.sh ]; then
  echo
  if bash tools/test-sdl-file-dialog.sh; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
fi
# `rae watch` restarts on a `.raescene` change too (the scene is app data no
# build sees; the supervisor is the one dev loop). Runs a real supervisor on a
# tiny CLI program — a few seconds.
if [ -f tools/test-watch-scene-data.sh ]; then
  echo
  if bash tools/test-watch-scene-data.sh; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
fi
# File, asset and console policy (docs/runtime-c-audit.md row 7) with stdin,
# $RAE_STDLIB and a scratch directory, which a single-file case cannot set.
if [ -f tools/test-files-io.sh ]; then
  echo
  if bash tools/test-files-io.sh; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
fi
echo
echo "=========================================="
echo "Results: $PASSED passed, $FAILED failed"
echo "($SKIPPED skipped; $((END-START)) s wall at $JOBS jobs)"
echo "=========================================="
[ -f tools/update_test_history.py ] && ./tools/update_test_history.py $PASSED_NAMES >/dev/null 2>&1
if [ "${RAE_TEST_KEEP_LOGS:-0}" = "1" ]; then echo "(per-case logs kept in $OUT)"; else rm -rf "$OUT"; fi
RC=0; [ "$FAILED" -gt 0 ] && RC=1

if [ "${RAE_RUN_EXAMPLES:-0}" = "1" ] && [ "${RAE_SKIP_EXAMPLES:-0}" != "1" ]; then
  echo; echo "Example smoke tests run SEQUENTIALLY (windows/screenshots):"
  ./tools/run_examples.sh || RC=1
else
  echo; echo "Example smoke tests (visual) not run — RAE_RUN_EXAMPLES=1 or 'make test-examples' runs them."
fi
exit $RC
