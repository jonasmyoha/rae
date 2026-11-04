#!/bin/sh
# Host migration in a walker session, on loopback (docs/walker-net.md): a relay at
# its default timeouts, a headless host (walkerBot host), WINDOWS 143 windows and
# BOTS bots. The host is killed with SIGKILL mid-session. Every survivor must be
# playing again within the relay's timeout (10 s, which is when it notices) plus
# BOUND_AFTER seconds: one hosting, the others with a desync check from the new host.
#
#   sh tools/walkerBot/migration-check.sh [BOTS] [WINDOWS]
#       (default 3 bots, no windows; the relay promotes a desktop that joined
#       first, so with windows a window hosts)
#
# Fails when a survivor is not back in time, no survivor hosts or more than one
# does, a desync was seen, or a window made a new character for a walker that
# stayed. Prints each survivor's time. Builds into a temporary directory and leaves
# no process running.
set -u
BOTS=${1:-3}
WINDOWS=${2:-0}
BOUND_AFTER=3
RELAY_TIMEOUT=10
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RAE="$ROOT/compiler/bin/rae"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rae-walker-migration.XXXXXX")
PIDS=""
cleanup() {
  for pid in $PIDS; do kill "$pid" 2>/dev/null; done
  sleep 0.2
  for pid in $PIDS; do kill -9 "$pid" 2>/dev/null; done
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

nowTenths() { perl -MTime::HiRes=time -e 'printf "%d\n", time() * 10'; }

build() {
  out="$WORK/$2-build"
  mkdir -p "$out"
  perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --profile release --emit-c \
    --out "$out/app.c" --entry "$ROOT/$1" >"$out/build.log" 2>&1 &&
  perl -e 'alarm shift; exec @ARGV' 300 cc -std=c11 -O2 -DNDEBUG -w "$out/app.c" "$out/rae_runtime.c" \
    -I"$out" -framework Foundation -framework ImageIO -framework CoreGraphics -framework Security \
    -o "$WORK/$2" >>"$out/build.log" 2>&1 || {
    cat "$out/build.log" >&2
    exit 1
  }
}

# start <seconds> <log> <command...>: its pid in LAST_PID
start() {
  limit=$1
  log=$2
  shift 2
  perl -e 'alarm shift; exec @ARGV' "$limit" "$@" >"$log" 2>&1 &
  LAST_PID=$!
  PIDS="$PIDS $!"
}

# wait_line <tries of 0.2 s> <log> <pattern>
wait_line() {
  tries=$1
  while [ "$tries" -gt 0 ]; do
    line=$(grep -m 1 -e "$3" "$2" 2>/dev/null)
    if [ -n "$line" ]; then
      echo "$line"
      return 0
    fi
    sleep 0.2
    tries=$((tries - 1))
  done
  echo "timed out waiting for '$3' in $2" >&2
  tail -5 "$2" >&2
  return 1
}

build tools/relay/Main.rae relay
build tools/walkerBot/Main.rae walkerBot

start 400 "$WORK/relay.log" "$WORK/relay" --host 127.0.0.1 --port 0 --seconds 390 --stats
PORT=$(wait_line 50 "$WORK/relay.log" "relay listening on port" | awk '{print $NF}') || exit 1
start 390 "$WORK/host.log" "$WORK/walkerBot" host --relay-port "$PORT" --seconds 380
HOST_PID=$LAST_PID
TOKEN=$(wait_line 50 "$WORK/host.log" "^token " | awk '{print $2}') || exit 1

SURVIVORS=""
w=0
while [ "$w" -lt "$WINDOWS" ]; do
  start 300 "$WORK/window$w.log" env RAE_WALKER_NET=join RAE_WALKER_RELAY=127.0.0.1:$PORT \
    RAE_WALKER_TOKEN=$TOKEN RAE_WALKER_NET_SECONDS=150 "$RAE" run "$ROOT/examples/143_walker_forest/Main.rae"
  SURVIVORS="$SURVIVORS window$w"
  w=$((w + 1))
done
w=0
while [ "$w" -lt "$WINDOWS" ]; do
  wait_line 900 "$WORK/window$w.log" "joined:" >/dev/null || exit 1
  w=$((w + 1))
done
b=0
while [ "$b" -lt "$BOTS" ]; do
  start 90 "$WORK/bot$b.log" "$WORK/walkerBot" bot --relay-port "$PORT" --token "$TOKEN" \
    --name "bot$b" --seed $((b + 1)) --seconds 45
  SURVIVORS="$SURVIVORS bot$b"
  b=$((b + 1))
done
sleep 6
EXPECTED=$((BOTS + WINDOWS - 1))
echo "relay port $PORT; $BOTS bots and $WINDOWS windows; killing the host"
w=0
while [ "$w" -lt "$WINDOWS" ]; do
  grep -c "joined:" "$WORK/window$w.log" >"$WORK/joinedBefore$w"
  w=$((w + 1))
done
kill -9 "$HOST_PID"
KILLED=$(nowTenths)

status=0
BOUND=$(((RELAY_TIMEOUT + BOUND_AFTER) * 10))
PENDING=$SURVIVORS
: >"$WORK/times"
while [ -n "$PENDING" ] && [ $(($(nowTenths) - KILLED)) -le $((BOUND + 50)) ]; do
  STILL=""
  for who in $PENDING; do
    if grep -q -e "\[net\] the host went" -e "\[net\] playing again" "$WORK/$who.log"; then
      echo "$who $(($(nowTenths) - KILLED))" >>"$WORK/times"
    else
      STILL="$STILL $who"
    fi
  done
  PENDING=$STILL
  sleep 0.1
done
while read -r who tenths; do
  how=$(grep -m 1 -e "\[net\] the host went" -e "\[net\] playing again" "$WORK/$who.log" | cut -c7-60)
  echo "$who: back $((tenths / 10)).$((tenths % 10)) s after the kill ($how)"
  if [ "$tenths" -gt "$BOUND" ]; then
    echo "$who took over the bound of $((BOUND / 10)) s" >&2
    status=1
  fi
done <"$WORK/times"
for who in $PENDING; do
  echo "$who never came back" >&2
  status=1
done

hosts=$(cat "$WORK"/window*.log "$WORK"/bot*.log 2>/dev/null | grep -c "\[net\] the host went")
if [ "$hosts" -ne 1 ]; then
  echo "$hosts survivors host, not 1" >&2
  status=1
fi

sleep 5
[ "$WINDOWS" -gt 0 ] && screencapture -x "${RAE_MIGRATION_SCREENSHOT:-/tmp/walker-migration.png}" 2>/dev/null
# Bots end by their --seconds and print their counts; windows by their own time
for pid in $PIDS; do :; done
b=0
while [ "$b" -lt "$BOTS" ]; do
  line=$(wait_line 300 "$WORK/bot$b.log" "bot$b: ") || status=1
  echo "$line"
  case "$line" in
    *"desyncs 0,"*) ;;
    *) status=1 ;;
  esac
  b=$((b + 1))
done
w=0
while [ "$w" -lt "$WINDOWS" ]; do
  before=$(cat "$WORK/joinedBefore$w")
  after=$(grep -c "joined:" "$WORK/window$w.log")
  echo "window$w: characters made after the kill: $((after - before)), and $(grep -c " left" "$WORK/window$w.log") walkers left"
  if [ "$after" -ne "$before" ]; then
    status=1
  fi
  line=$(wait_line 1000 "$WORK/window$w.log" "desync checks") || status=1
  echo "window$w: $line"
  case "$line" in
    *"desyncs 0,"*) ;;
    *) status=1 ;;
  esac
  w=$((w + 1))
done
grep "^relay:" "$WORK/relay.log" | tail -1
if [ "$status" -eq 0 ]; then
  echo "PASS: walker host migration ($EXPECTED survivors within $((BOUND / 10)) s, no desync)"
else
  echo "FAIL: walker host migration" >&2
fi
exit "$status"
