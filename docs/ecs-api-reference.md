# lib/ecs — API reference

The reusable ECS core, extracted out of `lib/ui/` so every Rae program (UI, 3D,
gameplay, headless) builds on the same entities, component tables, queries,
systems, registry and serialization. Rationale and the "ECS is Rae's default
architecture" argument live in `ecs-general-architecture.md`; this file is the
practical API surface, verified against `lib/ecs/*`.

Conventions: a component is any plain `type`; a **table** stores one component
kind for many entities; a **World** is an app-defined struct that embeds one
`EntityAllocator` plus one `ComponentTable(T)` field per component (`UiWorld`,
`World3d`, the 116 `GameWorld` are all this shape). Generic functions take the
element type as a leading `T: type` argument that is inferred from the table at
the call site — you write `componentGet(this: table, entity: e)`, not the `T`.

## Entities & handles — `lib/ecs/entity.rae`, `lib/ecs/world.rae`

An entity is a generational handle, never a bare index:

```rae
type EntityId { index: Int, generation: Int }
func noEntity() ret EntityId                 # the null handle (index -1)
func entityId(v: view Int) ret EntityId      # wrap a raw index (generation 0)
func isNone(this: view EntityId) ret Bool
```

`EntityAllocator` is the whole lifecycle core — index recycling, the generation
table, and an O(1) dense live-set:

```rae
func createEntityAllocator() ret EntityAllocator
func allocEntity(this: mod EntityAllocator) ret EntityId    # reuse a freed slot (bumped gen) or a fresh index
func freeEntity(this: mod EntityAllocator, entity: view EntityId)   # bump generation, recycle index, O(1) drop from alive
func allocatorIsAlive(this: view EntityAllocator, entity: view EntityId) ret Bool
func allocatorAliveCount(this: view EntityAllocator) ret Int
func allocatorGeneration(this: view EntityAllocator, index: view Int) ret Int
```

Iterate live entities via `world.allocator.alive` (a dense `List(EntityId)`).
Recycling + generation is what lets a freed handle compare unequal to any future
entity reusing its slot (`687`, `681`, `682`).

## Component tables — `lib/ecs/component_table.rae`

A sparse set: O(1) add / get / remove by `EntityId`, dense contiguous storage for
iteration, a table-level `generation` counter, and per-entity modify stamps.

```rae
type ComponentTable(T: type) { ... }
func createComponentTable(T: type) ret ComponentTable(T)

func componentSet(this: mod ComponentTable(T), entity: view EntityId, data: own T)
func componentHas(this: view ComponentTable(T), entity: view EntityId) ret Bool
func componentGet(this: view ComponentTable(T), entity: view EntityId) ret T          # a COPY
func componentView(this: view ComponentTable(T), entity: view EntityId) ret view T    # read-only alias
func componentMod(this: mod ComponentTable(T), entity: view EntityId) ret mod T        # write-through alias
func componentRemove(this: mod ComponentTable(T), entity: view EntityId)               # ordered? no — swap-remove
func componentCount(this: view ComponentTable(T)) ret Int

# Dense iteration (index 0..count):
func componentEntityAt(this: view ComponentTable(T), i: view Int) ret EntityId
func componentDataAt(this: view ComponentTable(T), i: view Int) ret T

# Change tracking:
func componentTableGeneration(this: view ComponentTable(T)) ret Int    # bumped on any set/mod/remove
func componentModStamp(this: view ComponentTable(T), entity: view EntityId) ret Int
```

`componentMod` returning a live `mod T` is the #1 ECS ergonomic — drive an entity
in place, no copy-out/mutate/set-back. Dense order is insertion order UNTIL a
`componentRemove` swap-removes (which moves the last row into the hole); systems
that need a stable order use the hierarchy `depthOrder` instead.

## Queries — `lib/ecs/query.rae`

Join tables on the entities they share, probing the smallest table:

```rae
type Query2Match { entity: EntityId, denseA: Int, denseB: Int }
func query2(A: type, B: type, tableA: view ComponentTable(A), tableB: view ComponentTable(B)) ret List(Query2Match)
type Query3Match { entity: EntityId, denseA: Int, denseB: Int, denseC: Int }
func query3(A: type, B: type, C: type, tableA: ..., tableB: ..., tableC: view ComponentTable(C)) ret List(Query3Match)
```

