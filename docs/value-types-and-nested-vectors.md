# Value types and nested vectors in Rae (Vec2/Vec3/Vec4)

An investigation into how Rae represents small value types such as `Vec2`, `Vec3`,
`Vec4` when used as fields inside larger structs (ECS components, a `Particle`,
etc.). Method: read the compiler / runtime / type system, emit and inspect the
generated C, compile + run, and probe struct sizes and layout. Evidence is
generated-C, not language-design assumption.

## TL;DR — the headline question

> Does a `Vec3` inside a larger Rae struct behave like three inline floats all the
> way down to the generated C, or is Rae introducing heap allocation/indirection?

**It is three inline floats, all the way down.** A `Vec3`/`Vec4` field is embedded
directly in the containing C struct, byte-identical to writing the floats flat,
with zero padding, zero indirection, and zero per-element heap allocation. Using
`Vec3`/`Vec4` in an ECS component such as a particle is **free** versus separate
floats.

The only heap hazard in this area has **nothing to do with the vectors**: it is the
`opt T` return of `List.get/at`, and the idiomatic `if let` access pattern already
avoids it (details below).

## 1. Rae's allocation / value model (verified)

- A Rae `type` is a **value type**: it lowers to a plain C `struct rae_Foo { … }`,
  passed, returned and stored **by value**. No header, no refcount, no vtable.
- **Inline vs heap.** Value all the way: primitives (`Int`/`Float`/`Bool`/`Char`),
  `type` structs, fixed `Array(T, cap: N)`. Own a heap block: `List(T)`,
  `StringMap`, `IntMap`, `Buffer(T)` (a `{ T* data; i64 length; i64 cap; }` header
  over one contiguous allocation), and `String`. Boxing: `Any` and `opt T` for a
  non-pointer `T` (see §5).
- **Nested structs embed inline**, never by pointer.
- **`Vec2`/`Vec3`/`Vec4` are ordinary library structs** (`lib/vec2.rae` … define
  `type Vec3 { x: Float y: Float z: Float }`). No compiler special-casing — the C
  backend explicitly classifies `Vec3` as a "plain value struct"
  (`c_backend.c`). They own no heap, so `type_needs_cascade_drop` is false → **no
  drop/retain/release** is generated for them.

## 2. What the generated C actually does

For
```rae
type Particle { position: Vec3  velocity: Vec3  life: Float  maxLife: Float  size: Float  color: Vec3 }
type Particle4 { position: Vec3  velocity: Vec3  color: Vec4  life: Float }
```
the emitted C is:
```c
struct rae_Vec3 { float x; float y; float z; };
struct rae_Vec4 { float x; float y; float z; float w; };
struct rae_Particle {
  rae_Vec3 position;      // embedded inline, NOT a pointer
  rae_Vec3 velocity;
  float life; float maxLife; float size;
  rae_Vec3 color;
};
struct rae_Particle4 { rae_Vec3 position; rae_Vec3 velocity; rae_Vec4 color; float life; };
struct rae_List_rae_Particle { rae_Particle* data; int64_t length; int64_t cap; };
```
`List(Particle)` is **one contiguous array of `rae_Particle` values** — not an array
of pointers, and with no per-`Vec3`/`Vec4` allocation.

## 3. Concrete sizes / layout / allocation (measured)

| type | size | note |
|---|---|---|
| `Vec3` | 12 B | 3× float, no padding |
| `Vec4` | 16 B | 4× float |
| `Particle` (3×Vec3 + 3 float) | **48 B** | `velocity`@12, `color`@36 |
| `ParticleFlat` (12 flat floats) | **48 B** | **byte-identical** to `Particle` |
| `Particle4` | 44 B | 12+12+16+4 |

Measured behaviour of a 1000-element `List(Particle)`:

- **Construction** emits a pure C compound literal
  `(rae_Particle){ .position = rae_vec3_create(1,2,3), … }`; `vec3.create` returns a
  `Vec3` **by value**. No heap.
