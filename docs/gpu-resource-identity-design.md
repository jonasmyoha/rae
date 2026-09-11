# GPU resource identity: manager IDs and the public resource API (#892)

Status: **approved contract (maintainer approval 2026-09-11: all four
recommendations taken as written); implementation is #869.** Executable model:
`compiler/tests/cases/794_gpu_identity_model` (deterministic, no GPU).

This is library design over the shipped lifecycle (`create`/`drop`/`copy`,
[constructors and destructors](constructors-and-destructors.md)) and the approved
pointer boundary ([ptr-and-gpu-resource-design.md](ptr-and-gpu-resource-design.md)).
It adds no language feature and no ownership framework: an ID is a plain
copyable value, and `GpuResources` is an ordinary owner.

## Inherited constraints

- `GpuResources` has `create` and `drop` and no `copy`; it owns native resources.
- `TextureId`, `BufferId`, `TextureViewId`, `SamplerId` (and later `PipelineId`,
  `BindGroupId`, `SubmissionId`, `ReadbackId`) are copyable identities. Copying an
  ID never allocates, retains or releases; a named fallible operation
  (`duplicateTexture`) makes an independent allocation.
- No fixed 256-bit nonce, no hidden current-manager registry, no global renderer
  slot table, no parameter injected into `main`. `main` keeps its normal
  signature and creates what it owns.
- The `App -> RenderSystem -> GpuResources` ownership tree and the direct owned
  `if let` branch stay as shown in the pointer design.
- #897: a safe operation validates the manager's AUTHORITATIVE state; a field the
  caller can mutate (including every field of an ID) can never enlarge access.
- #896: a value that contains no `Ptr` is unrestricted — it may be copied,
  interpolated, serialized and stored anywhere without `unsafe`. An ID must
  therefore contain no pointer, so that ordinary systems can hold and log IDs.

## The four representations, compared with two independent managers

Every option keeps `slot` + `generation` (the `EntityId` idiom already in
`lib/ecs`): `slot` addresses the manager's table, `generation` distinguishes a
live resource from a stale handle to a since-retired/reused slot. They differ in
how an ID names WHICH manager issued it.

