# ECS as Rae's general architecture — design recommendation

Status: **adopted design**; implementation is tracked as QUEUE tasks #700–#757.
Verified against the current code (citations inline). This challenges the current
architecture; it does not rationalize it.

## 0. Verdict

Rae already has a real, good ECS — but it is **filed under the UI** and shaped for
the UI, and **the 3D/gameplay examples do not use it**. Example 114 instead
hand-rolls a *worse* ECS (`Scene3d`: entity = `Int` index into parallel
component `List`s, with manual `prevTransforms` lockstep and a manual `revision`
counter — `lib/scene3d.rae:142-155`). So today's reality is exactly the thing to
avoid: **"ECS for UI, Lists for games."**

Decision: **ECS is Rae's default architecture for programs** — not a pattern
for games, not a pattern for UI, but the way a Rae program is structured. Extract
the ECS core out of `lib/ui/` into `lib/ecs/`, add the capabilities a general
program needs (generational entities, multi-component queries, resources, tags,
events, a schedule, a reusable `World` shape, serialize-out), and let the UI, the
3D examples, and every future application build on it. `Scene3d`'s
struct-of-arrays, its parallel-list bookkeeping, and its manual dirty/revision
tracking are then **subsumed by the ECS**, not reimplemented.

The language needs **no** new features for this. In particular: no `persistent` /
`runtime` / `serialized` / `replicated` type annotations — serialization,
networking and editor-only state are all **framework registration**, built on the
per-struct `toJson`/`fromJson` the compiler already generates
(`compiler/src/c_backend.c:1841`).


### 0.1 Why ECS is Rae's architecture

Rae's stated goal is a language that is easy for **humans and machines** to read,
analyze, and transform: explicit data, explicit control flow, no hidden state, no
clever ambiguity (AGENTS.md). ECS is that goal applied to program *structure*:

- **Data is explicit and flat.** State lives in typed component tables, not in
  object graphs threaded through pointers. A tool or an AI agent can enumerate
  every kind of state a program has by reading the `World` struct.
- **Behavior is explicit and separable.** A system is a plain function over the
  world with a declared read/write set (the tables it touches). Nothing runs that
  is not in the schedule; the schedule is the program.
- **Identity is a value.** An entity is a small copyable handle, never a pointer
  into someone's heap — which is exactly what Rae's ownership model wants
  (`view`/`mod` alias storage; `=` copies; no shared mutable pointers).
- **It composes without inheritance.** Rae has no classes; ECS's "compose
  behavior by attaching components" is the only large-scale composition story the
  language needs.
- **It is analyzable.** Table-level `generation` stamps already give exact
  change-detection; queries make data dependencies between systems visible; a
  future scheduler, profiler, editor, or hot-reload can be built on that, not on
  reflection.

