# Entity replication (networking 7/12, 8/12, 9/12)

`lib/net/replication`: the host's ECS World is the truth, and each player
keeps a copy of the part of it near the player. The layer runs over
lib/net/Connection (any transport: the relay, a direct path, or a test
moving packets by hand) and knows no component types. The app chooses the
tables to send and gives each a kind number.

## Pieces

| file | what |
|---|---|
| `ReplicaValues.rae` | a component as a list of Ints and back, by compile-time field reflection |
| `ReplicaSnapshot.rae` | a snapshot (records + interest points) and the delta writer |
| `ReplicationHost.rae` | the `Replicated` component, the settings, the host resource (snapshot ring), `ReplicaPeer`, capture |
| `snapshotSystem/SnapshotSystem.rae` | host: one player's delta of the newest snapshot, in parts, as unreliable messages |
| `inputSystem/InputSystem.rae` | host: a player's acks and inputs, `replicaNextInput` |
| `ReplicationPlayer.rae` | the player resource, `replicaSendInput` |
| `receiveSystem/ReceiveSystem.rae` | player: collect a snapshot's parts, rebuild it from its baseline |
| `applySystem/ApplySystem.rae` | player: entities for new net ids, `applyReplicaTable`, gone entities |
| `interpolationSystem/InterpolationSystem.rae` | player: the host-clock estimate, other entities drawn between two snapshots |
| `predictionSystem/PredictionSystem.rae` | player: the own entity from the host's word plus the pending inputs |

## How the app uses it

Host, each snapshot tick (`replicaSnapshotDue`, every 50 ms by default):

```rae
replicaBeginSnapshot(host: host, nowMs: now)
captureReplicaTable(Replicated, host: host, table: world.replicated, replicated: world.replicated, kind: 0)
captureReplicaTable(Transform3D, host: host, table: world.transforms, replicated: world.replicated, kind: 1)
captureInterestPoints(host: host, transforms: world.transforms, replicated: world.replicated)
loop ... each player { replicaSnapshotSystem(host: host, peer: peer, connection: connection) }
```

The host takes every message from a player's connection through
`replicaTakeMessage` (acks and inputs), then calls `replicaNextInput(Input,
...)` until it answers -1. An entity becomes replicated with
`replicateEntity`, which gives it a net id. A player's own entity is its
`peer.focusNetId`.

Player, each frame: give every message to `replicaReceiveSystem`. When
`replicaHasUpdate` is true, call `replicaSyncEntities`, then destroy
`goneEntityIds`, then call `applyReplicaTable(T, ...)` per table, then
`replicaFinishApply`. Finally `replicaSendInput(Input, ...)`, which also
acknowledges the newest snapshot.

`compiler/tests/cases/1272_replication` is the whole loop in about 600
lines, with the world type, the input, and two players.

## What is sent

- **Components as numbers.** `appendReplicaValues` takes the component's
  `Float` fields (quantized: `round(value * floatScale)`, 1/1000 by
  default), then `Int`, `Bool`, `Vec3` (3 values) and `Quat` (4 values).
  Both ends read the same order, so the type is the schema. Other fields
  are not sent, so a replicated component should be a plain struct of
  numbers. (Nested structs of other types could be reached with
  `typeName` folding, `docs/compile-time-reflection.md`. Not needed yet.)
- **Snapshots.** A snapshot is a set of records (net id, kind, values),
  plus each entity's interest point. The host keeps the last
  `historySize` (32) of them.
- **Deltas against the acknowledged snapshot.** For each player, the host
  keeps which net ids it was sent at each tick. A delta is relative to
  the newest snapshot that player acknowledged, when the host still has
  it. It lists changed records (a varint bit mask of the changed values,
  then each difference as a signed varint), components that went, and
  entities that went or left the player's interest. An unchanged record
  costs nothing. With no usable baseline, every record is sent whole.
- **Parts.** Each part's header also carries the player's own net id and
  the newest input tick the host applied for it (prediction). A snapshot
  bigger than `maxMessageBytes` (1000) is split into
  parts, all unreliable. The player uses a snapshot only when every part
  arrived. A lost part costs that snapshot, never a wrong world. The next
  snapshot is then a delta against an older acknowledged one.
- **Interest.** A player gets the entities within `interestRadius` (60) of
  its own entity. An entity with no interest point (no Transform3D, and no
  `replicaSetPoint`) is global state, sent to everyone.
- **Inputs.** Unreliable. Each message repeats the player's last
  `inputRedundancy` (3) inputs with their ticks, so one lost message loses
  none. The host skips inputs older than the newest it took. Inputs use
  the same field encoding as components.

Replication's messages start with kind byte 1 (snapshot part) or 2 (input).
An app's own messages on the same connection use 16 and up.

## Measured (fixture 1272)

Moving entities (position, velocity and a flag change every snapshot), all
in view, no loss:

| entities | first snapshot (whole) | delta per snapshot | per entity | at 20 snapshots/s |
|---|---|---|---|---|
| 4 | 191 bytes | 89 bytes | 22 bytes | 1.8 KB/s |
| 32 | 1,402 bytes (2 parts) | 585 bytes | 18 bytes | 11.7 KB/s |
| 100 | 4,341 bytes (5 parts) | 1,806 bytes | 18 bytes | 36.1 KB/s |

(8/12 added 8 bytes per part: the player's own net id and its newest
applied input tick.)

These bytes are the replication messages, before the connection's own
header and the 24 bytes a sealed packet adds. Entities that do not move
cost nothing per snapshot. A far entity costs nothing until it comes into
view.

