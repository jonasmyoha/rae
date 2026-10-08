#!/bin/sh
# The correctness checker (docs/server-benchmarks-design.md §8): every rule of
# spec/Http.md, against a running server or the two Rae servers.
#
#   check.sh <port> [name]   check the server listening on 127.0.0.1:<port>
#   check.sh --rae           build both Rae HTTP servers (eventLoop, ecs), start
#                            each, check it, stop it (the pre-suite case, F7)
#
# It checks correctness, never speed. Exit 0 when every check passes.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"

check_port() {
  run_with_timeout 60 python3 - "$1" "$2" <<'PYTHON'
import json, re, socket, sys, time

port, name = int(sys.argv[1]), sys.argv[2]
failures = []
DATE = re.compile(r"^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d\d (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{4} \d\d:\d\d:\d\d GMT$")

def request(path):
    return ("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n" % path).encode()

class Connection:
    def __init__(self):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buffer = b""

    def fill(self):
        chunk = self.socket.recv(65536)
        if not chunk:
            raise EOFError("connection closed")
        self.buffer += chunk

    def response(self):
        while b"\r\n\r\n" not in self.buffer:
            self.fill()
        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ", 2)
        headers = {}
        for line in lines[1:]:
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "-1"))
        if length < 0:
            raise ValueError("no Content-Length")
        while len(self.buffer) < length:
            self.fill()
        body, self.buffer = self.buffer[:length], self.buffer[length:]
        return int(parts[1]), headers, body

    def close(self):
        self.socket.close()

def common(label, status, headers, body):
    if "server" not in headers:
        failures.append("%s: no Server header" % label)
    if not DATE.match(headers.get("date", "")):
        failures.append("%s: Date is not an IMF-fixdate: %r" % (label, headers.get("date")))
    if int(headers.get("content-length", "-1")) != len(body):
        failures.append("%s: Content-Length does not match the body" % label)

def expect(label, kind, response):
    status, headers, body = response
    common(label, status, headers, body)
    content_type = headers.get("content-type", "")
    if kind == "plaintext":
        if status != 200 or body != b"Hello, World!" or not content_type.startswith("text/plain"):
            failures.append("%s: plaintext wrong: %d %r %r" % (label, status, content_type, body))
    elif kind == "json":
        try:
            value = json.loads(body)
        except ValueError:
            value = None
        if status != 200 or value != {"message": "Hello, World!"} or not content_type.startswith("application/json"):
            failures.append("%s: json wrong: %d %r %r" % (label, status, content_type, body))
    elif status != 404:
        failures.append("%s: expected 404, got %d" % (label, status))

def single(kind, path):
    connection = Connection()
    connection.socket.sendall(request(path))
    expect("GET " + path, kind, connection.response())
    connection.close()

checks = [
    ("plaintext", lambda: single("plaintext", "/plaintext")),
    ("json", lambda: single("json", "/json")),
    ("not found", lambda: single("missing", "/missing")),
]

def keep_alive():
    connection = Connection()
    for i, (kind, path) in enumerate([("plaintext", "/plaintext"), ("json", "/json"), ("plaintext", "/plaintext")]):
        connection.socket.sendall(request(path))
        expect("keep-alive #%d" % i, kind, connection.response())
    connection.close()

def pipelined():
    # 16 requests in one write before any reply, /plaintext and /json mixed:
    # the replies must come back in request order
    kinds = [("plaintext", "/plaintext"), ("json", "/json")] * 8
    connection = Connection()
    connection.socket.sendall(b"".join(request(path) for _, path in kinds))
    for i, (kind, _) in enumerate(kinds):
        expect("pipelined #%d" % i, kind, connection.response())
    connection.close()

def split():
    # One request in two writes
    connection = Connection()
    whole = request("/json")
    connection.socket.sendall(whole[:9])
    time.sleep(0.05)
    connection.socket.sendall(whole[9:])
    expect("split request", "json", connection.response())
    connection.close()

checks += [("keep-alive", keep_alive), ("pipelining", pipelined), ("split request", split)]
for label, check in checks:
    before = len(failures)
    try:
        check()
    except Exception as error:
        failures.append("%s: %s: %s" % (label, type(error).__name__, error))
    print("%s: %s %s" % ("PASS" if len(failures) == before else "FAIL", name, label))
for failure in failures:
    print("  " + failure)
sys.exit(1 if failures else 0)
PYTHON
}

if [ "${1:-}" = "--rae" ]; then
  status=0
  trap 'stop_server' EXIT INT TERM
  for style in eventLoop ecs; do
    build_rae "$style" || { echo "FAIL: rae-$style does not build"; status=1; continue; }
    port=$(free_port)
    start_server "$BUILD/rae-$style/check.log" env PORT="$port" WORKERS=2 "$BUILD/rae-$style/server"
    if wait_for_port "$port"; then
      check_port "$port" "rae-$style" || status=1
    else
      echo "FAIL: rae-$style did not start"
      cat "$BUILD/rae-$style/check.log"
      status=1
    fi
    stop_server
  done
  if [ "$status" = 0 ]; then
    echo "PASS: server-check (rae-eventLoop and rae-ecs HTTP servers, every spec/Http.md rule)"
  else
    echo "FAIL: server-check (see above)"
  fi
  exit $status
fi

if [ $# -lt 1 ]; then
  echo "usage: check.sh <port> [name] | check.sh --rae" >&2
  exit 2
fi
check_port "$1" "${2:-server on $1}"
