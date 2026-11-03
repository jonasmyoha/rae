# The relay (networking 5/12, 6/12)

A headless Rae program that sits between a game's host and its players
when they cannot (or should not) reach each other directly. The host opens
a session and gets a token, the players join with the token, and the relay
forwards each packet between the host and the session's players. It does
not read the packets: they are lib/net/Connection packets, sealed with
ChaCha20-Poly1305 (lib/net/PacketSeal.rae). Through the relay, every side
sees only the relay's address and peer ids, never another side's IP
address.

When both sides allow it, the relay also tells a player and its host each
other's public address, and they try a direct path. Traffic moves to the
direct path once it works and stays on the relay when it does not (see
"Direct first, the relay as a fallback").

## Pieces

| file | what |
|---|---|
| `lib/net/relay/RelayWire.rae` | the relay's packet kinds and encoders, token text |
| `lib/net/relay/RelayServer.rae` | the World: components, events, resources, add/remove helpers |
| `lib/net/relay/receiveSystem/ReceiveSystem.rae` | datagrams in; forwards out at once; control packets become events |
| `lib/net/relay/sessionSystem/SessionSystem.rae` | answers open / join / leave |
| `lib/net/relay/timeoutSystem/TimeoutSystem.rae` | removes peers silent for `peerTimeoutMs` (scans every 100 ms) |
| `lib/net/relay/noticeSystem/NoticeSystem.rae` | tells a host which players left, players that the session closed |
| `lib/net/relay/webSocketSystem/WebSocketSystem.rae` | WebSocket peers' binary messages in as packets; peers whose connection closed leave |
| `lib/net/relay/Relay.rae` | `relayFrame` (the systems in order) and `relayStep` (wait, then a frame) |
| `lib/net/relay/RelayClient.rae` | a host's or player's side: open / join, keep-alive, forward header, the direct paths |
| `lib/net/relay/RelayConnections.rae` | Connections over the relay: `RelayHost` (one per player), `RelayPlayer` |
| `lib/webSocket/WebSocketClient.rae` | a native WebSocket client (connect, handshake, masked binary messages) |
| `tools/relay/Main.rae` | the relay program |
| `tools/relayPeer/Main.rae`, `tools/relay/loopback-check.sh` | the multi-process check and its measurements |

## The ECS shape

The relay is a World (`RelayServer`). A session is an entity with a
`RelaySession` (its token, its host, the next peer id), and a peer is an
entity with a `RelayPeer` (UDP address, session, peer id, last heard). The
socket, the config, three lookups (address -> peer, token -> session,
(session, peer id) -> peer) and the event queues are resources on it. Each
frame runs four systems in a fixed order:

1. **receive**: drains the socket. A forward packet from a known peer goes
   straight on. Its 4-byte destination peer id is overwritten in place with
   the source's id, and the same buffer is sent to the other side. This path
   copies nothing and allocates nothing. Open, join and leave become
   `RelayRequestEvent`s. A keep-alive marks its peer as heard and is
   answered.
2. **session**: answers the requests. Open makes a session with a random
   16-byte token. Join with a known token makes a player with the next peer
   id. A repeated request (its reply was lost) gets the same answer again.
   An unknown token is refused.
3. **timeout**: removes a silent peer. A player's removal sends a
   `PeerLeftEvent`. A host's removal closes its session, with a
   `SessionClosedEvent` for each of its players.
4. **notice**: sends those events as `peerLeft` to the host and `closed` to
   the players.

## Wire format

Byte 0 is the kind. Integers are big-endian.

| kind | from -> to | rest |
|---|---|---|
| 201 open | host -> relay | request id (4) |
| 202 opened | relay -> host | request id (4), token (16) |
| 203 join | player -> relay | request id (4), token (16) |
| 204 joined | relay -> player | request id (4), peer id (4) |
| 205 refused | relay -> client | request id (4), reason (1: unknown session, 2: full, 3: relay full) |
| 206 forward | client -> relay | destination peer id (4), payload |
| 206 forward | relay -> client | source peer id (4), payload |
| 207 keepAlive | both ways | nothing |
| 208 leave | client -> relay | nothing |
| 209 peerLeft | relay -> host | peer id (4) |
| 210 closed | relay -> player | nothing (the host went and no player was left, or migration is off) |
| 211 introduce | relay -> host, player | the other side's peer id (4), secret (8), address (23: family, high, low, port, scope) |
| 212 punch | client -> client, direct | secret (8), sender's peer id (4) |
| 213 punchAck | client -> client, direct | secret (8), sender's peer id (4) |
| 214 direct | client -> client, direct | sender's peer id (4), payload (a forward that skipped the relay) |