The language work of the last epic was, in effect, building ECS primitives:
`Array(T, cap: N)` value aggregates, `copyAt`/`viewAt`/`modAt` naming copy vs
alias, struct-rep `opt T` with no heap traffic, storage-aliasing `view`/`mod`
bindings (#654/#656/#662/#663), and the borrow check on aliased elements (#658).
ECS is where those primitives pay off.

### 0.2 The four nouns (the rule for every Rae program)

- **Entity** — anything with **identity and a lifetime** you spawn and destroy: a
  UI node, a metaball, a crowd member, a terrain prop, a network peer, a job.
- **Component** — **data about one entity**: `Transform3D`, `Material3D`,
  `ScrollState`, `AnimationState`. Vec/math value types, never scalar sprawl.
- **System** — **behavior over components**, a function in its own PascalCase
  folder, run by the schedule: `TransformSystem`, `LayoutSystem`, `RenderSystem`.
- **Resource** — **world-global state with exactly one instance**: the frame
  clock, input, the camera, the layout viewport, GPU handles. A plain field on the
  `World`, not an entity pretending to be one.

Corollary — where `List` still belongs: **inside** a component as bulk data with
no per-item identity (a mesh's vertices, a string's bytes, a grass instance
stream *derived* from blade components). The moment items need identity,
lifetime, or per-item behavior, they are entities. "Structure-of-arrays kept in
sync by index" is always an entity table in disguise.

## 1. Current state (verified)

### The ECS core — good, but UI-located and UI-shaped
- `EntityId { value: Int }` — a **bare counter, no generation/version**
  (`lib/ui/ecs.rae:7`, `:414-416`); `createEntity` never recycles ids
  (`ecs.rae:615`); `isAlive`/destroy splice are O(n) over `world.alive`
  (`ecs.rae:624`, `:884`).
- `ComponentTable(T)` — a proper **typed sparse set**, O(1) add/get/remove, with a
  table `generation` and per-entity `denseStamps` change-detection
  (`ecs.rae:112-135`, `:171-194`). This is the strong part and worth keeping
  verbatim.
- **Registration is hardcoded in four places** per component: a field on
  `UiWorld` (`ecs.rae:424+`), a construct in `createUiWorld` (`ecs.rae:530+`), a
  `componentRemove` line in `destroyEntity` (`ecs.rae:807-881`), and a deser arm
  in `registry.rae` (`:775+`). The code names this tax explicitly
  (`ecs.rae:406-411`): "no reflection, no first-class type ids."
- **No multi-component query, no archetypes.** Joins are hand-written nested
  `componentHas` over `world.alive` (`layout.rae:833-835`).
- **Systems are plain functions**, ordered by call site; dirty-skip is a
  per-system wrapper caching table `generation` (`layout.rae:931`).
- **Serialization is deserialize-only** (`registry.rae` dispatch + ~50 `deserX`
  readers); `lib/json.rae` is parser-only (no builder/stringify). But the
  compiler *does* auto-generate `toJson`/`fromJson` for every struct
  (`c_backend.c:1841`) — the ECS just doesn't use it.

### UI — a genuine ECS that fights ECS in specific spots
- Real `UiWorld` + ~65 component tables; layout/transform/render/hit-test are
  systems over the world (`layout.rae:827`, `transform.rae:142`,
  `render_gpu2d.rae:577`). Only the JSON parse layer is a struct tree, and it is
  transient (`scene.rae:40-53` → dissolved into entities in `scene_loader.rae`).
- Fights ECS: (a) hierarchy is **recursive `Children` pointer-chasing** in every
  pass rather than linear iteration over a depth-sorted order — the table
  docstring admits "~a dozen lookups/entity" per frame (`ecs.rae:117-118`);
  (b) `Parent`+`Children` are both stored and can desync, with **no referential
  integrity** on destroy (`ecs.rae:798-804`); (c) helper structs bolted outside
  the world and keyed by **strings** (`ScrollPanel` re-resolves `viewportId`
  strings every frame, `scroll_panel.rae:31,54`); (d) **scalar geometry** —
  `Rect{x,y,w,h}`, `WorldTransform{x,y,scaleX,scaleY,rotation,alpha}`
  (`components.rae:150`, `:765`) — with `Vec2` used only for offsets/pivots and
  even abused as a size tuple (`layout.rae:406`); (e) a **duplicate `Vec2`** in
  `lib/ui/components.rae:126` shadowing `lib/vec2.rae`.

### 3D examples — not ECS
- 112 and 114 `open ui/ecs` **only for their 2D UI**. All 3D state is structs +
  `List`s.
- `Scene3d` (`lib/scene3d.rae:142`) is a hand-rolled SoA: `transforms`,
  `prevTransforms`, `materials`, `meshRenderers`, `sdfPrimitives`, each row keyed
  by `entity: Int`. It is an ECS with none of the ergonomics — the app must keep
  `prevTransforms` "in lockstep" and bump `revision` by hand (`:151-155`).
- 114's "systems" are **modules holding a State struct + update/render over
  Lists** (`terrain.rae:83/307`, `grass.rae:47/89`, `render.rae`, `physics.rae`
  stub, `net.rae` stub) — none iterate entities.
- Good news for the migration: `Transform3d = { position: Vec3, rotation: Vec3,
  scale: Vec3 }` (`lib/scene3d.rae:68`) is **already Vec-based** — the correct
  component shape. `Mat4` appears only as a derived/cached world transform
  (`character.rae:135`). No general scene-graph parenting exists (absolute
  transforms); skeleton hierarchy is internal to library types; there is a camera
  rig relation (`camera.rae:85`).
- Where it fights ECS: 11 parallel entity-pool `List`s synced by `poolIndex`
  (`terrain.rae:92-103`) plus a manual `broadVisible: List(Bool)` side-table that
  is "a component a query would carry" (`terrain.rae:104-110`); `partIds`/
  `partRough` parallel arrays (`app.rae:152-153`); crowd **copy-out → mutate →
  set-back** churn because "lists hand back a value, not a reference"
  (`app.rae:590-599`); per-frame O(n) scans that branch on a `cluster` tag with a
  7-way `if` ladder (`scene_animation.rae:60-168`) — a query-by-tag; and fixed
  pools + scale-to-0 "culling" standing in for real spawn/despawn
  (`terrain.rae:159-163`).

## 2. Recommended architecture

### 2.1 One ECS framework, two (or more) worlds
Extract the core into `lib/ecs/` (`EntityId`, `ComponentTable(T)`, `World`
helpers, query, hierarchy, serialization). UI and gameplay each instantiate their
own **`World` struct** but share all the code. A `World` in Rae is a
**compile-time struct of `ComponentTable(T)` fields** — that struct *is* the
component registry (Rae has no reflection, and we are **not** adding any). This is
already how `UiWorld` works; the change is to make the pattern reusable and to cut
the per-component boilerplate.

Two worlds (a `UiWorld` and a `GameWorld`) is fine and normal — UI nodes and
game entities are different domains. The point is that they run on the **same
ECS**, not that they share one entity space.

### 2.2 Entities — add generational handles
`EntityId { index: Int, generation: Int }` plus a free-list that recycles indices
and bumps the slot generation on destroy. Games spawn/despawn constantly
(arrows, pickups, crowd, multiplayer joins — all stubbed today,
`net.rae:17-21`); a bare counter that never recycles (`ecs.rae:615`) leaks index
space and gives a persisted handle no validity signal. `isAlive` becomes O(1)
(compare slot generation) instead of the current O(n) scan (`ecs.rae:624`). UI can
ignore generations; games rely on them.

### 2.3 Components — Vec/math types, never scalar sprawl
- Standardize on `Transform2D` and `Transform3D` as **separate** components:
  ```rae
  type Transform2D { position: Vec2, rotation: Float, scale: Vec2 }
  type Transform3D { position: Vec3, rotation: Quat, scale: Vec3 }   # Quat, not Euler Vec3
  ```
  114's `Transform3d` is the template; upgrade its `rotation` from Euler `Vec3`
  to `Quat` (`lib/quat.rae` exists) — its own comment already flags this.
- The UI's scalar `Rect`/`WorldTransform`/`ComputedRect` should move onto `Vec2`
  (position/size) — one `Rect { position: Vec2, size: Vec2 }` and a
  `Transform2D` instead of x/y/scaleX/scaleY/rotation loose floats. This also
  makes the UI's own transform a `Transform2D`, unifying 2D UI and 2D gameplay.
- Delete the duplicate `Vec2` (`components.rae:126`); use `lib/vec2.rae`.
- No artificial `x`/`y`/`z` members where a `Vec2`/`Vec3` says it.

### 2.4 Transform & hierarchy — `Children` authoritative (ordered), `Parent` derived
- Components: `Transform2D`, `Transform3D`, `Parent { parent: EntityId }`,
  `Children { ids: List(EntityId) }`. No separate `Hierarchy` component (nothing
  in the investigation justifies one).
- **`HierarchySystem/` owns parent/child.** `Children` (the ordered child list)
  is authoritative and is what persists; `Parent` is a **derived reverse index the
  system maintains** and rebuilds (`rebuildParentsFromChildren`). Flipped from the
  original Parent-authoritative plan in #769: sibling ORDER is real layout/paint
  information that lives only in `Children`, and component tables are swap-remove,
  so a Parent-driven rebuild would scramble siblings after any destroy. Game code
  never edits either table by hand: `setParent` (append) and `insertChild`
  (explicit index) are the only mutation entries (the pre-ECS UI stored both and
  warned they could desync — `ecs.rae:798-804`).
  `HierarchySystem` also enforces referential integrity: on destroy it repairs
  dangling `Parent`/`Children` edges (the gap at `ecs.rae:798-804`).
- **`TransformSystem/` owns transform processing.** It composes local
  `Transform2D`/`Transform3D` with the parent's world transform into a derived
  `WorldTransform2D`/`WorldTransform3D` (a `Mat4`/`Mat3` cache), driven by
  `HierarchySystem`'s ordering. This replaces UI's recursive `transformSystem`
  (`transform.rae:142`) and 114's per-frame `walkerWorldModel` matrix rebuild
  (`character.rae:135`) with one system, and it iterates a **depth-sorted entity
  order** (built once from the hierarchy) instead of pointer-chasing `Children`.

### 2.5 Systems — one folder each, exact PascalCase name
Every system is its own folder whose name is the exact system type, with a
primary file of the same name (this composes with Rae's project-folder namespaces
— each system folder is already a namespace, so `TransformSystem.update(...)` is
available with no import):
```
TransformSystem/TransformSystem.rae
HierarchySystem/HierarchySystem.rae
RenderSystem/RenderSystem.rae
```
No lowercase, snake_case, plurals, or abbreviations — `TransformSystem`, not
`transform`/`xform`/`TransformSys`. A system is a function over a `World`. Systems
run from a **`Schedule`**: an ordered list of system entries the app builds once
and runs every frame. The schedule generalizes today's per-system
`*IfDirty` wrappers (`layout.rae:931`, `transform.rae:209`) into one
mechanism — each entry declares the tables it reads, and the schedule skips it
when none of their `generation` stamps moved. Ordering is explicit data (the
list), not a dependency solver; the schedule is where a future profiler,
hot-reload, or parallel runner attaches. The current UI systems
(`layoutSystem`, `transformSystem`, `renderSystemGpu2d`, …) become
`LayoutSystem/`, `TransformSystem/`, `RenderSystem/`, etc.; 114's `terrain.rae`/
`grass.rae`/`render.rae` become `TerrainSystem/`, `GrassSystem/`, `RenderSystem/`.

### 2.6 Queries — a real multi-component iterator
Add `lib/ecs` query helpers that iterate the **smallest** matching table and probe
the rest, yielding `(EntityId, mod A, mod B, …)`:
```rae
loop each in query2(world.transforms, world.velocities) {
  each.a.position = each.a.position.add(each.b.value.scale(dt))
}
```
This removes the hand-written `componentHas` ladders (`layout.rae:833`) and 112's
`cluster` `if`-ladder scan (`scene_animation.rae:60-168`), and it fixes 114's
parallel-array joins (`partIds`/`partRough`, the 11 terrain pools) by making them
components you query instead of indices you keep in sync. Archetypes are **not**
needed yet — sparse-set + smallest-table probe is enough for these workloads; add
archetypes only if profiling a real query demands it.

### 2.7 Serialization/replication/editor — registration, not language
Serialization is a **framework** concern. Build a component registry that records,
per component type, an optional `save`/`load` pair (the compiler-generated
`toJson`/`fromJson`) plus flags:
```rae
registerComponent(reg, name: "Transform3D", save: transform3dToJson, load: transform3dFromJson,
                  flags: SERIALIZE or REPLICATE)
# Children, WorldTransform, ComputedRect, MeasuredSize: registered with NO SERIALIZE flag
# (derived/cache) -> never written, always rebuilt by their system.
```
- **Serialize-out** (the missing half of `registry.rae`) becomes: for each entity,
  for each registered component with `SERIALIZE`, call its `toJson`. Derived
  components (`Children`, `WorldTransform*`, `ComputedRect`, `MeasuredSize`,
  `RuntimeOffset`) are simply not registered as serializable, so the "don't
  persist runtime/cache state" requirement falls out of the registry with **no
  type annotation**.
- **Networking/replication** reuses the same registry with a `REPLICATE` flag and
  the same `toJson`/`fromJson` (or a compact binary the compiler can also emit).
  **Editor-only** state is a `EDITOR_ONLY` flag. All three are one mechanism —
  a bitmask on the registration entry — and **zero** language syntax, honoring
  the decision.
- Prerequisite: give `lib/json.rae` a **document builder / stringify** (it is
  parser-only today), so `toJson` has somewhere to write. This is the one real
  library gap for round-tripping (editor save).


### 2.8 Resources — world-global singletons, named as such
Every real program has state with exactly one instance: the frame clock, input,
the active camera, the layout viewport (`layoutW`/`layoutH`, `ecs.rae:520`), GPU
handles, the MSDF font state. Today these are ad hoc fields on `UiWorld` or loose
structs beside the `App` (`Frame`, `InputUi`, `CameraState`, `PersistState` in
114). Name the concept: a **resource** is a plain field on the `World`, accessed
as `world.frame`, `world.input`. Systems take the world and read/write resources
directly. No entity, no table, no registry — but the *word* matters so nobody
models the camera as a component on a fake entity, and so serialization can treat
resources deliberately (a resource is registered for `SERIALIZE` like a component
when it is authored state, and omitted when it is runtime state).

### 2.9 Tags and events
- **Tags** (marker components) are zero-field structs in a `ComponentTable`:
  `type Enemy {}` + `world.enemies`. They replace integer kind fields branched on
  with `if` ladders (112's `cluster`, `scene_animation.rae:60-168`; 114's
  `broadVisible`). **Verified gap:** an empty struct does not compile today (the C
  backend emits an empty C struct). Making `type Tag {}` legal — lowering to a
  one-byte C struct — is a small compiler fix, not new syntax (QUEUE #751).
- **Events** are values with no identity, produced by one system and consumed by
  others within a frame or the next. The existing `Queue(T)` ("ECS system inbox",
  `ecs.rae:346-396`) is the right primitive; formalize it as an `EventQueue(T)`
  resource on the world, double-buffered, drained by systems in schedule order
  (QUEUE #754). Events are never persisted.

### 2.10 Cutting the per-component boilerplate (the real friction)
Adding one component today edits four places in lockstep (`ecs.rae:406-411`): a
`World` field, a construct call, a `destroyEntity` remove line, and a
deserialization arm. For "ECS is how you write any program" this is the single
biggest tax, and it is also the most error-prone (miss the remove line and the
component leaks on entity death). The compile-time `World` struct is already the
registry; the missing piece is **compiler-synthesized per-World helpers**, in the
exact spirit of the per-struct `toJson`/`fromJson`/deep-copy/drop the compiler
already generates (`c_backend.c:1841`): for a struct whose fields are
`ComponentTable(T)`, synthesize `destroyEntity` (remove from every table) and
registry population. That is generated code, **not** new syntax or reflection, and
it takes the tax from four places to one (declare the field). QUEUE #756
investigates and prototypes it before the UI/example migrations lock in the
four-place pattern at scale.

## 3. Concrete mapping

### UI
| Role | Today | Recommended |
|---|---|---|
| Entity | UI node (already an entity) | unchanged |
| Component | ~65 tables (`components.rae`) — good | keep; move geometry onto `Vec2` (`Rect{position,size}`, `Transform2D`); drop duplicate `Vec2` |
| System | `layoutSystem`/`transformSystem`/`renderSystemGpu2d`/… | `LayoutSystem/`, `TransformSystem/`, `RenderSystem/`, `HierarchySystem/` (owns Parent/Children) |
| Becomes ECS storage | `ScrollPanel`, `UiInput`, string `nodeIds` | `ScrollState`/`Interaction` components keyed by `EntityId`, not strings |
| Already good | sparse-set tables, change-detection, dirty-skip systems | keep as the shared core |
| Fights ECS | recursive `Children` walks; `Parent`/`Children` desync; string identity; immediate-mode paint | depth-sorted iteration; `HierarchySystem` maintains `Children`; entity identity; a `DrawCommand` extract buffer |

### 112 (metaballs / deferred)
- **Entities:** each metaball/cube/torus/prop (today `SdfPrimitive`/`MeshRenderer`
  rows keyed by `entity: Int`, `scene3d.rae:114`).
- **Components:** `Transform3D`, `PrevTransform3D`, `Material3D`, `SdfPrimitive`,
  `MeshRenderer`; **`cluster: Int` becomes a tag/marker component** queried
  instead of branched on.
- **Systems:** `AnimationSystem/` (replaces the `cluster` `if`-ladder with
  per-tag queries), `RenderSystem/`, `PrevTransformSystem/` (replaces manual
  `snapshotPrevTransforms`).
- `Scene3d`'s SoA lists **are** the component tables — migrate them into a
  `GameWorld` and delete the manual `revision`/lockstep bookkeeping.

### 114 (walker)
- **Entities:** hero, each `CrowdMember`, each terrain prop slot, each
  `GrassBlade`, each skinned part, each ground tile.
- **Components:** `Transform3D`, `WorldTransform3D` (derived `Mat4`),
  `ThirdPersonController`, `AnimationState { clipIndex, animTime }`, `Skeleton`/
  `SkeletonPose`, `Material3D`, `partRough` → a `PartRoughness` component,
  `broadVisible` → a `TreeVariant` tag, `GrassBlade`.
- **Systems (one folder each):** `InputSystem/`, `MovementSystem/`,
  `TerrainSystem/`, `GrassSystem/`, `AnimationSystem/`, `RenderSystem/`,
  `PhysicsSystem/` (stub→real), `NetSystem/` (stub→real), `CameraSystem/`,
  `HierarchySystem/`, `TransformSystem/`.
- **Becomes ECS storage:** the 11 parallel `InfiniteTerrain` pools →
  entities with `Transform3D` + `TreeVariant`/`PropKind` tags queried per pass;
  `partIds`/`partRough` → one component; `crowd: List(CrowdMember)` → entities
  with `mod` component access (kills the copy-mutate-set churn, `app.rae:592`).
- **Resources (not entities):** `Frame`, `InputUi`, `CameraState`,
  `PersistState`, the `Scene3d` GPU handles — one instance each, plain fields
  on the `GameWorld`.
- **Already good:** `Transform3d` is Vec3-based; the module-per-system split is
  the right shape — it just needs to iterate entities, not Lists.
- **Fights ECS:** everything keyed by `poolIndex`; fixed pools instead of
  spawn/despawn; manual `prevTransforms` lockstep.

**Do UI and 3D need different models? No.** The UI is proof that ECS fits
non-game state; the 3D examples only avoid ECS because the ECS was filed under UI
and lacked generational entities + queries. One general ECS serves both.

## 4. Smallest set of improvements to make this practical

Ordered by leverage; each is a library/ECS change, **no language change**:

1. **Extract `lib/ecs/`** from `lib/ui/ecs.rae` — `EntityId`, `ComponentTable(T)`,
   `World` helpers, `createEntity`/`destroyEntity`. `UiWorld` becomes a thin
   `World` user. (Mechanical; unblocks everything.)
2. **Generational `EntityId`** + free-list recycling + O(1) `isAlive`
   (`ecs.rae:7`, `:615`, `:624`).
3. **Multi-component query** (`query2`/`query3`, smallest-table probe) in
   `lib/ecs` — removes the `componentHas` ladders and the parallel-array joins.
4. **`HierarchySystem`** owning `Children`(authoritative, ordered)/`Parent`(derived) with
   referential-integrity on destroy — fixes the desync + dangling-ref gap
   (`ecs.rae:798-804`).
5. **`TransformSystem`** producing derived world transforms over a depth-sorted
   order — replaces recursive `Children` walks and 114's manual matrix rebuilds.
6. **Component registry + serialize-out**: a registration table (`save`/`load`/
   flags) driving both save and load, using the compiler's per-struct
   `toJson`/`fromJson`; register derived/cache components with no `SERIALIZE`
   flag. Requires a **JSON builder in `lib/json.rae`** (currently parser-only).
7. **Cut component boilerplate** (§2.10): compiler-synthesized per-World
   `destroyEntity`/registry helpers so adding a component is one field. Do this
   BEFORE the large migrations, or they hard-code the four-place pattern.
8. **Fix `mod`-component ergonomics for value structs** so games stop doing
   copy-mutate-set (`app.rae:590-599`) — this leans on the `opt T`/`view`/`mod`
   work already done this epic (#654/#656/#662/#663); verify component `mod`
   access aliases storage for gameplay structs.
9. **`Schedule`** (§2.5): ordered system entries + generalized generation-based
   dirty-skip, replacing the per-system `*IfDirty` wrappers.
10. **Tags**: make empty structs legal (one-byte C lowering) and add the tag
    table pattern + query-by-tag.
11. **Resources + events** (§2.8, §2.9): name the resource pattern; `EventQueue(T)`
    on the world.
12. **A non-graphical ECS example** (headless simulation/CLI) in the test suite,
    proving ECS as general program structure, not a graphics idiom.

## 5. Non-goals (do not add to the language)

- No `persistent`/`runtime`/`serialized`/`replicated`/`editorOnly` type
  properties or annotations. All are registry flags.
- No reflection / first-class type ids. The compile-time `World` struct is the
  registry.
- No archetype engine yet. Revisit only if a real query profiles poorly.
- No dependency-SOLVING scheduler. The `Schedule` is an explicit ordered list;
  it does not infer order from read/write sets.
- No new syntax for any of this. Compiler-**synthesized helpers** (like the
  existing `toJson`/`fromJson`/deep-copy/drop) are in bounds; keywords and
  annotations are not.