| | 1. manager-local only | 2. explicitly shared issuer | 3. random namespace | 4. retained identity token |
|---|---|---|---|---|
| ID contents | slot, generation | managerTag, slot, generation | random tag, slot, generation | slot, generation + retained manager token/pointer |
| wrong-owner rejection across two Apps | **no** — both Apps issue slot 0 / gen 0, so A's ID validates in B | **yes, guaranteed** within one issuer | yes, probabilistic | yes (if the token is checked) |
| survives manager destruction/recreation | **no** — a new manager's fresh slot 0 / gen 0 accepts the old ID | **yes** — a dropped manager's tag is never reissued | yes, probabilistic | token dangles or must be refcounted |
| deterministic / testable | yes | **yes** | no (PRNG, seeding) | yes |
| coordination required | none | the root that creates the Apps owns one issuer and passes it explicitly | none | none |
| collision assumption | n/a (no owner check at all) | none within an issuer; **no guarantee across independent issuers** | birthday bound on the tag width; depends on PRNG quality and process-global seed state | none |
| ID copy cost | 16 B memcpy | 24 B memcpy | 24 B memcpy | refcount or pointer copy; not POD |
| language-contract fit | fine | fine | fine, but relies on hidden global PRNG state | **rejected**: a retained pointer makes the ID a `Ptr`-bearing value (#896 rules, `unsafe` to copy/log), and a refcounted token is a second ownership framework |

Option 1 fails the two required guarantees (wrong-owner and recreation). Option 4
is rejected on the language contract. Option 3 works but trades a deterministic
guarantee for a probabilistic one and a hidden global (the PRNG), which the
pointer design explicitly avoids. **Option 2 is chosen.**

## Chosen contract

### Representation

```rae
# Owned by the root that creates managers (main, or the App root). Hands out
# each manager's tag; never reissues one. Passed explicitly, never looked up.
type IdIssuer {
  nextTag: Int
}
func create() ret IdIssuer            # nextTag starts at 1; tag 0 is "no manager"
func issue(this: mod IdIssuer) ret Int

# One layout for every kind; the KIND is the Rae type. A BufferId cannot be
# passed where a TextureId is expected, so kind confusion is a compile error and
# no runtime kind field is needed in the ID.
type TextureId     { managerTag: Int  slot: Int  generation: Int }
type BufferId      { managerTag: Int  slot: Int  generation: Int }
type TextureViewId { managerTag: Int  slot: Int  generation: Int }
type SamplerId     { managerTag: Int  slot: Int  generation: Int }
```

An ID is three `Int`s: no pointer, no destructor, value equality with `is`. Its
fields are readable for logging and diagnostics and are treated as HINTS by every
manager operation; the slot record is authoritative.

### Manager

```rae
type GpuResources {
  managerTag: Int
  # per kind: capacity, generations: List(Int), live: List(Int), plus the native
  # handle storage kept per context (#869)
}
func create(issuer: mod IdIssuer, capacity: view Int) ret opt GpuResources
func drop(this: mod GpuResources)
# no copy: the manager is uncopyable by the shipped rule (destructor, no copy)
```

`create` returns `none` when the device/context is unavailable or the slot tables
cannot be allocated; the caller's direct owned `if let` branch handles it and
nothing leaks (the model's `capacity: 0` stands in for this).

### Validation (every safe operation, in this order)

1. `id.managerTag is resources.managerTag` — otherwise **wrong owner**;
2. kind — enforced by the ID's Rae type;
3. `0 <= id.slot < capacity` — otherwise out of range;
4. the slot is live — otherwise retired/never created;
5. `generations[slot] is id.generation` — otherwise **stale**;
6. operation-specific: usage flags permit the operation, and any byte offset/size
   is overflow-safe and within the slot's authoritative native size (#897) —
   never a size carried by the caller.

A rejected ID yields `false` / `none`; it never reaches native state.

### Creation, retirement, errors

- `createTexture(resources: mod GpuResources, descriptor: view TextureDescriptor) ret opt TextureId`
  returns `none` on **exhaustion** (no free slot), native allocation failure or an
  invalid descriptor. The reused slot carries the generation bumped by its last
  retirement, so it is a NEW identity.
- `retireTexture(resources: mod GpuResources, id: view TextureId) ret Bool`
  validates, bumps the slot's generation (every copy of the ID becomes stale at
  once), frees the slot and returns `true`. An invalid, stale, or foreign ID
  returns `false`; a double retire is therefore harmless, never a crash.
- Exhaustion is deterministic at the capacity chosen in `create`; nothing wraps
  or overwrites. Growth, if ever wanted, is an explicit later operation.
- Recording/submission lifetime (#870): `retire` rejects NEW uses immediately;
  the native release of a resource with in-flight recorded uses is deferred by
  #870's tracking, exactly as the older notes require ("retired IDs reject new
  uses; existing uses keep native resources alive").

### Resource groups

A `ResourceGroup` is a manager-owned identity list (`List(TextureId)` etc.)
created by and retired through its manager; it does not own native resources
independently. Copying a group's ID list copies identities only — a copied
controller of IDs does not acquire a destructor that releases another copy's
resources (the water requirement in the older notes). Retiring a group retires
its members with the same generation bump.

### Costs and guarantees

- ID: 24 bytes, memcpy copy, O(1) validation, no allocation, no `unsafe`.
- Manager: per kind `capacity × 2 Int` bookkeeping plus native handle storage.
- Within one issuer: two managers never share a tag (monotonic counter; 63-bit
  space, unreachable); a dropped manager's tag is never reissued, so recreation
  never resurrects an ID; per-slot generation wrap needs 2^63 retirements of one
  slot, unreachable. **No probabilistic component.**
- Across INDEPENDENT issuers there is no guarantee: two roots that each create
  their own issuer can both issue tag 1. The contract is therefore: one issuer
  per root that creates managers, passed explicitly. This is the tradeoff to
  approve (below).

## Two-App demonstration

`compiler/tests/cases/794_gpu_identity_model` models the contract with plain
Rae (a slot table, no GPU) and logs:

- managers 1 and 2, built from one issuer, each issue `slot 0 / gen 0`, and
  `slots and generations match: true` — yet each ID is `false` in the other
  manager (wrong owner rejected both ways);
- a copied ID `is` the original; after `retire`, the original AND the copy are
  stale, a double retire is `false`, the slot is reused as `gen 1` and the old
  ID stays stale;
- with capacity 2 the third create is `none (exhausted)`; freeing a slot lets a
  create succeed again;
- manager 3 is dropped, its escaped ID (`tag 3 slot 0 gen 0`) is rejected by the
  recreated manager 4 although slot and generation match, while manager 4's own
  ID is valid;
- a failed construction takes the `none` branch; the issuer has issued 4 tags.

## Canonical example (kept from the pointer design, with the explicit issuer)

```rae
open gpu/GpuResources

type RenderSystem { resources: GpuResources  waterTexture: TextureId }

func create(issuer: mod IdIssuer, texturePath: view String) ret opt RenderSystem {
  if let resources: GpuResources = GpuResources.create(issuer: issuer, capacity: 256) {
    if let texture: TextureId = loadTexture(resources: resources, path: texturePath) {
      ret RenderSystem { resources: own resources, waterTexture: texture }
    }
    # resources drops here if texture creation failed.
  }
  ret none
}

type App { renderSystem: RenderSystem }

func create(issuer: mod IdIssuer) ret opt App {
  if let renderSystem: RenderSystem = RenderSystem.create(issuer: issuer, texturePath: "water.png") {
    ret App { renderSystem: own renderSystem }
  }
  ret none
}

func main() {
  var issuer: IdIssuer = IdIssuer.create()       # main owns it; nothing injected
  if let app: App = App.create(issuer: issuer) {
    loop not shouldClose(resources: app.renderSystem.resources) {
      frame(app: app)
    }
  } else {
    log("App creation failed")
  }
}
```

The only change to the shipped example is the explicit `issuer` thread; a second
App in the same `main` takes the same issuer and is distinguishable by
construction.

## Tradeoffs — APPROVED 2026-09-11 (all recommendations as written)

The maintainer approved every recommended option below on 2026-09-11; #869
implements exactly these. Only a conflict surfaced by #870/#871 reopens them.


1. **Issuer sharing scope** (the identity guarantee). Recommended: one
   `IdIssuer` owned by the root that creates managers and passed explicitly;
   deterministic, zero collision. Alternatives: (a) each App owns its own issuer
   — simpler call sites but NO cross-App wrong-owner guarantee; (b) a random tag
   at `create` — no coordination, but probabilistic and dependent on the global
   PRNG. Please approve the recommended scheme or choose (a)/(b).
2. **Capacity policy.** Recommended: a fixed per-manager capacity chosen at
   `create` (deterministic `none` on exhaustion). Alternative: a growable table
   (exhaustion only at memory limits).
3. **ID layout.** Recommended: three explicit `Int` fields (24 B, readable,
   serializable). Alternative: two packed `Int`s (16 B) — only if measured.
4. **Kind safety.** Recommended: distinct Rae types per kind over one layout
   (compile-time kind safety, no runtime kind field). Alternative: one
   `ResourceId` with a runtime `kind`.

## Relationship to older notes

This settles the "GPU manager identity remains under review" item of
[webgpu-resource-management-in-rae.md](webgpu-resource-management-in-rae.md) and
the deferred identity representation in the pointer design. Async native holds,
retirement-with-in-flight-uses and manager-owned groups keep their meaning there
and are implemented by #870/#871 over this ID layout.