Each match carries the dense indices, so you fetch the row with the accessors
without a second lookup:

```rae
loop let hit: view Query2Match in query2(tableA: world.positions, tableB: world.velocities) {
  let pos: mod Position => queryModAt(table: world.positions, denseIndex: hit.denseA)
  let vel: view Velocity => queryViewAt(table: world.velocities, denseIndex: hit.denseB)
  pos.x = pos.x + vel.dx
}
func queryModAt(table: mod ComponentTable(T), denseIndex: view Int) ret mod T
func queryViewAt(table: view ComponentTable(T), denseIndex: view Int) ret view T
```

`queryTagged(D, T, dataTable, tagTable)` is `query2` specialised to "data table
filtered by a zero-field tag". `forEach` / `forEachView` iterate one table.

## Tags — `lib/ecs/tag.rae`

A tag is a zero-field marker component (`type FooTag {}`); its table stores
presence only:

```rae
func addTag(table: mod ComponentTable(T), entity: view EntityId)
func removeTag(table: mod ComponentTable(T), entity: view EntityId)
func hasTag(table: view ComponentTable(T), entity: view EntityId) ret Bool
func tagCount(table: view ComponentTable(T)) ret Int
func taggedEntities(table: view ComponentTable(T)) ret List(EntityId)
```

## Hierarchy — `lib/ecs/hierarchy.rae`, `lib/ecs/HierarchySystem/`

