#!/usr/bin/env bash
# Tests tools/device_log.c (the log file of an app on a phone) on the Mac, with no phone: each run
# of device_log_test.c logs into a folder and ends as a run on a phone may, and the files are read.
set -uo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d /tmp/device-log-test.XXXXXX)
trap 'rm -rf "$work"' EXIT
passed=0
failed=0

check() { # label, command...
  local label=$1
  shift
  if "$@"; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    echo "FAIL: $label"
  fi
}
has() { grep -qE -- "$1" "$2"; }
hasNot() { ! grep -qE -- "$1" "$2"; }
equal() { [ "$1" = "$2" ] || { echo "  '$1' is not '$2'"; false; }; }
time='^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} '
untimed() { sed -E "s/$time//" "$1"; }
unstamped() { grep -cvE "$time" "$1"; }
run() { perl -e 'alarm shift; exec @ARGV' 60 "$work/app" "$@" > "$work/leak.txt" 2>&1; }

cc -std=c11 -O1 -g -Wall -Wextra -D_DARWIN_C_SOURCE -o "$work/app" "$here/device_log.c" "$here/device_log_test.c" || exit 1
log=$work/log
mkdir -p "$log"

run "$log" mix
check "mix: every line has its time" equal "$(unstamped "$log/rae.log")" 0
check "mix: nothing on the old stdout" equal "$(wc -c < "$work/leak.txt" | tr -d ' ')" 0
check "mix: the first line says the date and the zone" \
  has "$time\[run\] started $(date +%Y-%m-%d), local time UTC[+-][0-9]{4} " <(head -1 "$log/rae.log")
check "mix: then what runs" equal "$(untimed "$log/rae.log" | sed -n 2,4p | tr '\n' '|')" \
  "[run] app Test 1.0|[run] binary UUID none|the run mix|"
check "mix: stdout, stderr and raw writes in the order they were made" \
  equal "$(untimed "$log/rae.log" | grep -E '^(out|err|raw) [0-9]+$' | sed 's/.* //' | tr '\n' ' ')" "$(seq 0 599 | tr '\n' ' ')"
check "mix: a line printed in halves is one line" has "${time}half of a line$" "$log/rae.log"
check "mix: a raw line before exit is in" has "${time}raw at the end$" "$log/rae.log"

run "$log" kill
check "kill: a line printed just before a SIGKILL is in" equal "$(untimed "$log/rae.log" | tail -1)" "line 999"
check "kill: the run before is kept" has "the run mix" "$log/rae.previous.log"

run "$log" abort
check "abort: the panic's words" has "${time}why it did$" "$log/rae.log"
check "abort: the crash handler's last line" equal "$(untimed "$log/rae.log" | tail -1)" "[rae crash] the last line"
check "abort: a line written in pieces is one line" has "${time}rae: abort in test$" "$log/rae.log"

run "$log" trap
check "trap: the assertion's words" equal "$(untimed "$log/rae.log" | tail -1)" "the assertion's words"

run "$log" mix fifth
check "kept: this run" has "the run fifth" "$log/rae.log"
check "kept: the last" has "the run trap" "$log/rae.previous.log"
check "kept: the one before" has "the run abort" "$log/rae.previous2.log"
check "kept: and before" has "the run kill" "$log/rae.previous3.log"
check "kept: no more" equal "$(ls "$log" | wc -l | tr -d ' ')" 4

run "$log" threads
check "threads: every line whole, with its time" equal "$(untimed "$log/rae.log" | grep -cE '^thread [012] line [0-9]+ [0-9]+ end$')" 6000
for which in 0 1 2; do
  check "threads: thread $which's lines in order" equal \
    "$(untimed "$log/rae.log" | grep "^thread $which " | awk '{print $4}' | cksum)" "$(seq 0 1999 | cksum)"
done

RAE_LOG_MAX_KB=32 run "$log" long
check "size: the file holds at most its cap and a line" [ "$(wc -c < "$log/rae.log")" -le 33000 ]
check "size: and so does the earlier part" [ "$(wc -c < "$log/rae.earlier.log")" -le 33000 ]
check "size: the next part says it goes on" has "goes on, what came before is in rae.earlier.log" <(head -1 "$log/rae.log")
check "size: and what runs" has "\[run\] app Test 1.0" "$log/rae.log"
check "size: no line lost between the two" equal \
  "$(cat "$log/rae.earlier.log" "$log/rae.log" | sed -E "s/$time//" | grep '^long run' | awk 'NR > 1 && $4 != last + 1 { print "gap" } { last = $4 } END { print last }')" 2999
run "$log" mix after
check "size: the last run's earlier part is kept too" has "long run line" "$log/rae.previous.earlier.log"
run "$log" mix again
check "size: and goes when its run is not the last" [ ! -e "$log/rae.previous.earlier.log" ]

echo "device-log-test: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
