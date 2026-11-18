# Simulation levels: near and far round relevance centers

Status: built (Rae 0.4.23). Modules: `simulation/SimLevelSystem`, `simulation/StandingSpot`.
Case: `compiler/tests/cases/1551_simulation_levels`.

## The problem

A host that keeps a world alive for several players cannot simulate everything in it. Characters
and vehicles near a player need a body, collisions, animation and the ground streamed under them.
Those far from every player only need to go on existing: a place and a few numbers that the game
moves by its own simple rules (a walk along a road, a timer for a delivery). Rae streams the
ground in tiles round centers (`app3d/TileStreaming`), but until now nothing said, per thing,
which of the two it is.

## The model

- A **subject** is anything that can be near or far: an entity id from the app's allocator with a
  `SimRelevance` component (its place on the ground plane and its level). While far, that id and
  the app's own record are all there is; no body, no other component, no ground.
- A **relevance center** is a point the world is simulated round: each player's character or
  camera (`RelevanceCenter { x, y }`).
- **The level** is `near` or `far`. A far subject comes near once its nearest center is within
  `enterDistance`; a near subject goes far once every center is past `leaveDistance`. The two are
  apart (hysteresis), so a subject at the edge does not change by turns.
- **Events.** `simLevelSystem` sends one `CameNear` or `WentFar` per change, in the system's own
  event queues, cleared on its next run. The app answers them:
  - `CameNear`: make the full entity from the record (its body, its components), at the record's
    place or at a free spot out of sight (below).
  - `WentFar`: write back into the record what it needs (its place, its state) and remove the
    full entity's body and components. Keep the id: it is the record's.

A new subject starts far, so one placed near a center comes near on the first run. Distances are
horizontal (x, y), like the tile streaming's.

```rae
var levels: SimLevelSystem = createSimLevelSystem(
  settings: SimLevelSettings { enterDistance: 60.0, leaveDistance: 75.0 }
)
let id: EntityId = allocEntity(this: allocator)
addSimSubject(system: levels, entityId: id, x: record.x, y: record.y)
...each frame, after moving the records and the full entities:
moveSimSubject(system: levels, entityId: id, x: place.x, y: place.y)
simLevelSystem(system: levels, centers: playerCenters)
loop over levels.cameNear: build the full entity
loop over levels.wentFar: save the record, remove the full entity
```

`simLevelOf(system, entityId)` asks one subject's level; `system.nearCount` is how many are near.

## With TileStreaming's centers

Use the same points for both: the centers handed to `streamTilesAround` / `streamTilesPrimary`
(`TileCenter`) and to `simLevelSystem` (`RelevanceCenter`) are the players. Then:

- **The ground must be there before a subject comes near.** Keep `enterDistance` at most the
  tiles' `loadDistance` less one tile, so the tile under a subject is loaded when it comes near
  (with `streamTilesPrimary`, the other centers' `otherLoadDistance`).
- **A near subject must not lose its ground.** Keep `leaveDistance` below the tiles'
  `unloadDistance`, so a subject goes far before the tile under it is let go.
- A tile load is one a frame. A player arriving somewhere new (a respawn, a teleport) has its
  tiles load over several frames; make the subjects round it come near only once their tile is
  loaded, or let the record wait a frame when its `CameNear` finds no ground yet.

## A free spot out of sight: StandingSpot

A subject coming near at the record's place may appear in front of a player, or inside
something. `simulation/StandingSpot` finds a place near a point where a character can stand:

- on the ground (the app's height function),
- not inside a collider (the app's physics overlap query),
- within a search radius,
- and, given a camera, out of its view: behind it, past its reach, outside its cone, or hidden
  behind something (the app's own ray test).

Rae passes no functions, so the search hands out candidates nearest first (the point, then rings
`step` apart) and the app answers each one:

```rae
var search: SpotSearch = beginSpotSearch(x: record.x, y: record.y, radius: 12.0, step: 1.0)
spotSearchAvoidView(search: search, spotView: SpotView {
  eyeX: eye.x, eyeY: eye.y, eyeZ: eye.z, forwardX: look.x, forwardY: look.y, forwardZ: look.z
  halfAngle: 0.9, reach: 200.0
})
loop {
  if let candidate: SpotCandidate = nextSpotCandidate(search: search) {
    let z: Float = groundHeightAt(x: candidate.x, y: candidate.y)
    # a Box3D world: overlapSphereEntities (box3d/ecs/PhysicsQueries) round the body's middle
    let free: Bool = overlapSphereEntities(
      physics: physics
      center: Vec3.create(x: candidate.x, y: candidate.y, z: z + 0.9)
      radius: 0.45
      filter: filter
    ).length is 0
    reportSpot(search: search, z: z, free: free, occluded: false)
  } else {
    break
  }
}
if let spot: StandingSpot = spotSearchResult(search: search) {
  # spot.hidden: out of the view (false when every free spot was in view: the nearest of them)
}
```

Without a view the search ends at the first free spot. With one it ends at the first free spot
out of view, keeping the nearest free spot in view as the answer when there is no hidden one. A
character counts as seen when its feet or its head (`bodyHeight` above the ground, 1.8 m) is in
view. Ground the app does not want stood on (water, too steep) is `free: false`.

## Not in it

- More than two levels (a middle level with simplified physics) and a level per kind of thing.
- Moving far records: their rules are the game's.
- Who sees what over a network (interest management for replication): related, and could use the
  same centers, but it is the net layer's.
