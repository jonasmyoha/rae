# The relay (networking 5/12)

A headless Rae program that sits between a game's host and its players
when they cannot (or should not) reach each other directly. The host opens
a session and gets a token, the players join with the token, and the relay
forwards each packet between the host and the session's players. It does
not read the packets: they are lib/net/Connection packets, sealed with
ChaCha20-Poly1305 (lib/net/PacketSeal.rae). Every side sees only the
relay's address and peer ids, never another side's IP address.

## Pieces

| file | what |
|---|---|
| `lib/net/relay/RelayWire.rae` | the relay's packet kinds and encoders, token text |
| `lib/net/relay/RelayServer.rae` | the World: components, events, resources, add/remove helpers |
| `lib/net/relay/receiveSystem/ReceiveSystem.rae` | datagrams in; forwards out at once; control packets become events |
| `lib/net/relay/sessionSystem/SessionSystem.rae` | answers open / join / leave |
| `lib/net/relay/timeoutSystem/TimeoutSystem.rae` | removes peers silent for `peerTimeoutMs` (scans every 100 ms) |
| `lib/net/relay/noticeSystem/NoticeSystem.rae` | tells a host which players left, players that the session closed |
| `lib/net/relay/Relay.rae` | `relayFrame` (the systems in order) and `relayStep` (wait, then a frame) |
| `lib/net/relay/RelayClient.rae` | a host's or player's side: open / join, keep-alive, forward header |
| `lib/net/relay/RelayConnections.rae` | Connections over the relay: `RelayHost` (one per player), `RelayPlayer` |
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
| 210 closed | relay -> player | nothing |

The host is peer 0, and players are 1, 2, ... in the order they joined. A
player forwards only to its host. A client ignores datagrams that do not
come from the relay's address.

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
- **Direct connection first, the relay as a fallback** is 6/12.
