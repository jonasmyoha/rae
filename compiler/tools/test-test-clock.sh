#!/bin/bash
# test-test-clock.sh — the test clock (RAE_NOW, compiler/runtime/
# runtime_system_log.c rae_ext_nowMs): the wall clock starts at a local time
# or at epoch ms and runs on; a value that is not a time stops the program
# with a message; with no RAE_NOW the clock is the real one. The fixture
# harness cannot set an environment variable, so these run a scratch program.
# Prints one PASS/FAIL line per check; exits non-zero if any failed.
set -u
BIN="${RAE_BIN:-bin/rae}"
[ -x "$BIN" ] || BIN="./bin/rae"
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: test-clock/$1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: test-clock/$1 — $2"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/Main.rae" <<'RAE'
open Time

func two(value: view Int) ret String {
  if value < 10 {
    ret "0{value}"
  }
  ret "{value}"
}

func main() {
  let first: Int = nowMs()
  let local: CivilTime = localCivilFromEpochSeconds(epochSeconds: first / 1000)
  let utc: CivilTime = civilFromEpochSeconds(epochSeconds: first / 1000)
  log("local {local.year}-{two(value: local.month)}-{two(value: local.day)} {two(value: local.hour)}:{two(value: local.minute)}")
  log("utc {utc.year}-{two(value: utc.month)}-{two(value: utc.day)} {two(value: utc.hour)}:{two(value: utc.minute)}")
  sleep(ms: 1100)
  log("ran {nowMs() - first >= 1000}")
}
RAE
export RAE_FORMAT=off RAE_PROGRESS=off
run() { (cd "$WORK" && env "$@" "$BIN" run --target compiled "$WORK/Main.rae" 2>&1 | grep -v '^@@'); }

# 1. A local time: the clock starts there and runs on
OUT=$(run RAE_NOW=2026-11-03T10:00)
if echo "$OUT" | grep -q "^local 2026-11-03 10:00$" && echo "$OUT" | grep -q "^ran true$"; then ok local_time; else bad local_time "[$OUT]"; fi

# 2. Seconds too, just before midnight
OUT=$(run RAE_NOW=2026-11-03T23:59:30)
if echo "$OUT" | grep -q "^local 2026-11-03 23:59$"; then ok seconds; else bad seconds "[$OUT]"; fi

# 3. Epoch milliseconds (1793694615000 is 2026-11-03 08:30:15 UTC)
OUT=$(run RAE_NOW=1793694615000)
if echo "$OUT" | grep -q "^utc 2026-11-03 08:30$"; then ok epoch_ms; else bad epoch_ms "[$OUT]"; fi

# 4. Not a time: the program stops and names it
OUT=$(run RAE_NOW=tomorrow)
if echo "$OUT" | grep -q "RAE_NOW='tomorrow' is not a time" && ! echo "$OUT" | grep -q "^local"; then ok not_a_time; else bad not_a_time "[$OUT]"; fi

# 5. A day that does not exist is not a time either
OUT=$(run RAE_NOW=2026-02-30T10:00)
if echo "$OUT" | grep -q "is not a time"; then ok no_such_day; else bad no_such_day "[$OUT]"; fi

# 6. No RAE_NOW: the real clock
OUT=$(run -u RAE_NOW)
if echo "$OUT" | grep -q "^local $(date +%Y-%m-%d) "; then ok real_clock; else bad real_clock "[$OUT] (today $(date +%Y-%m-%d))"; fi

echo "test-clock: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
