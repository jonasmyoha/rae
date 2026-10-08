# Phase 1: HTTP/1.1

Our own short spec, after TechEmpower's test types 1 (JSON) and 6
(plaintext) and their rules. It is committed so that external changes cannot
change what is measured. `check.sh` tests every rule below.

## Routes

- `GET /plaintext` → `200`, body exactly `Hello, World!`.
  - Headers: `Content-Type: text/plain`, `Content-Length: 13`, `Server`, `Date`.
- `GET /json` → `200`, body a JSON object equal to
  `{"message":"Hello, World!"}`.
  - The object is serialised **for each request**, with no cached bytes.
    Whitespace between the tokens is allowed, so the check parses the body.
  - Headers: `Content-Type: application/json`, `Content-Length` (the body's
    length), `Server`, `Date`.
- Any other path → `404`.

## Rules for every response

- `Date` is an IMF-fixdate (`Thu, 08 Oct 2026 12:56:33 GMT`). It may be
  cached for up to a second.
- `Content-Length` equals the length of the body. Chunked responses are not
  used.
- `Server` is present. Its value is free.

## Connections

- **Keep-alive.** An HTTP/1.1 connection stays open across requests unless the
  client sends `Connection: close`.
- **Pipelining.** A client may send many requests back to back before reading
  any reply. The server answers them all, in request order. The checker sends
  16 mixed `/plaintext` and `/json` requests in one write.
- **Split requests.** A request may arrive in pieces across several reads.

## Load (run.sh)

- `plaintext` and `json`: 256 connections, keep-alive, one request in flight
  per connection (`oha`).
- `pipelined`: `/plaintext` at 256 connections, 16 requests per write (`wrk`
  with `pipeline.lua`). Only throughput is reported, because wrk's latency
  histogram is wrong under pipelining.
- Each case runs with 1 worker and with every performance core, the same
  for each implementation:
  - Rae: `WORKERS` loops or Worlds;
  - Rust: tokio worker threads;
  - Node: `node:cluster` processes;
  - Bun: processes with `reusePort`.