- **`add(value: p)`** copies `p` into `data[length]`. The only allocation in the
  whole program is `createList`/`grow`'s single contiguous backing block (the
  expected one).
- **Read via `if let particle: Particle = particles.copyAt(index: j)`** is peephole-
  lowered to a direct, bounds-checked array index:
  ```c
  if ((uint64_t)i < (uint64_t)list->length) { rae_Particle particle = list->data[i]; … }
  ```
  a stack copy from contiguous storage — **no malloc, no boxing**.
  `particle.position.x` is a direct nested field read, no indirection. (This copies
  the *whole* element; §5 covers the zero-copy `view`/`mod` alternatives.)
- **Write via `set(index:, value:)`** is a direct store
  `*(rae_Particle*)((char*)data + i*sizeof) = value;` — no heap.
- **No hidden retain/release/free** per element: the element type owns no heap, so
  only the list's own `drop` (freeing the backing block) is emitted.

## 4. Is `Vec3`/`Vec4` in an ECS component free vs flat floats?

**Yes, entirely.** Same 48-byte layout, same packing, same access codegen, same
(zero) allocation. Replacing `x,y,z / vx,vy,vz / r,g,b` with
`position/velocity/color: Vec3` changes nothing in the generated C except field
names — and it unlocks `vec3.*` math on the fields. Recommended.

## 5. Access modes — value copy vs `view`/`mod` (this is the part that matters)

`List` has three read accessors — `copyAt` / `viewAt` / `modAt` — and the choice of
binding (`=` value, `=> view`, `=> mod`) decides whether you copy the element or
alias it in place. (Historical note: this report predates #643, which renamed the
value accessor `at` → `copyAt` and removed the `get`/`viewGet`/`modGet` aliases;
snippets below say `copyAt`.) `copyAt` returns `opt T` **by value**.

| pattern | returns | generated C | cost |
|---|---|---|---|
| `if let particle: Particle = particles.copyAt(index:)` | `opt T` | `rae_Particle particle = data[i];` | **full-struct copy** |
| `if let particle: view Particle => particles.viewAt(index:)` | `opt view T` | `rae_Particle* particle = &data[i];` … `particle->…` | **pointer, zero copy** |
| `if let particle: mod Particle => particles.modAt(index:)` | `opt mod T` | `rae_Particle* particle = &data[i]; particle->position.x = …;` | **pointer, in-place write** |

All three are peephole-lowered to a direct bounds-checked index into the contiguous
`data` block — **none allocate**. The difference is copy-vs-reference:

- **value (`= at`)** copies the *whole* element onto the stack. Cheap for a 48-byte
  particle; not free for a large component, and you cannot write back without a
  `set(index:, value:)` round trip.
- **`view` (`=> viewAt`)** binds a `rae_Particle*` into the list's own storage — no
  copy, reads are `particle->field`. Best for read-only access to large elements.
- **`mod` (`=> modAt`)** binds a mutable pointer; writing `particle.position.x = …`
  stores **in place** — no read-modify-`set` round trip and no drop/re-add of the
  element's owned fields.

The `=>` peephole also fires when a `view`/`mod` alias is bound to `at`/`get`
directly (`if let particle: view Particle => particles.copyAt(index:)`), aliasing live
storage the same way — but `viewAt`/`modAt` are the accessors that actually *return*
`opt view T` / `opt mod T`, so they are the clearer spelling. Verified at runtime: a
`modAt` write persists and is seen by a subsequent value/`view` read.

**The boxing hazard, and how it is avoided.** The *out-of-line* generic `get`/`at`
realises `opt T` for a value struct by **heap-boxing**
(`malloc(sizeof(T))` + copy + `rae_any_owned_ptr`). The `if let` peephole above
bypasses that entirely (direct index; no box, no malloc); scalar
`let n: Int = list.copyAt(index:)` auto-unwraps trivially.

