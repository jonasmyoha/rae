#!/usr/bin/env bash
# Live test runner: streams `make test` to a stable log you can `tail -f`,
# with a running "▸ N pass · M fail · <current case>" counter on stderr and
# each FAIL printed on its own line as it happens.
#
#   ./tools/watch-tests.sh          # run + watch here
#   tail -f /tmp/rae-test-live.log  # ...or watch from another terminal
set -uo pipefail
LOG="${RAE_TEST_LOG:-/tmp/rae-test-live.log}"
cd "$(dirname "$0")/.." || exit 1        # -> compiler/
: > "$LOG"
pass=0; fail=0
make test 2>&1 | while IFS= read -r line; do
  printf '%s\n' "$line" >> "$LOG"
  case "$line" in
    "PASS: "*) pass=$((pass+1)); last="${line#PASS: }" ;;
    "FAIL: "*) fail=$((fail+1)); last="${line#FAIL: }"
               printf '\r\033[K\033[31mFAIL\033[0m %s\n' "$last" >&2 ;;
    "Results:"*) printf '\r\033[K\033[1m%s\033[0m\n' "$line" >&2 ;;
    *) last="${last:-}" ;;
  esac
  printf '\r\033[K▸ %d pass · %d fail · %s' "$pass" "$fail" "${last:-starting}" >&2
done
printf '\n' >&2
