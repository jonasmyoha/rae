#!/bin/sh
# The relay with every side in its own process, on loopback (docs/relay.md):
# a relay, a host and four players exchange messages (each player finds a
# direct path to the host through the relay's introduction, as loopback
# always allows), one player leaves, one joins late; then, relay-only, the
# relay's added latency and its throughput are measured against direct UDP.
#
#   sh tools/relay/loopback-check.sh        (from the repository root)
#
# Then a host-migration game: the host process is killed with SIGKILL
# mid-session and the players must be playing again within 3 s, player 2
# (the desktop) as the new host, everyone with their own entity and the same
# entities.
#
# Exits non-zero when a message is missing or out of order, a migration
# check fails, or a process fails. Builds into a temporary directory and leaves no process running.
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
  LAST_PID=$!
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
    *"echoes 200/200, out of order 0"*"path direct"*) ;;
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
start "$WORK/benchHost.log" "$WORK/relayPeer" bench-host --relay-port "$PORT" --seconds 12 --relay-only
BENCH_TOKEN=$(wait_line "$WORK/benchHost.log" "^token " | awk '{print $2}') || exit 1
DIRECT=$(wait_line "$WORK/benchHost.log" "^direct port" | awk '{print $NF}') || exit 1
run_with_timeout 30 "$WORK/relayPeer" bench-player --relay-port "$PORT" --token "$BENCH_TOKEN" \
  --direct-port "$DIRECT" --relay-only >"$WORK/benchPlayer.log" 2>&1 || status=1
cat "$WORK/benchPlayer.log"
for pid in $PIDS; do
  kill "$pid" 2>/dev/null
done
sleep 0.3
grep "^relay:" "$WORK/relay.log" | tail -1

# Host migration: the game host is killed with SIGKILL mid-session; the
# relay (peer timeout 1.5 s) promotes player 2, the only desktop, who takes
# over from its replicated copy; players 1 and 3 reconnect to it
# (MigrationGame.rae, docs/replication.md "Host migration")
start "$WORK/gameRelay.log" "$WORK/relay" --host 127.0.0.1 --port 0 --seconds 30 --timeout-ms 1500 --stats
GAME_PORT=$(wait_line "$WORK/gameRelay.log" "relay listening on port" | awk '{print $NF}') || exit 1
start "$WORK/gameHost.log" "$WORK/relayPeer" game-host --relay-port "$GAME_PORT" --seconds 25
GAME_HOST_PID=$LAST_PID
GAME_TOKEN=$(wait_line "$WORK/gameHost.log" "^token " | awk '{print $2}') || exit 1
start "$WORK/game1.log" "$WORK/relayPeer" game-player --relay-port "$GAME_PORT" --token "$GAME_TOKEN" --device mobile --seconds 15
sleep 0.2
start "$WORK/game2.log" "$WORK/relayPeer" game-player --relay-port "$GAME_PORT" --token "$GAME_TOKEN" --device desktop --seconds 15
sleep 0.2
start "$WORK/game3.log" "$WORK/relayPeer" game-player --relay-port "$GAME_PORT" --token "$GAME_TOKEN" --device mobile --seconds 15
sleep 3
kill -9 "$GAME_HOST_PID"
{ wait "$GAME_HOST_PID"; } 2>/dev/null
echo "game host killed (SIGKILL)"
sleep 6
first_after=""
for who in game1 game2 game3; do
  line=$(grep "^migration:" "$WORK/$who.log")
  echo "$who: ${line:-no migration seen}"
  host=$(echo "$line" | sed -n 's/^migration: host \([a-z]*\),.*/\1/p')
  gap=$(echo "$line" | sed -n 's/.*gap \(-*[0-9]*\) ms.*/\1/p')
  own_before=$(echo "$line" | sed -n 's/.*own before \(-*[0-9]*\) after.*/\1/p')
  own_after=$(echo "$line" | sed -n 's/.*own before -*[0-9]* after \(-*[0-9]*\),.*/\1/p')
  after=$(echo "$line" | sed -n 's/.*after \[\([^]]*\)\]$/\1/p')
  expected_host=false
  [ "$who" = game2 ] && expected_host=true
  if [ -z "$gap" ] || [ "$gap" -gt 3000 ] || [ "$host" != "$expected_host" ] \
     || [ -z "$own_before" ] || [ "$own_before" -lt 0 ] || [ "$own_before" != "$own_after" ]; then
    status=1
  fi
  if [ -z "$first_after" ]; then
    first_after=$after
  elif [ "$after" != "$first_after" ]; then
    status=1
  fi
done
grep "^relay:" "$WORK/gameRelay.log" | tail -1

if [ "$status" -eq 0 ]; then
  echo "PASS: relay loopback check"
else
  echo "FAIL: relay loopback check" >&2
fi
exit "$status"
