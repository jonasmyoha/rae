# Entity replication (networking 7/12)

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
- **Parts.** A snapshot bigger than `maxMessageBytes` (1000) is split into
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
| 4 | 183 bytes | 81 bytes | 20 bytes | 1.6 KB/s |
| 32 | 1,386 bytes (2 parts) | 577 bytes | 18 bytes | 11.5 KB/s |
| 100 | 4,301 bytes (5 parts) | 1,790 bytes | 17 bytes | 35.8 KB/s |

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

## Not yet

- Interpolation and prediction (8/12): the player's world jumps from
  snapshot to snapshot.
- Late join, leave and reconnect (9/12). A late joiner already gets a
  whole snapshot first, because it has acknowledged nothing.
- Per-component send rates and priorities (everything in view is
  considered every snapshot).
- Allocation: capturing and encoding a snapshot allocates (new lookup maps
  per snapshot, the decoder's new snapshot). That is fine at 20 Hz for a
  host with 100 entities and a few players. Making it allocation-free like
  the connection layer is a later step if profiles ask for it.
