# Require `copy` for cascade-drop value bindings (design note, #646)

Status: **design note + recommendation** (investigate-first). Implementation is
tracked as **#659**. No behavior has changed yet.

## What happens today (verified by emitted C)

Binding a value of a struct that owns heap (`type_needs_cascade_drop` is true —
it has a `List`/`Map`/`String`/nested-owning field) is an **implicit deep copy**:

```rae
type Owner { items: List(Int) }
var owner: Owner = { items: createList(Int, cap: 2) }
owner.items.add(value: 5)
let copyOfOwner: Owner = owner        # <-- implicit deep copy
copyOfOwner.items.add(value: 6)
# copyOfOwner.items.length == 2, owner.items.length == 1  (independent lists)
```

Emitted C:
```c
rae_Owner copyOfOwner = (__extension__ ({ rae_Owner __dc0;
    rae_deep_copy_rae_Owner(&__dc0, &(owner)); __dc0; }));
...
rae_drop_struct_rae_Owner(&copyOfOwner);
rae_drop_struct_rae_Owner(&owner);
```

Findings:
- **It is a deep copy, not a shallow copy** — each binding gets its own heap;
  both are dropped independently; **no double free, no leak** (`RAE_MEM_STATS`
  reports `outstanding=0`). There is no correctness bug here.
- **The deep copy is unconditional** — it happens even when the source is dead
  after the binding (last use). There is **no move optimization** for local
  value bindings.
- **`copy T` and `own T` local bindings emit the SAME deep copy** as bare `T`.
  `is_copy` / `is_own` are annotation-only for *local bindings* — they only
  affect *parameter passing* codegen (c_call.c / c_backend.c). `ast.h:38`:
  "Stage A is annotation-only — codegen treats it identically to bare T."
- **Non-cascade-drop types are a plain value copy.** `let b: Point = a` (all
  scalars) emits `rae_Point b = a;` — no deep copy, no allocation. `Vec2`/`Vec3`/
  a heap-free `Particle` are in this class.

## Correction to the task premise

The report framed this as "`copy` is annotation-only, so give it real deep-copy
codegen." But the deep-copy codegen **already exists** (`rae_deep_copy_<T>`,
generated for every cascade-drop type; also used by #641's `opt` deep copy) and
**bare `T` already uses it**. There is no missing codegen and no shallow-copy
double-free to fix. The real gap is a **semantic requirement**: nothing forces
the author to acknowledge the hidden deep-copy allocation.

## The proposal

For a **cascade-drop** type, make a bare value binding a sema error and require
the author to spell the intent — mirroring the copy-vs-alias distinction the rest
of this epic established (`copyAt`/`viewAt`/`modAt` in #643, `=` value vs `=>`
alias in #644):

- `let x: copy Owner = owner` — explicit deep copy (visible allocation). Emits
  the existing `rae_deep_copy_<T>` (already correct).
- `let x: view Owner => owner` / `mod Owner => owner` — alias, no copy (already
  works; the `=>` reference forms).
- `let x: Owner = owner` (bare value binding of a cascade-drop type) — **error**:
  "`Owner` owns heap; a value copy is a deep copy — write `copy Owner` to copy,
  or `view`/`mod` to alias."

Non-cascade-drop types (`Int`, `Vec2`, `Particle`) keep the implicit plain copy —
requiring `copy` there would be noise, and there is no allocation to spell out.

## Honest caveat (material to the decision)

Because there is **no move-of-locals**, requiring `copy` does **not** save any
allocation — `copy Owner` deep-copies exactly as today's implicit binding does.
The payoff of #646 is **explicitness / auditability of heap allocation**
(and consistency with the `copy` parameter mode and the copy-vs-alias direction),
**not** correctness (already safe) or performance (identical). The cost is a
migration: every existing `let x: HeapOwningStruct = y` must gain `copy` (or move
to a `view`/`mod` alias). Two adjacent findings worth folding into the decision:

1. The unconditional deep copy on a dead source is a real (small) inefficiency; a
   future move-on-last-use for the *bare* (non-`copy`) case would give `copy` a
   genuine cost distinction. That is a larger, separate feature (needs use-after-
   move analysis) and is out of scope here.
2. `own T` as a *local binding* currently deep-copies and drops both sides rather
   than moving — a separate wart (own is really a parameter mode). Not #646.

## Recommendation

Adopt the semantic requirement (bare cascade-drop value binding → error; require
`copy`, or a `view`/`mod` alias), because it is consistent with the copy-vs-alias
direction the epic committed to and makes hidden heap allocation auditable — while
recording clearly that the benefit is explicitness, not perf/correctness. If the
migration churn is judged not worth pure explicitness, the fallback is to keep the
status quo (implicit deep copy is safe) and instead pursue the move-on-last-use
optimization (finding #1) as the higher-value follow-up. Implementation and this
decision point are filed as **#659**.
