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
- `tools/walkerBot/migration-check.sh [BOTS] [WINDOWS]`: kills the host mid-session
  ("A host that goes").
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

### A host that goes

A session outlives its host, whether the host stalled, froze, crashed or quit. The
relay promotes a player (a desktop first), and:

- **The promoted player hosts** (`walkerNetTakeOver`) from its copy of the world, as
  in docs/replication.md, "Host migration", and logs `[net] the host went: hosting
  the session now`. The invite stays the same.
- **The other players connect to it** and get a whole snapshot.
- **An old host that was only stalled comes back as a player** of the same session
  (`hostLost`), because the relay answers it `forgotten`. The new host stays.
- **A host that was alone** has no session left to go back to, since the relay closed
  it. It hosts a new one, and that means a new invite, which it logs.

The two other options were weighed and not taken. A longer relay
timeout makes every peer that is really gone take longer to notice, and a stall can
still outlast it. A thread that sends keep-alives keeps only the relay satisfied: the
connections still time out, and it cannot help a process that is frozen or dead.

**A remote walker's character stays through it.** A player keeps which entity holds
which net id across the reconnect (`replicaPlayerForNewHost`), and the new host keeps
the net ids. So only the old host's walker goes away. What a player sees with the
default timeouts: connections time out at 5 s and the relay at 10 s, so the walkers on
screen stand at their last poses until the relay promotes someone at 10 s, then move
again. One that moved more than 3 m in that time jumps to its pose. A host that quits
on purpose is replaced at once.

**The app is told.** `walkerNetEvents(net:)`, read after the frame, lists what changed
this frame, and `walkerNetIsHost` says which side this is now:

| event | meaning |
|---|---|
| `becameHost` | the relay promoted this player: it hosts, with the same invite |
| `hostChanged` | another player hosts: this side reconnects to it |
| `becamePlayer` | this side hosted, the relay lost it, and it came back as a player |
| `hostingNewSession` | this side hosted and has no session to go back to: a new invite |
| `playingAgain` | a player that reconnected has the host's word again |

A stall of 5-10 s leaves the host in place: its connections and the players' end, and
the players connect again. The host then gives them new slots, so in that case the
walkers do get new characters.

`tools/walkerBot/migration-check.sh [BOTS] [WINDOWS]` kills the host with SIGKILL,
with a relay at its default timeouts, and fails if a survivor is not back within 13 s,
if not exactly one hosts, on a desync, or if a window makes a new character for a
walker that stayed. Measured on 2026-11-04, one MacBook:

| session | back after the kill | characters made | desyncs |
|---|---|---|---|
| 3 bots | 10.0 s the new host, 10.2 s the others | not applicable | 0 |
| 2 143 windows, 2 bots | 10.0 s the new host (a window), 10.2-10.3 s the others | 0 | 0 |

A frozen host that comes back was checked once, before characters stayed through a
migration: a 143 host frozen by SIGSTOP for 12 s, with a 143 window and a bot joined.
The window hosted and the old host came back as a player. Case 1341 covers this with short
timeouts, a killed host, a second migration, a host alone, a shorter stall and a host
that quits.

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
