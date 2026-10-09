# Shared by check.sh and run.sh (sourced): paths, timeouts, building the
# servers and starting / stopping one.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RAE_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD="$HERE/build"
RAE_BIN="$RAE_ROOT/compiler/bin/rae"
mkdir -p "$BUILD"

run_with_timeout() {
  seconds=$1
  shift
  perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"
}

# build_rae <style> [phase]: benchmarks/servers/rae/<style>/<phase>
# (phase http, the default, or webSocket) -> build/rae-<style>/server, or
# build/rae-<style>-webSocket/server
build_rae() {
  style=$1
  phase=${2:-http}
  out="$BUILD/rae-$style"
  [ "$phase" = http ] || out="$BUILD/rae-$style-$phase"
  mkdir -p "$out"
  run_with_timeout 300 "$RAE_BIN" build --profile release --emit-c --out "$out/server.c" \
    --entry "$HERE/rae/$style/$phase/Main.rae" >"$out/build.log" 2>&1 || {
    cat "$out/build.log" >&2
    return 1
  }
  run_with_timeout 300 cc -std=c11 -O2 -DNDEBUG "$out/server.c" "$out/rae_runtime.c" -I"$out" \
    -framework Foundation -framework ImageIO -framework CoreGraphics -o "$out/server" \
    >>"$out/build.log" 2>&1 || {
    cat "$out/build.log" >&2
    return 1
  }
}

# start_server <log> <command...>: start it in the background (its own
# process group, so stop_server ends its children too); SERVER_PID is set
start_server() {
  log=$1
  shift
  if command -v setsid >/dev/null 2>&1; then
    setsid "$@" >"$log" 2>&1 &
  else
    perl -e 'setpgrp(0, 0); exec @ARGV' "$@" >"$log" 2>&1 &
  fi
  SERVER_PID=$!
}

# wait_for_port <port>: until something answers on it (10 s at most)
wait_for_port() {
  for attempt in $(seq 1 100); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$1/plaintext"; then
      return 0
    fi
    kill -0 "$SERVER_PID" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

# wait_for_tcp <port>: until something accepts a connection on it (10 s at
# most); for servers that answer no plain HTTP
wait_for_tcp() {
  for attempt in $(seq 1 100); do
    if python3 -c 'import socket, sys; socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1).close()' "$1" 2>/dev/null; then
      return 0
    fi
    kill -0 "$SERVER_PID" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

# raise_file_limit: a WebSocket ramp holds thousands of sockets open
raise_file_limit() {
  ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
}

stop_server() {
  [ -n "${SERVER_PID:-}" ] || return 0
  kill -TERM -- "-$SERVER_PID" 2>/dev/null || kill -TERM "$SERVER_PID" 2>/dev/null || true
  for attempt in $(seq 1 30); do
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
  done
  kill -KILL -- "-$SERVER_PID" 2>/dev/null || kill -KILL "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=
}

# A free TCP port on 127.0.0.1
free_port() {
  python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}
