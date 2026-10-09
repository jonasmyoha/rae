# Phase 2: WebSocket

Our own short spec, after the websocket-shootout protocol (echo, broadcast,
broadcastResult). It is committed so that external changes cannot change what
is measured. `check.sh --websocket` tests every rule below.

## The connection

- The server upgrades `GET /ws` (RFC 6455 §4.2): `101 Switching Protocols`
  with the right `Sec-WebSocket-Accept`. No extension and no subprotocol is
  negotiated (no `permessage-deflate`).
- What the server does with any other request is free (an HTTP answer, or
  closing the connection).
- Clients send masked frames, the server unmasked ones. The server answers a
  ping with a pong carrying the same payload, and a close with a close.
- A message may be fragmented (a text frame and continuations), may arrive
  split across reads, and many messages may arrive in one read. A message can
  be larger than 64 KiB (the 64-bit length form).

## Messages

Every message is a text frame holding a JSON object with a `type` and a
`payload`. The payload is any JSON value; the server passes it on unchanged as
a value (it may re-serialise it, so whitespace and key order inside it may
change).

- `{"type":"echo","payload":X}` → the same message back to the sender:
  `{"type":"echo","payload":X}`.
- `{"type":"broadcast","payload":X}` → `{"type":"broadcast","payload":X}` to
  **every** connected client, the sender included; then
  `{"type":"broadcastResult","payload":X,"listenCount":N}` to the sender, where
  N is the number of clients the broadcast was sent to. The sender sees its
  own broadcast before the broadcastResult.
- A client that has closed is no longer counted.

## Workers

Phase 2 runs every server as **one worker** (one event loop, one World, one
process). A broadcast reaches every client of the server, and with several
workers that needs a fan-out between them (a channel per worker), which is
not built yet; a single worker keeps the comparison the same for everyone.

## Load (run.sh, loadClient)

Our own load client (`loadClient/`, Rust, decision F4) ramps the clients up
in steps (1 000 at a time by default). Every client reads every message it
is sent. After each step a few clients send broadcasts (4 in flight at a
time), each payload carrying its send time, and the round-trip time is the
time from sending a broadcast to its sender receiving the broadcastResult.
The server encodes the broadcast for every client before it queues the
broadcastResult, so the encoding of the whole fan-out is inside the round
trip. Each client's own write happens when the server writes that client: the
4 senders are the first clients to connect, so a server that writes its
connections in the order they were queued or accepted answers them early in
its write pass. Busy fan-outs still delay the next broadcasts, which is what
the ramp sees. Per step it reports the broadcast RTT p50 and p99 and checks
every listenCount.

The ramp stops when the p99 passes 250 ms, when a client cannot connect, or
at the client cap (15 000: macOS has about 16 000 ephemeral ports for one
address). Reported: the client count reached with p99 at most 250 ms, and the
p50 / p99 at every step.
