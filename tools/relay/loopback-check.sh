#!/bin/sh
# The relay with every side in its own process, on loopback (docs/relay.md):
# a relay, a host and three players exchange messages through it, one
# player leaves, one joins late; then the relay's added latency and its
# throughput are measured against direct UDP.
#
#   sh tools/relay/loopback-check.sh        (from the repository root)
#
# Exits non-zero when a message is missing or out of order, or a process
# fails. Builds into a temporary directory and leaves no process running.
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RAE="$ROOT/compiler/bin/rae"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rae-relay-check.XXXXXX")
PIDS=""

run_with_timeout() {
  seconds=$1
  shift
  perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"
}

cleanup() {
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null
  done
  sleep 0.2
  for pid in $PIDS; do
    kill -9 "$pid" 2>/dev/null
  done
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# build <entry> <name>: a release binary at $WORK/<name>
build() {
  out="$WORK/$2-build"
  mkdir -p "$out"
  run_with_timeout 300 "$RAE" build --profile release --emit-c --out "$out/app.c" \
    --entry "$ROOT/$1" >"$out/build.log" 2>&1 &&
  run_with_timeout 300 cc -std=c11 -O2 -DNDEBUG -w "$out/app.c" "$out/rae_runtime.c" -I"$out" \
    -framework Foundation -framework ImageIO -framework CoreGraphics -o "$WORK/$2" \
    >>"$out/build.log" 2>&1 || {
    cat "$out/build.log" >&2
    exit 1
  }
}

# start <log> <command...>: in the background, its pid remembered
start() {
  log=$1
  shift
  # perl execs the program itself, so $! is the program (a shell function
  # in the background would be a subshell, and killing it would leave the
  # program running)
  perl -e 'alarm shift; exec @ARGV' 60 "$@" >"$log" 2>&1 &
  PIDS="$PIDS $!"
}

# wait_line <log> <pattern>: the first matching line (10 s at most)
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
build tools/relayPeer/Main.rae relayPeer

# The scenario: host + players 1-3, player 3 leaves, player 4 joins late
start "$WORK/relay.log" "$WORK/relay" --host 127.0.0.1 --port 0 --seconds 20 --stats
PORT=$(wait_line "$WORK/relay.log" "relay listening on port" | awk '{print $NF}') || exit 1
start "$WORK/host.log" "$WORK/relayPeer" host --relay-port "$PORT" --seconds 6
TOKEN=$(wait_line "$WORK/host.log" "^token " | awk '{print $2}') || exit 1
start "$WORK/player1.log" "$WORK/relayPeer" player --relay-port "$PORT" --token "$TOKEN" --count 200 --seconds 5
start "$WORK/player2.log" "$WORK/relayPeer" player --relay-port "$PORT" --token "$TOKEN" --count 200 --seconds 5
start "$WORK/player3.log" "$WORK/relayPeer" player --relay-port "$PORT" --token "$TOKEN" --count 200 --leave-after-ms 200 --seconds 5
start "$WORK/player4.log" "$WORK/relayPeer" player --relay-port "$PORT" --token "$TOKEN" --count 200 --start-after-ms 1500 --seconds 3
sleep 6.5
status=0
for who in player1 player2 player3 player4; do
  line=$(grep "^player" "$WORK/$who.log")
  echo "$who: ${line:-no result}"
  case "$line" in
    *"echoes 200/200, out of order 0"*) ;;
    *) status=1 ;;
  esac
done
grep "^player" "$WORK/player3.log" | grep -q "left" || status=1
host_line=$(grep "^host:" "$WORK/host.log")
echo "$host_line"
case "$host_line" in
  *"received 800 messages"*"told of 1 leaving"*) ;;
  *) status=1 ;;
esac

# Latency and throughput through the relay, against direct UDP
start "$WORK/benchHost.log" "$WORK/relayPeer" bench-host --relay-port "$PORT" --seconds 12
BENCH_TOKEN=$(wait_line "$WORK/benchHost.log" "^token " | awk '{print $2}') || exit 1
DIRECT=$(wait_line "$WORK/benchHost.log" "^direct port" | awk '{print $NF}') || exit 1
run_with_timeout 30 "$WORK/relayPeer" bench-player --relay-port "$PORT" --token "$BENCH_TOKEN" \
  --direct-port "$DIRECT" >"$WORK/benchPlayer.log" 2>&1 || status=1
cat "$WORK/benchPlayer.log"
for pid in $PIDS; do
  kill "$pid" 2>/dev/null
done
sleep 0.3
grep "^relay:" "$WORK/relay.log" | tail -1

if [ "$status" -eq 0 ]; then
  echo "PASS: relay loopback check"
else
  echo "FAIL: relay loopback check" >&2
fi
exit "$status"