`open` and `join` may end with two more bytes: relay only (1), then the
device (1 desktop, 2 mobile), which the host-migration rule uses. When the
host goes, kind 215 `hostChanged` (the promoted player's former peer id)
goes to every peer of the session; see docs/replication.md, "Host
migration".

The host is peer 0, and players are 1, 2, ... in the order they joined. A
player forwards only to its host. A client ignores datagrams that come
neither from the relay's address nor from a peer the relay introduced.

## Direct first, the relay as a fallback

1. When a player joins, the session system sends `introduce` (twice, in
   case one is lost) to the player, with the host's address, and to the
   host, with the player's address. Both get the same random 8-byte secret.
   The address is what the relay saw: behind a NAT, the public mapping of
   the very socket the client talks to the relay from.
2. Both sides send `punch` from that same socket every `punchIntervalMs`
   (50 ms). A NAT that lets the outgoing punch create a mapping then lets
   the other side's punch in. A punch with the right secret and peer id is
   answered with a `punchAck`. The first ack, or the first `direct` packet,
   makes the path `direct` (`RelayClient`'s `DirectPath`).
3. Traffic does not wait for this: until the path is direct, packets go
   through the relay as before. When no ack comes within `punchTimeoutMs`
   (1.5 s), the path is `fallback`, the punching stops, and the relay
   carries the traffic. A direct path that hears nothing for
   `directSilenceMs` (3 s) goes back to the relay too. The connection's
   keep-alives (every 100 ms) keep a working path from looking silent.
4. Keep-alives still go to the relay (`lastRelaySendMs`), so the relay
   keeps the peer and its NAT mapping alive while the game traffic is
   direct.
5. `RelayClientConfig.relayOnly` (on either side) makes the relay skip
   the introduction, so neither side learns the other's address and no
   direct path is tried. A streaming host that hides its address sets it.

The direct packets are the same sealed Connection packets. A forged
`direct` packet from the right address still fails to open.

What the loopback tests show (fixture `1271_direct_first_relay_fallback`,
and `loopback-check.sh`, where all four player processes end up direct):

- the punches meet and the traffic leaves the relay;
- with a lib/net/BadNetwork dropping every direct packet (standing in for
  a NAT that blocks the punches), both sides fall back after 0.6 s and
  every message still arrives;
- a relay-only host or player is never introduced;
- a direct path that dies later falls back, and nothing is lost.

What one machine cannot test, and so is not tested yet:

- **Real NAT behavior.** Endpoint-independent ("cone") NATs let the
  punches through. Symmetric NATs map each destination to a new port, so
  the address the relay saw is not the one the punch comes from, and the
  punches fail. That case falls back to the relay by design, but no test
  shows it on a real NAT. NATs that need both punches within a few
  milliseconds, and ones that expire a mapping quickly, are untested.
- **Carrier-grade NAT on mobile networks (CGNAT)**, often symmetric, and
  IPv6 on carriers (no NAT, but a stateful firewall: the simultaneous
  punches should open it, untested).
- **Two players behind the same NAT** (a LAN party). The relay gives them
  each other's public address, which works only if the router
  "hairpins". Local-address candidates are not sent yet.
- **A network change mid-game** (Wi-Fi to cellular). The address changes,
  the direct path goes silent and falls back to the relay, but the relay
  knows the peer by its old address, so the peer times out there.
  Reconnect is 9/12.
- **Real latency and loss.** Loopback has neither. lib/net/BadNetwork
  simulates them, but not on the direct path's NAT timing.

## Measured (loopback, Apple M1 Max, release build, 2026-11-03)

`sh tools/relay/loopback-check.sh` runs a relay, a host and four players as
separate processes. The players send 200 messages each through encrypted
Connections, and the host echoes them. Player 3 leaves, and player 4 joins
1.5 s late. All 800 messages arrive once and in order, and the host is told
once that a player left. Then it measures with 64-byte payloads:

| | value (3 runs) |
|---|---|
| round trip through the relay (median of 2000) | 69-81 us |
| round trip direct, same payload (median of 2000) | 34-54 us |
| added by the relay | 26-36 us per round trip, about 13-18 us per packet |
| relay throughput, 64 round trips in flight | 83,000-98,000 packets/s forwarded |

The throughput is one relay process on one core, and the test client
shares the machine. It is a lower bound for the relay itself, not a limit
of it.

## WebSocket: peers that cannot send UDP

A browser cannot send UDP, so the WASM build of a game reaches the relay
over a WebSocket. `relay --web-socket-port P` (`relayListenWebSocket`) also
listens for TCP on P and upgrades `ws://host:P/relay`; plain HTTP there is
answered 404. Each binary message is one relay packet, byte for byte what a
datagram would carry, so the relay's systems and the client's handshake,
keep-alives and forward header are unchanged, and so is everything above
them: lib/net/Connection's encryption and reliable channel, replication,
sessions.

- **Relay side.** A WebSocket peer gets a stand-in address in the IPv6
  discard prefix `100::/64` (RFC 6666, never a real sender) that names its
  connection entity in the relay's HTTP server (lib/net/ecs). Every lookup
  by address works unchanged; `relaySendBytes` sends to such an address as
  a binary frame and to any other as a datagram, so a forward from a UDP
  peer to a WebSocket peer (or back) is the same code. A WebSocket peer is
  always relay-only: its address means nothing to anyone else. The UDP
  socket is watched by the HTTP server's poller, so one wait covers both. A
  closed connection is noticed within 100 ms and the peer leaves at once
  (an ended UDP peer only times out).
- **Client side.** `createRelayClientOverWebSocket(host, port, config,
  connectTimeoutMs)` makes a relay-only `RelayClient` whose transport is a
  `WebSocketClient` (no UDP socket); `createRelayHostWith` /
  `createRelayPlayerWith` wrap it. `relayClientSocketId` is the socket to
  wait on. When the connection breaks, the client reports the session closed.
- **Checks.** `compiler/tests/cases/1283_relay_web_socket` (one process): a
  UDP host with a UDP and a WebSocket player, and a WebSocket host with a UDP
  player; every player's messages come back in order; a dropped WebSocket
  player's host is told within 1 s. `tools/relay/loopback-check.sh` runs a
  WebSocket player as its own process (`relayPeer player --web-socket-port`).
- **Not yet: the browser itself.** `WebSocketClient` uses a TCP socket,
  which a browser does not have. In the WASM build it needs a second branch
  (`when Target.os is Os.web`) over Emscripten's `emscripten/websocket.h`:
  one-call shims for open, send and close, and a small C queue that the
  message callback fills and Rae drains each frame. The lib already
  type-checks for web/wasm32. Queued.
- **Cost.** TCP delivers in order and retransmits, so a lost packet stalls
  every packet behind it (head-of-line blocking). Connection's own
  reliability then runs over a channel that never loses anything, and
  unreliable messages (snapshots) can arrive late instead of not at all.
  That is the price of a browser on WebSocket.

### WebTransport, later

WebTransport gives a browser UDP-like unreliable datagrams (and streams)
without head-of-line blocking. A relay that speaks it needs:

- **QUIC** (RFC 9000) with TLS 1.3: a C library behind a shim (ngtcp2 +
  a TLS library, or quiche or msquic prebuilt), on its own UDP port.
- **HTTP/3** (RFC 9114) with Extended CONNECT (RFC 9220) to open a
  WebTransport session, and HTTP datagrams (RFC 9297, QUIC datagrams RFC
  9221) for the unreliable packets; each datagram would be one relay packet,
  like a WebSocket message now.
- **A certificate the browser accepts.** A real one for the relay's name,
  or, for development, a self-signed ECDSA one valid for at most 14 days
  whose hash the page passes in `serverCertificateHashes`.
- **The browser side** through JS glue (Emscripten has no WebTransport
  API): `new WebTransport(url)`, `datagrams.writable` / `readable`.

## Limits and next steps

- **Spoofing.** The relay knows a peer only by its UDP source address, so
  an attacker who can forge that address on the path can inject packets
  for that peer. The packets themselves are sealed: a forged packet fails
  to open at the other end, so the attacker can disrupt the session but
  not read it or speak in it.
- **Active relay.** The connection keys are agreed with unsigned X25519
  keys, so a malicious relay (not one that only forwards) could put its own
  keys in the handshake. This is queued as "authenticate encrypted
  connections".
- **Players talk only to their host** (a star). Player-to-player traffic
  goes through the host, which is what a host-authoritative game wants.
- **Direct paths reveal addresses.** When neither side is relay-only, the
  introduction gives each side the other's public address. That is
  inherent to a direct path; `relayOnly` is the switch.
