#!/usr/bin/env bash
# Live test runner: streams `make test` to a stable log you can `tail -f`,
# with a running "▸ N pass · M fail · <current case>" counter on stderr and
# each FAIL printed on its own line as it happens.
#
#   ./tools/watch-tests.sh          # run + watch here
#   tail -f "$(tools/test-log-path.sh)"   # ...or watch from another terminal
#
# The log is this checkout's (tools/test-log-path.sh): /tmp/rae-test-live.log in the main
# checkout, which Rae devtools tails, and a log of its own in each linked worktree, so suites in
# several worktrees run side by side. RAE_TEST_LOG overrides it.
set -uo pipefail
# Run under the absolute path, so the checkout's path is in the command line: the scoped kill
# (pkill -f "$PWD/compiler/tools/(watch-tests|run_tests)", AGENTS.md) then finds this run and
# no other worktree's
case "$0" in
  /*) ;;
  *) exec bash "$(cd "$(dirname "$0")" && pwd)/$(basename "$0")" "$@" ;;
esac
LOG="$("$(dirname "$0")/test-log-path.sh")"
cd "$(dirname "$0")/.." || exit 1        # -> compiler/
# ONE RUN AT A TIME PER CHECKOUT (AGENTS.md "Only ONE test run at a time"). A
# checkout's log is shared by its runs: a second run would empty it in the middle of the first, and the
# devtools would drop the first run and show the second (a 900-case suite
# turned into "27 tests, 9.5 s"), while the two runs corrupt each other's
# build files (compiler/build, the cases' outputs). Another worktree has its
# own log, lock and build files, so its run goes on beside this one. So a run holds a lock next to the log, and a second one WAITS
# until the first ends (up to RAE_TEST_LOCK_WAIT seconds, default 3600). A
# lock whose run is gone (killed) is cleared.
LOCK="$LOG.lock"
waited=0
until mkdir "$LOCK" 2>/dev/null; do
  holder="$(cat "$LOCK/pid" 2>/dev/null)"
  # A lock is stale when its pid is gone, or the pid is now another program
  if [ -n "$holder" ] && ! ps -p "$holder" -o command= 2>/dev/null | grep -q watch-tests; then
    rm -rf "$LOCK"
    continue
  fi
  if [ "$waited" -eq 0 ]; then
    printf 'watch-tests: another test run (pid %s) is writing %s; waiting for it to end\n' "${holder:-?}" "$LOG" >&2
  fi
  if [ "$waited" -ge "${RAE_TEST_LOCK_WAIT:-3600}" ]; then
    printf 'watch-tests: gave up after %ss waiting for pid %s\n' "$waited" "${holder:-?}" >&2
    exit 3
  fi
  sleep 2
  waited=$((waited + 2))
done
printf '%s\n' "$$" > "$LOCK/pid"
# Removed when the run ends. A run killed by a signal (a timeout's alarm)
# leaves it behind; the next run sees its pid gone and clears it. (No signal
# trap here: bash would run it only after `make test` returned, so a timeout
# would no longer stop the run.)
trap 'rm -rf "$LOCK"' EXIT
# Bracket the run with explicit sentinels so the devtools tailer knows exactly
# when the run starts/ends and its real exit code — no idle-timeout guessing.
RUN_ID="$(date +%s)-$$"
: > "$LOG"
printf '@@RAE_RUN_START %s@@\n' "$RUN_ID" >> "$LOG"
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
code=${PIPESTATUS[0]}                     # make's exit, not the while loop's
printf '@@RAE_RUN_END exit=%s@@\n' "$code" >> "$LOG"
printf '\n' >&2
exit "$code"
