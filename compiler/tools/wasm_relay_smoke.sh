#!/usr/bin/env bash
# A browser player in a relay session with a native UDP host (docs/relay.md,
# "WebSocket"): checks/webRelay built for the browser joins over the page's
# own WebSocket, sends 100 numbered messages through lib/net/Connection and
# gets each echoed in order by tools/relayPeer's UDP host, through a native
# tools/relay with a WebSocket port. Runs in headless Chrome; skipped when
# emcc or Chrome is missing. Leaves no process running.
#
#   bash compiler/tools/wasm_relay_smoke.sh      (from the repository root)
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

command -v emcc >/dev/null 2>&1 || { echo "SKIP wasm_relay_smoke: emcc not found"; exit 0; }
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
if [ ! -x "$CHROME" ]; then CHROME="$(command -v google-chrome || command -v chromium || true)"; fi
if [ -z "$CHROME" ] || [ ! -x "$CHROME" ] || ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP wasm_relay_smoke: headless Chrome or python3 not found"
  exit 0
fi

TMP="$(mktemp -d)"
PIDS=""
cleanup() {
  for pid in $PIDS; do kill "$pid" 2>/dev/null || true; done
  sleep 0.2
  for pid in $PIDS; do kill -9 "$pid" 2>/dev/null || true; done
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

RAE=compiler/bin/rae

# A native release binary of entry $1 at $TMP/$2
build_native() {
  local out="$TMP/$2-build"
  mkdir -p "$out"
  if ! { perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --profile release --emit-c \
      --out "$out/app.c" --entry "$1" &&
    perl -e 'alarm shift; exec @ARGV' 300 cc -std=c11 -O2 -DNDEBUG -w "$out/app.c" \
      "$out/rae_runtime.c" -I"$out" -framework Foundation -framework ImageIO \
      -framework CoreGraphics -framework Security -o "$TMP/$2"; } >"$out/build.log" 2>&1; then
    cat "$out/build.log" >&2
    exit 1
  fi
}

# start <log> <command...>: in the background, its pid remembered (perl execs
# the program, so the pid is the program's)
start() {
  local log=$1
  shift
  perl -e 'alarm shift; exec @ARGV' 120 "$@" >"$log" 2>&1 &
  PIDS="$PIDS $!"
}

wait_line() {
  local attempt
  for attempt in $(seq 1 100); do
    if grep -m 1 "$2" "$1" 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "FAIL wasm_relay_smoke: no '$2' in $1:" >&2
  cat "$1" >&2
  return 1
}

build_native tools/relay/Main.rae relay
build_native tools/relayPeer/Main.rae relayPeer
if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target wasm --profile dev \
    --project checks/webRelay --out "$TMP/page/index.html" checks/webRelay/Main.rae \
    >"$TMP/page-build.log" 2>&1; then
  cat "$TMP/page-build.log" >&2
  exit 1
fi

start "$TMP/relay.log" "$TMP/relay" --host 127.0.0.1 --port 0 --web-socket-port 0 --seconds 100
PORT=$(wait_line "$TMP/relay.log" "relay listening on port" | awk '{print $NF}')
WS_PORT=$(wait_line "$TMP/relay.log" "relay WebSocket on port" | awk '{print $NF}')
start "$TMP/host.log" "$TMP/relayPeer" host --relay-port "$PORT" --relay-only --seconds 90
TOKEN=$(wait_line "$TMP/host.log" "^token " | awk '{print $2}')

HTTP_PORT=$((20000 + $$ % 20000))
(cd "$TMP/page" && exec perl -e 'alarm shift; exec @ARGV' 100 python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1) >/dev/null 2>&1 &
PIDS="$PIDS $!"
sleep 1
perl -e 'alarm shift; exec @ARGV' 60 "$CHROME" --headless=new --enable-logging=stderr --v=0 \
  --no-first-run --user-data-dir="$TMP/chrome" \
  "http://127.0.0.1:$HTTP_PORT/index.html?RAE_RELAY_WS_PORT=$WS_PORT&RAE_RELAY_TOKEN=$TOKEN" \
  >"$TMP/chrome.log" 2>&1 &
CHROME_PID=$!
PIDS="$PIDS $CHROME_PID"
for tick in $(seq 1 120); do
  grep CONSOLE "$TMP/chrome.log" 2>/dev/null | grep -qE 'RELAY (OK|FAIL)' && break
  kill -0 "$CHROME_PID" 2>/dev/null || break
  sleep 0.5
done
verdict=$(grep CONSOLE "$TMP/chrome.log" | grep -oE 'RELAY (OK|FAIL)[^"]*' | head -1 || true)
if [ "${verdict#RELAY OK}" != "$verdict" ]; then
  echo "PASS wasm_relay_smoke: a browser player joined a UDP host's relay session; $verdict"
  exit 0
fi
echo "FAIL wasm_relay_smoke: ${verdict:-no verdict from the page}"
grep CONSOLE "$TMP/chrome.log" | sed 's/.*CONSOLE:[0-9]*\] "//;s/", source.*//' | head -15
echo "--- relay"; tail -5 "$TMP/relay.log"
echo "--- host"; tail -5 "$TMP/host.log"
exit 1
