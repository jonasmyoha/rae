#!/usr/bin/env bash
# run_cases.sh — run a chosen set of unit cases in parallel, with no gates:
# the narrow check for a change that cannot affect the whole suite (see
# "Which tests a task runs" in AGENTS.md).
#
# Usage:  bash tools/run_cases.sh 1163_named_arg_val 541_channel_worker
#         bash tools/run_cases.sh --grep 'open ecs/Query'   # cases whose .rae mention it
# Each name is a case directory under tests/cases (or its number). Verdict
# lines print as cases end; details of each failure and a summary at the end.
# Exit code: 1 if any case failed.
set -u
cd "$(dirname "$0")/.."     # -> compiler/
[ -x bin/rae ] || { echo "error: bin/rae not built (make -C compiler build)" >&2; exit 1; }
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

if [ "${1:-}" = "--grep" ]; then
  [ -n "${2:-}" ] || { echo "usage: run_cases.sh --grep <text>" >&2; exit 2; }
  NAMES=$(grep -rlF --include='*.rae' -- "$2" tests/cases | while read -r f; do
    d=$(dirname "$f")
    # A case's helper modules live below it; the case is the first dir under tests/cases
    echo "$d" | sed 's#^tests/cases/##; s#/.*##'
  done | sort -u)
else
  NAMES=$(printf '%s\n' "$@")
fi
[ -n "$NAMES" ] || { echo "no cases given or matched"; exit 2; }

OUT=$(mktemp -d -t rae_cases_XXXXXX); export OUT
trap 'rm -rf "$OUT"' EXIT
TOTAL=$(printf '%s\n' "$NAMES" | wc -l | tr -d ' ')
echo "Running $TOTAL case(s), $JOBS jobs..."
printf '%s\n' "$NAMES" | xargs -P "$JOBS" -n 1 bash -c '
  RAE_TEST_NO_HISTORY=1 RAE_TEST_APPLY_SKIPS=1 bash tools/run_tests.sh "$1" > "$OUT/$1.log" 2>&1 </dev/null
  grep -m1 -E "^(PASS|FAIL|SKIP): " "$OUT/$1.log" || echo "FAIL: $1 (no verdict)"' _

FAILED=0
for n in $NAMES; do
  if ! grep -qE "^(PASS|SKIP): " "$OUT/$n.log"; then
    FAILED=$((FAILED + 1))
    echo
    if grep -qE "^FAIL: " "$OUT/$n.log"; then
      sed -n '/^FAIL: /,/^$/p' "$OUT/$n.log" | head -40
    else
      echo "FAIL: $n (no such case, or the runner gave no verdict)"; tail -5 "$OUT/$n.log" | sed 's/^/    /'
    fi
  fi
done
echo
echo "Results: $((TOTAL - FAILED)) of $TOTAL passed"
[ "$FAILED" = "0" ]
