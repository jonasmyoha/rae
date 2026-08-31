# ECS Resources (world-global singletons)

A **World** holds two kinds of state:

- **Components** are *per-entity*. Each is a `ComponentTable(T)` field on the
  World, one row per entity, looked up by `EntityId`.
- **Resources** are *world-global singletons*. There is exactly **one** — so a
  resource is just a **plain field on the World struct**, read directly as
  `world.frame`, `world.input`, `world.layoutW`. No id, no table, no query.

```rae
type GameWorld {
  allocator: EntityAllocator          # entity core (#706)
  positions:  ComponentTable(Position) # a COMPONENT (per-entity)
  frame:      Frame                    # a RESOURCE (one per world)
  input:      Input                    # a RESOURCE
}
```

## Rule: a resource is NEVER an entity

Do **not** model the camera, the input snapshot, the clock, GPU handles, or any
other singleton as a one-off entity carrying a component. That fakes an
entity/table for something there is exactly one of, costs an id + a sparse-set
lookup, and reads worse than a field. A singleton is a field — full stop.

## Serialization

Resources and components obey the **same registry rule** (`lib/ecs/registry.rae`,
#717), just at different granularity:

| kind      | in a save file as            |
|-----------|------------------------------|
| component | `"Name": { "<entityIndex>": value, ... }` (entity→value map) |
| resource  | `"Name": value` (a single top-level value) |

Register with `registerResource` (which sets the `RESOURCE` flag):

```rae
registerResource(reg, name: "Frame", flags: SERIALIZE)  # AUTHORED  -> saved
registerResource(reg, name: "Input", flags: 0)          # RUNTIME   -> omitted
```

- An **authored** resource that belongs in a save is registered
  `SERIALIZE` — it round-trips through `worldToJson` / the load path.
- A **runtime** resource (frame timing, input, GPU/font state like the UI's
  `msdfState`) is registered `RESOURCE` **without** `SERIALIZE`, so save/load
  skips it and it is rebuilt at runtime.

Serialize/deserialize helpers live in `lib/ecs/serialize.rae`:

```rae
# out: value.toJson() in, gated by the SERIALIZE flag
addResource(writer, name: "Frame", valueJson: world.frame.toJson(), registry: reg)
# in:  the resource's JSON string out; feed to the concrete Type.fromJson
world.frame = Frame.fromJson(json: resourceJson(name: "Frame", registry: reg, doc: doc))
```

They are non-generic on purpose (the caller supplies `value.toJson()` and calls
`Type.fromJson`, both concrete) so nothing depends on inferring a bare type
parameter.

## UI example

`UiWorld` (`lib/ui/ecs.rae`) already uses this pattern: `layoutW` / `layoutH`
(the design-unit root extent) and `msdfState` (the MSDF font slot) are plain
fields — resources, not component tables. They are now documented as such; the
behaviour is unchanged.
