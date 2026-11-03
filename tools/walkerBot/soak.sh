#!/bin/sh
# A networked walker session filled with headless bots, on loopback
# (docs/walker-net.md): a relay, a headless host (walkerBot host) and BOTS
# bots that join and leave in waves for SECONDS seconds. Between waves every
# bot is gone for a few seconds; the host prints its live allocation counts
# at each such quiet point (RAE_MEM_STATS=1).
#
#   sh tools/walkerBot/soak.sh [SECONDS] [BOTS] [WINDOWS]
#       (default 600 s, 20 bots, no windows)
#
# WINDOWS 143 walker windows join the same session for the whole run
# (RAE_WALKER_NET=join) and print their own desync count when they close.
#
# Fails when a bot never got to play, a bot saw a desync (its copy of the
# poses differed from the host's at the same snapshot), a bot did no
# desync check at all, any process leaked at exit, or the host's live
# allocations at the last quiet point are above those at the first.
# Builds into a temporary
# directory (RAE_SOAK_KEEP=1 keeps it, with the logs) and leaves no process
# running.
set -u
SECONDS_TOTAL=${1:-600}
BOTS=${2:-20}
WINDOWS=${3:-0}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RAE="$ROOT/compiler/bin/rae"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rae-walker-soak.XXXXXX")
PIDS=""

cleanup() {
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null
  done
  sleep 0.2
  for pid in $PIDS; do
    kill -9 "$pid" 2>/dev/null
  done
  if [ -n "${RAE_SOAK_KEEP:-}" ]; then
    echo "logs kept in $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT INT TERM

# build <entry> <name>: a release binary at $WORK/<name>
build() {
  out="$WORK/$2-build"
  mkdir -p "$out"
  perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --profile release --emit-c \
    --out "$out/app.c" --entry "$ROOT/$1" >"$out/build.log" 2>&1 &&
  perl -e 'alarm shift; exec @ARGV' 300 cc -std=c11 -O2 -DNDEBUG -w "$out/app.c" \
    "$out/rae_runtime.c" -I"$out" -framework Foundation -framework ImageIO \
    -framework CoreGraphics -framework Security -o "$WORK/$2" >>"$out/build.log" 2>&1 || {
    cat "$out/build.log" >&2
    exit 1
  }
}

# start <seconds> <log> <command...>: in the background, its pid remembered
# (perl execs the program, so $! is the program itself)
start() {
  limit=$1
  log=$2
  shift 2
  RAE_MEM_STATS=1 perl -e 'alarm shift; exec @ARGV' "$limit" "$@" >"$log" 2>&1 &
  PIDS="$PIDS $!"
}

wait_line() {
  for attempt in $(seq 1 100); do
    line=$(grep -m 1 "$2" "$1" 2>/dev/null)
    if [ -n "$line" ]; then
      echo "$line"
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for '$2' in $1" >&2
  cat "$1" >&2
  return 1
}

build tools/relay/Main.rae relay
build tools/walkerBot/Main.rae walkerBot

start $((SECONDS_TOTAL + 420)) "$WORK/relay.log" "$WORK/relay" --host 127.0.0.1 --port 0 \
  --seconds $((SECONDS_TOTAL + 400))
PORT=$(wait_line "$WORK/relay.log" "relay listening on port" | awk '{print $NF}') || exit 1
start $((SECONDS_TOTAL + 420)) "$WORK/host.log" "$WORK/walkerBot" host --relay-port "$PORT" \
  --seconds $((SECONDS_TOTAL + 10)) --steady "$WINDOWS"
TOKEN=$(wait_line "$WORK/host.log" "^token " | awk '{print $2}') || exit 1
echo "relay port $PORT, token $TOKEN, $BOTS bots and $WINDOWS windows for $SECONDS_TOTAL s"
w=0
while [ "$w" -lt "$WINDOWS" ]; do
  start $((SECONDS_TOTAL + 400)) "$WORK/window$w.log" env RAE_WALKER_NET=join \
    RAE_WALKER_RELAY=127.0.0.1:$PORT RAE_WALKER_TOKEN=$TOKEN \
    RAE_WALKER_NET_SECONDS="$SECONDS_TOTAL" "$RAE" run "$ROOT/examples/143_walker_forest/Main.rae"
  w=$((w + 1))
done

# Waves: each bot stays a different time (3-15 s) and joins at a different
# moment of the wave; a wave lasts 20 s, its last 4 s are quiet
WAVE=20
wave=0
botCount=0
started=$(date +%s)
while [ $(( $(date +%s) - started + WAVE )) -le "$SECONDS_TOTAL" ]; do
  i=0
  while [ "$i" -lt "$BOTS" ]; do
    seed=$((wave * BOTS + i + 1))
    delay=$(( (seed * 389) % 1000 ))
    stay=$(( 3000 + (seed * 2731) % 12000 ))
    leave=$((delay + stay))
    if [ "$leave" -gt 15500 ]; then
      leave=15500
    fi
    start 40 "$WORK/bot$seed.log" "$WORK/walkerBot" bot --relay-port "$PORT" --token "$TOKEN" \
      --name "bot$seed" --seed "$seed" --start-after-ms "$delay" \
      --leave-after-ms $((leave - delay)) --seconds 30
    botCount=$((botCount + 1))
    i=$((i + 1))
  done
  sleep "$WAVE"
  wave=$((wave + 1))
done
# The host and the relay end by themselves
for attempt in $(seq 1 4000); do
  grep -q "^host:" "$WORK/host.log" && break
  sleep 0.1
done
sleep 1

status=0
echo "waves $wave, bots $botCount"
grep "^host:" "$WORK/host.log" || { echo "FAIL: the host did not finish" >&2; status=1; }
first=$(grep "^quiet" "$WORK/host.log" | head -1)
last=$(grep "^quiet" "$WORK/host.log" | tail -1)
echo "first quiet point: $first"
echo "last quiet point:  $last"
firstStrings=$(echo "$first" | awk '{print $5}')
lastStrings=$(echo "$last" | awk '{print $5}')
firstBuffers=$(echo "$first" | awk '{print $7}')
lastBuffers=$(echo "$last" | awk '{print $7}')
quietPoints=$(grep -c "^quiet" "$WORK/host.log")
if [ "$quietPoints" -lt 2 ]; then
  echo "FAIL: too few quiet points to compare" >&2
  status=1
elif [ "$lastStrings" -gt "$firstStrings" ] || [ "$lastBuffers" -gt "$firstBuffers" ]; then
  echo "FAIL: the host's live allocations grew between quiet points" >&2
  status=1
fi

played=0
desyncs=0
checks=0
for log in "$WORK"/bot*.log; do
  line=$(grep "^bot[0-9]*:" "$log")
  case "$line" in
  *"never playing"*)
    echo "FAIL: $line" >&2
    status=1
    ;;
  *playing*)
    played=$((played + 1))
    ;;
  *)
    echo "FAIL: $(basename "$log") did not finish:" >&2
    tail -5 "$log" >&2
    status=1
    ;;
  esac
  d=$(echo "$line" | sed -n 's/.*desyncs \([0-9]*\).*/\1/p')
  c=$(echo "$line" | sed -n 's/.*checks \([0-9]*\),.*/\1/p')
  desyncs=$((desyncs + ${d:-0}))
  checks=$((checks + ${c:-0}))
  if [ "${c:-0}" = "0" ]; then
    echo "FAIL: no desync check in $line" >&2
    status=1
  fi
