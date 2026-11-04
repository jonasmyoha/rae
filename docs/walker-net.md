# Networked walkers

The walker apps (136, 137, 143) can host or join a session through the relay
(`docs/relay.md`). Every player sees the other players' walkers walk, jump
and fall. Headless bots can fill a session.

## Running it

```sh
# a relay (prints "relay listening on port P")
bin/rae run tools/relay/Main.rae -- --port 7777
# a window that hosts (prints "[net] invite T": the relay token and the
# session key, lib/net/relay RelayWire)
RAE_WALKER_NET=host RAE_WALKER_RELAY=127.0.0.1:7777 bin/rae run examples/143_walker_forest/Main.rae
# more windows that join
RAE_WALKER_NET=join RAE_WALKER_RELAY=127.0.0.1:7777 RAE_WALKER_TOKEN=T bin/rae run examples/143_walker_forest/Main.rae
# headless bots (tools/walkerBot)
bin/rae run tools/walkerBot/Main.rae -- bot --relay-port 7777 --token T --seed 3 --seconds 60
```

`RAE_WALKER_NET_SECONDS=n` makes a window leave the session and close n
seconds after it joined. With no `RAE_WALKER_NET` an app is offline, as before. An app can also set
`WalkerAppSettings.net` itself.

## The pieces

- `lib/app3d/walkerNetSystem/WalkerNetSystem.rae`: the session with no
  window and no physics. A `WalkerPose` component (position, heading, speed,
  airborne, knocked down, clip) per walker, replicated with
  `lib/net/replication`, player slots from `lib/net/session`, over
  `lib/net/relay/RelayConnections`. Each player moves its own walker: it
  sends its pose as its replication input, and the host sets that player's
  entity to it and replicates it to everyone. A player draws the other
  walkers from the interpolated table (`shownPoses`).
- `lib/app3d/remoteWalkerSystem/RemoteWalkerSystem.rae`: the app side. The
  local walker's pose comes from its controller and mover. Each remote
  walker that appears gets a character of its own (controller, clip, model,
  height, mover, name label "Player <n>"), and the character is freed when
  the walker leaves: its ragdoll taken apart if it lies, its rows in the
  world and the physics cleared, the entity freed. The character is driven
  with the crowd's mover drive: it walks or runs toward the remote pose,
  jumps when the remote walker takes off, and gets a knock when it falls.
  So remote walkers use this app's physics, clips and ground. A character
  more than 3 m from its remote walker is moved there at once. A window
  shows up to 32 remote walkers (the renderer has 64 skin palettes). In a
  networked session the app has no crowd: every other character is a
  player.
- `tools/walkerBot/Main.rae`: a headless host or bot. A bot walks in a
  circle round the host's walker, jumps every 3 s and falls every 15 s.
- `tools/walkerBot/soak.sh [SECONDS] [BOTS]`: relay, headless host and bots
  joining and leaving in 20 s waves. It fails on a bot that never played, a
  desync, a leak at exit, or host memory that grew between quiet points.

## Desync check

With every snapshot the host replicates a `WalkerNetCheck`: the sum of all
pose values it captured. A player that applied the same snapshot sums its own
copy, and a difference counts as a desync (`net.desyncs` of `net.checks`).

## A slow start, and a player that stalls

Nothing reaches the relay while a frame is running, so a window whose frames stop
for a while looks gone. Two things deal with that:

- **The session starts after the first frames have settled** (`walkerNetStartDue`):
  once 5 frames in a row took under 500 ms, or 60 s after the first frame on a
  machine that never gets there. This matters most for a host, which cannot come
  back: its session goes with it.
- **A player that lost the session joins it again** (`recoverPlayerSession`). A
  stall can outlast any of three clocks: the relay's peer timeout (10 s), the
  relay client's silence timeout (15 s) and the connection's (5 s). Each used to
  leave the player stuck until it quit. Now it frees what it knew of the session
  and joins again as a new player, so it may come back as "Player 3" where it was
  "Player 2". It logs `[net] lost the session (...): joining again`. A session that
  ended, or a refusal, is left alone.

Checked on 2026-11-04 on one MacBook with a relay at its default timeouts: four
143 windows started at once all joined (no timeouts, 0 desyncs), and a joined 143
window frozen with SIGSTOP for 12 s came back and played (the relay counted the
timeout, 0 desyncs). Those windows' first frames were quick, 390-683 ms to the
start of the session, so the settled start itself was not stressed on this
machine; case 1340 tests it and the recovery with short timeouts.

A host that stalls past the relay's timeout mid-session is not recovered.

## Measured (2026-11-03, one MacBook, loopback)

- `soak.sh 600 20 4`: 10 minutes, 4 143 windows joined all the time and 20
  bots per 20 s wave (580 bot sessions, at most 24 other walkers at once).
  Desync checks: 104,637 by the bots and 8,000-11,700 by each window, 0
  desyncs. Host live allocations at the first and the last steady point
  (the 4 windows, no bots) were equal: 2 strings, 761 buffers. No relay,
  host or bot process leaked at exit. (The windows then left one 2 MB
  buffer at exit, offline too: the grass picture's decoded PNG in
  lib/SurfaceImages, freed since; soak.sh now checks the windows as well.)
- Two 143 windows (one host, one joined) and 3 bots: the joined window
  shows the host's walker beside its own, bots running in, and bots lying
  where they fell.
- A window's session starts at its first frame: started before the app's
  loading, the connect attempt ran out (5 s) before the first frame, and
  the window never joined. A player whose connect attempt runs out before
  the welcome now tries again.