The fixture also checks the following. After three seconds at 10% loss
and 40 ms each way, both players' worlds match the host's for every
visible entity, and the 32- and 100-entity worlds do too. A far entity is
not sent until it comes within the radius, and it is removed when it
leaves. A destroyed entity goes from the players. Global state reaches
everyone. The players' inputs move their own entity on the host.

## Interpolation and prediction (8/12)

Other entities are drawn a little in the past. `replicaAdvanceClock`
estimates the host's snapshot clock once a frame. It runs at the snapshot
rate, is steered by 5% of its error toward the newest snapshot that
arrived, and jumps only when it is more than 3 ticks off.
`replicaRenderTick` is that clock minus `interpolationDelayTicks` (2.0),
which is two snapshots, 100 ms at 20 Hz. The snapshot after the render time
has normally arrived, even after a loss and with jitter.
`applyReplicaTableInterpolated` sets each other entity's component from the
snapshots just before and just after the render time:

- `Float`, `Vec3` and `Quat` values are blended. A `Quat` is blended
  component-wise and not renormalized, which is fine for the small turn
  between two snapshots.
- `Int` and `Bool` values are the earlier snapshot's.
- If no snapshot after the render time has arrived yet, the entity holds
  its last value. A new entity starts at its first value.
- The player's own entity is skipped. Entities still come and go with the
  newest snapshot, so a removed entity vanishes about 100 ms before its
  last drawn position.

The own entity is predicted. Each snapshot also carries the player's own
net id and the newest input tick the host applied for it. The player keeps
its inputs until a snapshot says they were applied (`replicaPendingCount`,
`replicaPendingInput`). When a snapshot arrives (`replicaOwnFresh`), the app
sets its entity to the host's state (`applyReplicaOwn`), replays the pending
inputs with the same movement code the host runs, and calls
`replicaOwnTaken`. Each frame it then applies that frame's input at once and
sends it. The movement code is the app's, because Rae has no function
values to hand it in.

Measured in fixture `1278_interpolation_prediction`, over lib/net/BadNetwork
at 10% loss, 40 ms each way and 30 ms of jitter, in 10 ms frames:

- **Remote entities.** Wanderers move 0.04 units a frame. Interpolated, the
  largest step drawn in a frame is about 0.050, and the fixture's bound is
  0.06. The newest snapshot alone moves them 0 most frames and up to 0.40
  at once after a lost snapshot.
- **The own entity.**
  - Its error against the truth (the host's state plus the inputs the host
    has not applied yet) stays under 0.000002 units in normal play, so the
    replay matches the host.
  - When the host moves the entity on its own (a push), the next snapshot
    corrects the prediction 60 ms later. The fixture's bound is 300 ms, and
    the error then stays within 2 mm.
  - Standing still at the end, prediction and host agree.

Found on the way: lib/net/BadNetwork's random generator (a linear
congruential one) produced correlated losses. With one loss draw and one
jitter draw per packet, and two packets per frame, every fourth output
decided one stream's losses, and "10%" loss came in runs of three. That
was exactly a window of three redundant inputs. It is now xorshift32,
whose runs match independent loss (89 runs of three in 100,000 sends,
where independent loss gives about 90).

## Join, leave, reconnect (9/12)

`lib/net/session/Session.rae` keeps the host's player slots. It sits beside
replication, over the same connection:

- **The first exchange.** A player's first reliable message is a hello
  (kind 3) with its rejoin token, all zeros the first time. The host
  answers with a welcome (kind 4): the slot id, whether the slot was
  resumed, and the 16-byte random token to use next time. A new slot gets
  a new entity (the app makes it: `sessionSetEntity`). Its replication peer
  is fresh, so its first snapshot is whole.
- **Leaving.** A connection that ends (the player disconnected on purpose)
  frees its slot at the next `sessionExpired`, and the app removes the
  entity. The entity disappears from every other world with the next
  snapshot. Nobody else's connection or world is touched.
- **Dropping out.** A connection that times out (an app sent to the
  background, a network change) keeps its slot for `graceMs` (10 s in the
  fixture). A hello with that slot's token within the grace period resumes
  the slot and its entity, on a new connection, with a fresh peer (a whole
  snapshot). If the old connection has not timed out yet, the new one takes
  the slot over and the host stops serving the old one. After the grace
  period the slot is freed. The token then no longer matches, so the
  player gets a new slot.

Measured in fixture `1279_join_leave_reconnect`, a simulated minute over
lib/net/BadNetwork at 5% loss, 30 ms each way and 10 ms jitter:

- **Joining.** A late joiner is playing, with its own entity in a world
  built from a whole snapshot, 210 ms after its connection starts (310 ms
  when a packet was lost). That is the handshake, the hello and welcome,
  and the next snapshot. The fixture's bound is 500 ms.
- **Comings and goings.** Six players join, leave, go away for 3, 7 and
  17 seconds, and come back while the host keeps running.
- **Worlds.** Every second, every connected world is compared with the
  host's snapshot at the tick the player last applied. That is about 140
  comparisons, with no mismatch.

## Not yet

- Smoothing a visible correction (an error offset that decays). The
  prediction snaps to the corrected position. Correction is rare with the
  same movement code on both sides, but a push from the host is visible.
- Late join, leave and reconnect (9/12). A late joiner already gets a
  whole snapshot first, because it has acknowledged nothing.
- Per-component send rates and priorities (everything in view is
  considered every snapshot).
- Allocation: capturing and encoding a snapshot allocates (new lookup maps
  per snapshot, the decoder's new snapshot). That is fine at 20 Hz for a
  host with 100 entities and a few players. Making it allocation-free like
  the connection layer is a later step if profiles ask for it.