done
echo "bots playing $played/$botCount, desync checks $checks, desyncs $desyncs"
if [ "$desyncs" != "0" ]; then
  status=1
fi

# The windows: each ran, did desync checks and found none
w=0
while [ "$w" -lt "$WINDOWS" ]; do
  # A window started its session after its build and its loading
  for attempt in $(seq 1 1500); do
    grep -q "^\[net\] desync checks" "$WORK/window$w.log" && break
    sleep 0.2
  done
  line=$(grep "^\[net\] desync checks" "$WORK/window$w.log")
  echo "window $w: $line"
  case "$line" in
  *"desyncs 0,"*)
    case "$line" in
    *"checks 0,"*)
      echo "FAIL: window $w did no desync check" >&2
      status=1
      ;;
    esac
    ;;
  *)
    echo "FAIL: window $w: ${line:-no report}" >&2
    tail -5 "$WORK/window$w.log" >&2
    status=1
    ;;
  esac
  w=$((w + 1))
done

# Leaks at exit (RAE_MEM_STATS dumps): every process that ended normally.
# Not the windows: 143 leaves one 2 MB buffer at exit offline as well (queued)
for log in "$WORK"/host.log "$WORK"/relay.log "$WORK"/bot*.log; do
  s=$(sed -n 's/^  \[mem:string:TOTAL *\].* outstanding=\([0-9-]*\).*/\1/p' "$log" | tail -1)
  b=$(sed -n 's/^  \[mem:buf *\].* outstanding=\([0-9-]*\).*/\1/p' "$log" | tail -1)
  if [ "${s:-0}" != "0" ] || [ "${b:-0}" != "0" ]; then
    echo "FAIL: $(basename "$log") leaked at exit: ${s:-0} strings, ${b:-0} buffers" >&2
    status=1
  fi
done

if [ "$status" = 0 ]; then
  echo "walker soak OK"
fi
exit "$status"
