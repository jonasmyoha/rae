#!/usr/bin/env bash
set -euo pipefail

TERM_WAIT_SECONDS=2

# Literal substrings to match in the *command line*
MATCH_1="run_tests.sh"
#MATCH_2="run_examples.sh"
MATCH_2="list_native_any"

SELF_NAME="stop_tests_and_examples.sh"

echo "Stopping test/example scripts (${MATCH_1}, ${MATCH_2})..."

# Collect matching processes using ps + awk with literal substring checks.
# This avoids pgrep regex footguns on macOS.
# Output format: "PID<TAB>COMMAND"
matches="$(ps ax -o pid= -o command= | awk -v m1="$MATCH_1" -v m2="$MATCH_2" -v self="$SELF_NAME" '
  {
    pid=$1
    $1=""
    cmd=substr($0,2)
    if (index(cmd, self) > 0) next
    if (index(cmd, m1) > 0 || index(cmd, m2) > 0) {
      print pid "\t" cmd
    }
  }
')"

if [ -z "${matches}" ]; then
  echo "No matching processes found."
  exit 0
fi

echo "Found:"
echo "${matches}" | awk -F'\t' '{ printf "  %s  %s\n", $1, $2 }'
echo

# Extract PIDs only
pids="$(echo "${matches}" | awk -F'\t' '{print $1}')"

echo "Sending SIGTERM to:"
echo "${pids}" | awk '{printf "  %s\n", $1}'
kill -TERM ${pids} 2>/dev/null || true

sleep "${TERM_WAIT_SECONDS}"

# See which are still alive
still_alive=""
for pid in ${pids}; do
  if kill -0 "${pid}" 2>/dev/null; then
    still_alive="${still_alive} ${pid}"
  fi
done

if [ -z "${still_alive// }" ]; then
  echo "Stopped."
  exit 0
fi

echo
echo "Still running after ${TERM_WAIT_SECONDS}s (forcing SIGKILL):"
# Print only the still-alive ones (re-query ps so it’s accurate)
ps ax -o pid= -o command= | awk -v ids=" ${still_alive} " '
  {
    pid=$1
    $1=""
    cmd=substr($0,2)
    if (index(ids, " " pid " ") > 0) {
      printf "  %s  %s\n", pid, cmd
    }
  }
'

kill -KILL ${still_alive} 2>/dev/null || true
echo "Force-stopped."
