# Client connections through the system's network stack

`lib/net/NetworkConnection` opens a client connection the way the platform
wants a client to: on Apple (iOS, macOS), through Network.framework, not BSD
sockets. That brings up the cellular radio and VPN on demand, follows the
network from Wi-Fi to cellular, checks TLS against the system trust store, and
reports the path. `lib/net/HttpGet` fetches a URL over it. (`lib/net/Tcp` stays
the server side's BSD sockets.)

## Connections

| call | what |
|---|---|
| `networkConnectTls(host, port)` | TLS, certificate checked against the system trust store |
| `networkConnectWebSocket(url)` | a WebSocket client to `ws://` or `wss://`; pings answered on their own |
| `networkConnectTcp(host, port)` | plain TCP (a local server, a test) |
| `networkConnectTlsInsecure(host, port)` | TLS accepting any certificate: tests against a self-signed server only |
| `networkSend` / `networkSendText` | bytes / text (a WebSocket: one binary / text message) |
| `networkSendClose(connection, code)` | a WebSocket close (1000: normal) |
| `networkNextEvent(connection, timeoutMs)` | the next event, waiting up to `timeoutMs` (0: do not wait) |
| `networkClose(connection)` | close now and let go |

Events (`NetworkEvent { kind, code, bytes, text }`):

- `ready`: connected, handshakes done; send now.
- `message`: bytes; on a WebSocket one whole message, `code` its kind
  (`webSocketText` / `webSocketBinary`), `text` the text.
- `closed`: the server closed (a WebSocket: `code` its close code).
- `failed`: over; `text` why.
- `waiting`: no usable network (or the server refused); the framework
  connects when it can. A UI shows "offline"; a caller with a deadline gives
  up when it passes.
- `pathChanged`: `code` the path status (`networkPathSatisfied` 1,
  `Unsatisfied` 2, `Satisfiable` 3), `text` the interface (`wifi`,
  `cellular`, `wired`, `loopback`, `other`).
- `sendFailed`: a send did not go; `text` why.

## Threads and the UI loop

Each event also wakes the UI loop (`EventLoop.wake`), so a UI app reads them
in its next frame with `networkNextEvent(connection, timeoutMs: 0)` until it
answers `none`. A spawned worker waits instead (`timeoutMs` > 0; the call is
`blocking`). One connection is used from one thread. `httpGet` waits for its
events, so a UI app calls it in a worker.

## The C side

`compiler/runtime/runtime_network_apple.h` (platform reason: Network.framework's
C API delivers everything through blocks on a dispatch queue, which Rae cannot
pass). Each connection has a small event queue: the framework's handlers, on
the connection's own serial queue, append events, signal a condition for a
waiting worker and wake the UI loop; every other function is one framework
call. The request, the protocol on top and what an event means are Rae.

It is header-only and included by a program whose Rae imports the module
(`cheader`); the compiler then links `-framework Network -framework Security`,
so other programs neither compile nor link it. `tools/ios/gen-ios.sh` links
Network.framework into every app. Off Apple, connecting answers a `failed`
event that says there is no system connection yet.

## Checks

Case 1290 runs against a local Rae server on a worker thread: a WebSocket
echo and close, an HTTP GET, a refused connection waiting, chunked and URL
parsing; it is on the TSan gate. TLS needs the internet, so it is checked by
hand: `httpGet(url: "https://www.apple.com/robots.txt", timeoutMs: 15000)`
answers 200.