**Compiler rough edge found:** `let particle: Particle = particles.copyAt(index: j)` —
assigning `opt Struct` **directly** to a non-optional struct binding — is *not*
peephole-optimised and currently emits **non-compiling C**
(`rae_Particle particle = <RaeAny>`; a type mismatch). The scalar form compiles; the
struct form does not. This should be a clean sema error ("unwrap the optional — use
`if let`") or a valid unwrap. See §8.

All of this is **orthogonal to `Vec3`**: value-vs-reference access is identical for
`ParticleFlat`. It is a property of the list-read pattern, not of nested vectors.

## 6. Guidance for particle/ECS code

- Adopt `Vec3`/`Vec4` for `position` / `velocity` / `scale` / `color` freely — free
  in generated C, clearer, and enables `vec3.*` ops.
- **Pick the access mode by intent, not habit:** `view`/`modAt` for read-only or
  in-place mutation of a struct element (zero copy, no `set` round trip); the value
  `= at` form only when you genuinely want a detached copy. Defaulting everything to
  the value copy leaves the whole element being copied per access and forces a
  read-modify-`set` round trip on every mutation — measurable for large components in
  hot per-frame loops.
- If a system needs absolute max throughput, `Array(T, cap: N)` supports direct
  inline `arr[i]` (no `opt T`, no bounds branch beyond the peephole). `List` requires
  `at`/`viewAt`/`modAt`; direct `list[i]` is rejected by sema.

## 7. Semantic aliases for vector types

**Rae has no type alias today.** `alias` is not a keyword (spec keywords are
`type func let var ret spawn`); the parser's "alias" refers only to import `as` and
`view`/`mod` binding aliases.

Rae is **nominally typed**: a distinct `type Size { x: Float  y: Float }` is *not*
interchangeable with `Vec2` — verified, the compiler rejects it with
*"cannot convert Size to Vec2; they are different types."* So today you cannot give
a vector a domain name (`Size`) and still use `vec2.*` on it without either an alias
or re-declaring the ops.

**A true alias would be modest to implement and is a good direction.**
`alias Size = Vec2` should resolve `Size` to **Vec2's `TypeInfo`** in
`sema_resolve_type_internal` (before the nominal decl lookup). Then `Size` *is*
`Vec2`: same `TypeInfo`, same mangled C type (`rae_Vec2`), fully interchangeable,
**no implicit casts/conversions needed** (there is nothing to convert — it is the
same type), and no ambiguity. Layout and inlining are unchanged.

With that, `type Rect { pos: Vec2  size: Size }` is fully inline — 4 floats, 16 B,
identical to `{ x, y, w, h }` — with semantic names. (Verified nested-vector structs
stay inline.)

**Design guidance:** use `Vec2/3/4` for mathematical/vector data; use an alias
(`Size = Vec2`) when a vector wants a domain name; keep genuinely different concepts
(`Insets { l, t, r, b }`) as their own named flat structs. `Rect`/`Insets` as flat
named structs is fine today; `Rect { pos, size }` becomes attractive once aliases
exist.

## 8. Compiler follow-ups this surfaced

1. **Bug — RESOLVED (#642).** `let x: Struct = list.get/at(index:)` (opt-struct →
   non-opt-struct direct assign) used to emit non-compiling C, while the scalar
   form silently unwrapped (none → 0, no diagnostic). Assigning `opt T` to a
   non-optional binding, argument, or return is now a **uniform hard sema error
   for every `T`** — scalar accessors, struct accessors, and user functions
   returning `opt` — with the message *"optional not unwrapped — use `if let`"*.
   Every optional is consumed with `if let` (no `!`/default operator was added);
   the silent-unwrap path is gone.
2. **Enhancement:** add a real `alias` (type-alias) mechanism if the semantic-name
   direction is adopted — a small, well-contained change in type resolution.

Neither blocks using `Vec3`/`Vec4` as nested value-type fields today.