`Children` (the ordered child list on the parent) is AUTHORITATIVE and serialized;
`Parent` is the DERIVED reverse index (#769). Callers never edit either table by
hand — `setParent` (append) and `insertChild` (explicit index) are the only
mutation entries and keep both sides consistent:

```rae
type Children { ids: List(EntityId) }     # authoritative, ordered
type Parent { parent: EntityId }          # derived reverse index

func setParent(parents: mod ComponentTable(Parent), childrens: mod ComponentTable(Children), child: view EntityId, parent: view EntityId)
func insertChild(parents: ..., childrens: ..., parent: view EntityId, child: view EntityId, index: view Int)
func getChildren(childrens: view ComponentTable(Children), parent: view EntityId) ret List(EntityId)   # a COPY
func rebuildParentsFromChildren(parents: mod ComponentTable(Parent), childrens: mod ComponentTable(Children))  # after a bulk load / self-heal
func detachForDestroy(parents: ..., childrens: ..., entity: view EntityId)     # unlink one node, orphan its children
func destroySubtree(allocator: mod EntityAllocator, parents: ..., childrens: ..., root: view EntityId)
```

`HierarchyOrder` caches a parent-before-child topological order, dirty-tracked on
the Children table generation, so systems iterate a flat list once instead of
pointer-chasing the tree:

```rae
func createHierarchyOrder() ret HierarchyOrder
func depthOrder(state: mod HierarchyOrder, parents: view ComponentTable(Parent), childrens: view ComponentTable(Children)) ret view List(EntityId)
```

## Transforms — `lib/ecs/transform.rae`, `lib/ecs/TransformSystem/`

`Transform2D`/`Transform3D` are the local transforms; `WorldTransform2D`/`3D` the
composed world transforms a system derives:

```rae
func transformSystem3D(orderState: mod HierarchyOrder,
                       transforms: view ComponentTable(Transform3D),
                       worldTransforms: mod ComponentTable(WorldTransform3D),
                       parents: view ComponentTable(Parent),
                       childrens: view ComponentTable(Children))
func transformSystem2D(...)   # same shape for the 2D pair
```

Pass 1 sets every root's world = local; pass 2 walks `depthOrder` composing each
child onto its parent's world transform. `PrevTransformSystem/` keeps the
previous-frame transforms (motion vectors / TAA) without manual lockstep.

## Registry & serialization — `lib/ecs/registry.rae`, `lib/ecs/serialize.rae`

The registry maps a component/resource NAME to its flags; the flags decide what a
generic serializer does — NOT a per-name switch and NOT a type annotation:

```rae
const serialize: Int = 1     # persist to save files
const replicate: Int = 2     # send over the network
const editorOnly: Int = 4    # exists only in the editor
const resource: Int = 8

func createComponentRegistry() ret ComponentRegistry
func registerComponent(reg: mod ComponentRegistry, name: view String, flags: view Int)
func registerResource(reg: mod ComponentRegistry, name: view String, flags: view Int)
func registeredHasFlag(reg: view ComponentRegistry, name: view String, flag: view Int) ret Bool
```

Serialize a world to JSON, one fragment per table, each gated by the registry
flag and written through the compiler-generated per-struct `toJson()`/`fromJson()`
(which recurse into nested structs and List fields, #768):

```rae
func createWorldJson() ret WorldJson
func addComponentTable(writer: mod WorldJson, table: view ComponentTable(T), name: view String, registry: view ComponentRegistry)
func finishWorldJson(writer: view WorldJson) ret String
func loadComponentTable(table: mod ComponentTable(T), name: view String, registry: view ComponentRegistry, doc: view JsonDoc)
func addResource(writer: mod WorldJson, name: view String, valueJson: view String, registry: view ComponentRegistry)
func resourceJson(name: view String, registry: view ComponentRegistry, doc: view JsonDoc) ret String
```

## Whole-world helpers via field reflection (#760)

The per-table sweeps a whole-world op needs — clear an entity from every table,
serialize every table — are written ONCE, generically, with the compile-time
field loop (`docs/compile-time-reflection.md`), NOT a compiler-synthesised builtin
and NOT an `@`-attribute:

```rae
# lib/ecs/world.rae — one definition for every World.
func clearEntityComponents(W: type, world: mod W, entity: view EntityId) {
  loop let table: mod ComponentTable(any) in fields(world) {
    componentRemove(this: table, entity: entity)
  }
}
```

`fields(world)` unrolls at compile time to one statement per `ComponentTable`
field (non-table fields — the allocator, hierarchy caches, resources — are skipped
by the `ComponentTable(any)` filter); `W` is inferred from the argument. The same
form drives registry-gated serialize (`worldToJson` = `loop ... in fields(world) {
addComponentTable(name: fieldName(table), ...) }`). `destroyEntity` (UiWorld) and
the per-system despawn helpers call `clearEntityComponents`; UiWorld's machine
snapshot (`lib/ui/serialize.rae`) and the 116 `GameWorld` snapshot use the loop.
There is **no** compiler-synthesised per-World helper — the capability is expressed
in Rae via a general reflection primitive.

## Resources — `lib/ecs/world.rae`, `docs/ecs-resources.md`

A resource is a world-global singleton (one Clock, one input snapshot), so it is a
PLAIN FIELD on the World struct, not a per-entity component — no id, no table, no
query. It serializes as a single top-level value (`addResource`/`resourceJson`),
gated by the same registry rule (`registerResource(reg, name, serialize)` for an
authored resource; `flags: 0` for a runtime one that is rebuilt each run).

## Events & queues — `lib/ecs/event.rae`, `lib/ecs/queue.rae`

```rae
func createEventQueue(T: type) ret EventQueue(T)
func eventSend(this: mod EventQueue(T), value: own T)
func eventCount(this: view EventQueue(T)) ret Int
func eventAt(this: view EventQueue(T), i: view Int) ret T
func eventFrameAdvance(this: mod EventQueue(T))   # double-buffer: this frame reads last frame's events
```

`Queue(T)` is a plain FIFO (`enqueue`/`queueAt`/`clearQueue`). Events are never
persisted.

## Schedule — `lib/ecs/schedule.rae`

An ordered list of named systems with a per-system dirty-gate, so a system runs
only when the generation it reads has moved:

```rae
func createSchedule() ret Schedule
func addSystem(schedule: mod Schedule, name: view String) ret Int
func scheduleShouldRun(schedule: mod Schedule, index: view Int, readGeneration: view Int) ret Bool
```

The manual per-frame call order in the examples is the seam a richer scheduler
would own (see the wishlist).

## See also

- `ecs-general-architecture.md` — why ECS is Rae's default architecture.
- `compile-time-reflection.md` — the `fields()` / `fieldName()` language feature.
- `ecs-resources.md`, `ecs-systems-and-data-observation.md` — resources and change observation.
- `ecs-language-wishlist.md` — the language features that would make ECS in Rae cleaner still.
