# Language readiness for the target renderer

Status: **proposal**. Nothing here is implemented. This document audits what
Rae lacks for the renderer we actually want, and proposes the minimum
language/runtime work to make that renderer expressible *naturally* rather
than through workarounds. The work is grouped into dependency **tracks**,
not sequential phases — see §7 for why that distinction matters.

Companion documents: `render-graph-and-gi-plan.md` (renderer architecture),
`unified-3d-renderer.md` (scene model), `primitive-types.md` (numeric model).

---

## 1. Why this document exists

The renderer is being redirected from "evolve the forward prototype" to
"build the architecture we want": render graph, deferred G-buffer lighting,
depth pyramid, cascaded shadows, TAA, screen-space techniques, world-space
GI, and both animated triangle meshes and SDF geometry.

The forward prototype is small — roughly 1,500 lines of runtime C across
five passes — so replacing it is cheap. What is *not* cheap is discovering,
halfway through, that the language forces a workaround into a load-bearing
position. That has already happened repeatedly at small scale:

- `lib/ui/frame_stats.rae` is a ring buffer because `List` has no
  `removeLast`; the natural implementation was impossible.
- `lib/renderer3d.rae` dispatches passes through a tag switch because Rae
  has no function references.
- Several loops in `examples/110_gpu3d_ui` are restructured around the
  absence of `continue`.
- `lib/ui/log_overlay.rae` carries a comment explaining an ownership
  workaround for `List.set` taking `own T`.

Each was individually trivial. Collectively they are the shape of a
language being bent around a problem it cannot state directly. At renderer
scale — per-joint, per-object, per-pixel — the same gaps stop being
cosmetic and start being performance and correctness problems.

---

## 2. Audit: what exists today

Verified against the compiler and stdlib, not assumed.

### Present and adequate

| Capability | Where | Notes |
|---|---|---|
| Bitwise ops | `bitand`/`bitor`/`bitxor`/`bitnot`/`shl`/`shr` keywords | Enough for pass masks and packed fields |
| Explicit numeric conversion | `as` | No implicit conversions; errors are loud |
| `Float` = f32, `Float64` distinct | `type.h` | Matches GPU expectations; FFI already flipped |
| Struct value types | `type` declarations | `Vec3` is a real value type, not boxed |
| Contiguous element storage | `List(T)`'s `data` buffer | Element arrays are already cache-friendly |
| Real OS threads (compiled) | `spawn` → pthread thunks | Live VM runs `spawn` inline, but Live is frozen |
| Generic containers | `List(T)`, `StringMap(V)` | Specialised, not boxed |
| GPU compute seam | `lib/gpu.rae` | Arbitrary WGSL + storage buffers |

### Missing or inadequate

| Gap | Evidence | Renderer impact |
|---|---|---|
| **Fixed-size value aggregates** | Only `List(T)` (heap) and `Buffer(T)` (raw). No `[N]T`. | **Critical — see §3** |
| `continue` / `break` | Absent from the lexer keyword table | Inner loops over joints, samples, cascades |
| `Vec2` / `Vec4` / `Quat` / `Mat4` / `Mat3x4` types | Only `Vec3` exists | Animation, transforms, packing |
| SIMD | No `__m128`/NEON anywhere in the runtime | Skinning throughput |
| `f16` | Only `Float`(f32) and `Float64` | GPU-facing packed data |
| `popcount` | Absent | Visibility-bitmask AO/GI on the CPU side |
| `List` removal ops | Only `clear`; no `removeLast`/`truncate` | Forced ring buffers |
| Function references | Absent (documented in `renderer3d.rae`) | Pass dispatch, callbacks |
| SoA support | Manual parallel `List`s only | Skeletons, particles, instances |

### Known ownership sharp edges

These are not "missing features" but language-level hazards. They are
serious enough that §4 treats them as the second foundation, co-equal with
value types (see also Track A.2):

- `List(T).add`/`set` take `value: own T`; passing a `view String` makes the
  container alias caller memory and double-free. Hit in `log_overlay`.
- `List(Struct)` does not reliably deep-copy inner heap fields.
- Chained generic-method calls mis-resolved silently until #349.

---

## 3. The single most important finding: matrices allocate

`lib/math3d.rae` represents every 4×4 matrix as `List(Float)` — a
heap-allocated, reference-counted, growable container holding 16 floats.
Every matrix operation allocates a new one:

```rae
func mat4Mul(a: view List(Float), b: view List(Float)) pub ret List(Float) {
  let out: List(Float) = createList(Float, cap: 16)   # heap allocation
  ...
}
```

`gpu3d.modelMatrix()` builds a transform from five matrices and three
multiplies. That is **eight heap allocations and eight frees per object per
frame**, before any drawing happens. At 1,000 objects and 60 Hz that is
roughly half a million allocation pairs per second, purely to compute
transforms that should live in registers.

This is not a stdlib tuning issue. It is a *language* gap: Rae currently has
no way to express "16 floats, by value, no indirection". Until it does:

- `Mat4`/`Mat3x4` cannot be efficient
- `Quat`/`Vec4` cannot be SIMD-lowered (no guaranteed contiguous 16-byte value)
- Skinning matrix palettes cannot be stack- or arena-allocated
- Vertex/instance data cannot be laid out without going through `Buffer`

**Everything else in this document is downstream of fixing this.** It is the
one item that, left alone, guarantees the new renderer is slow no matter how
good its architecture is.

---

## 4. The second foundation: ownership at container boundaries

The ownership hazards in §2 share a single root: **ownership transfer at
container boundaries is assumed, not checked.** `List(T).add` and `set`
declare `value: own T` and simply trust the caller; passing a borrowed value
compiles cleanly and produces a container aliasing memory it does not own.

At the current scale that has produced isolated bugs, each found by a crash
and fixed in an afternoon. At renderer scale it becomes a different problem,
because the target architecture moves data through containers constantly and
across a thread boundary:

- transform and skinning palettes into per-frame arrays
- draw commands into a command buffer
- **scene snapshots handed to a render worker**, then read while the
  simulation thread continues mutating its own copy
- GI probe and history data surviving across frames

A silent aliasing bug in that machinery does not present as a clean crash at
teardown. It presents as intermittent corruption, one frame in a thousand,
on one thread, in data that has already been copied to the GPU. That is
among the most expensive classes of bug to diagnose, and the thread boundary
makes it worse: the double-free that `log_overlay` hit within one thread
would, across two, become a data race.

**This must be designed and tested before large amounts of renderer
infrastructure are built on top of it**, not retrofitted afterwards. The
value types of §3 make it more urgent, not less: once `Mat4` and `Quat` are
value aggregates flowing through lists and snapshots by the thousand, the
cost of getting transfer semantics wrong scales with the renderer.

What "designed" should mean here, concretely:

- A decision on whether passing a non-owned value to an `own` parameter is a
  **compile error** rather than undefined behaviour. This is the single
  highest-value fix and it is a checkable property.
- Defined, tested semantics for `List(T)` where `T` is a struct containing
  heap fields — currently unreliable.
- A `Snapshot`/hand-off concept for cross-thread transfer (see Track E),
  designed with these rules already settled rather than inventing them under
  deadline.
- Tests that specifically exercise value aggregates through containers,
  since that is the combination the renderer will lean on hardest.

---

## 5. What is *not* a language feature

Scope discipline matters as much as the feature list. Three items commonly
grouped here do not need language work:

**`f16` in shaders is a device feature, not a Rae type.** The reference
engine's `enable f16` in WGSL requires the WebGPU `shader-f16` feature to be
requested at device creation. That is a change in `runtime_webgpu.c` — zero
language work — and it delivers most of the win (halved ALU and register
pressure in AO, blur, bloom, lighting). A Rae-side `Float16` is only needed
to *author packed CPU data*, and even then `Int`-based bit packing suffices
initially. **Request the device feature early; defer the Rae type.**

**Depth pyramid, pass masks, cascade layout** are renderer design, not
language capability. They are listed here only to be explicitly excluded.

**Function references may never be needed.** The tag-switch dispatch in
`renderer3d.rae` is not purely a workaround: it makes the full set of passes
statically enumerable, avoids indirect calls in the frame loop, and keeps
the render graph's data (`lib/rendergraph.rae`) free of any callable type.
A data-driven graph with user-registered passes would want real function
values — but that is a *maybe later*, not a prerequisite. **Defer until a
concrete consumer exists.**

---

## 6. Proposed work — tracks, not a waterfall

The work below is grouped by dependency, **not by phase**. Only two things
genuinely gate renderer construction (Track A). Everything else should be
pulled in *on demand by the renderer itself* rather than completed
speculatively in advance.

Read the tracks as: A must land first; B starts the moment A's design is
settled and then drives C, D and E; C is free to happen at any time.

### Track A — the two foundations (gate everything)

**A.1 Fixed-size value aggregates.** The core addition. Some form of `[N]T`
— a value type, no heap, copied by value, embeddable in structs and in
`List(T)` elements. Design questions to settle deliberately:

- Spelling and whether `N` is a const-expression
- Interaction with ownership modes (`view`/`mod`/`own`/`copy`) — a value
  array should be trivially copyable and never need cascade-drop when `T`
  is primitive
- Indexing bounds behaviour (checked in debug, unchecked in release?)
- Whether it is a distinct kind or sugar over an internal `Buffer` with a
  compile-time length

**A.2 Ownership semantics at container boundaries** (§4). Settled and
tested, not deferred. A.1 and A.2 are co-designed: the rules for how a value
aggregate moves into a container are exactly where the two meet, and getting
them right is cheaper now than after the renderer depends on them.

**A.3 Value math types built on A.1.** `Vec2`, `Vec4`, `Quat`, `Mat3x4`,
`Mat4` as `type` declarations with value semantics and no allocation.
`Mat3x4` specifically because affine transforms need no fourth row — the
reference engine passes `mat3x4` across its render boundary for exactly this
reason, saving 25% of transform bandwidth.

### Track B — the renderer vertical slice (starts immediately after A, drives the rest)

As soon as A is designed, build a **thin but real slice of the target
architecture** — not a port of the forward prototype, and not a
microbenchmark:

> G-buffer pass → depth pyramid → one lighting pass → composite,
> with transforms and a skinning palette flowing through the new value
> types, declared in the render graph.

This is the validation criterion for Track A and the requirements source for
C, D and E. Porting `math3d.rae` off `List(Float)` happens here as a
consequence, not as the goal — **"`math3d` is faster" is not an acceptable
exit criterion for A.** The question A must answer is whether the *target
renderer's* data path is expressible and allocation-free.

Everything the slice cannot express naturally becomes a language
requirement, discovered against real code rather than predicted.

### Track C — cheap, unblocking, no design risk (do anytime)

Small, self-contained, and each removes an active workaround.

**C.1 `continue` and `break`.** Loop control with the obvious semantics.
Parser + codegen; `break` interacts with `defer` and scope-exit drops, which
is the only subtlety.

**C.2 `List` removal operations.** `removeLast()`, `truncate(len)`,
`removeAt(index)`. Must run element drop for heap element types — the same
path `set` already uses via `rae_ext_rae_buf_drop_at`.

**C.3 Bit helpers.** `popcount`, `leadingZeros`, `trailingZeros`. Needed for
visibility-bitmask work on the CPU side and generally useful for packed
data. Lowers to compiler intrinsics.

*Exit criterion: the three existing workarounds named in §1 are deleted.*

### Track D — performance, pulled in by Track B

Do these when the slice shows they matter, not before.

**D.1 SIMD lowering.** Not intrinsics in user code. The C backend should
lower `Vec4`/`Quat` operations to `__m128` / `float32x4_t` where the target
supports it, with a scalar fallback. The reference engine hand-wrote SIMD
for exactly one operation — quaternion multiply — because that is the hot
inner loop of skeletal animation. **Start there; measure before widening.**

**D.2 WebGPU `shader-f16` device feature.** Runtime change only (§5).
Enables half-precision in AO, blur, bloom, and lighting shaders. Cheap
enough to do early regardless.

**D.3 `Float16` as a storage type.** Only if profiling shows CPU-side
packing is hot. Conversion helpers, not arithmetic — f16 arithmetic on the
CPU is rarely worthwhile.

### Track E — structural, designed against Track B's real shape

**E.1 Threading hardening for a render worker.** The target architecture
puts the renderer on its own thread behind a command stream. `spawn` already
produces real OS threads in the compiled target, but a render worker needs
more than thread creation:

- A safe hand-off type: ownership transfer across threads that the compiler
  checks, so a scene packet cannot be mutated after being queued
- Some notion of "this data is immutable now" for snapshot/interpolation
- Bounded queues with backpressure

This is the largest and least-specified item. It should be designed against
the actual command-stream protocol, not in the abstract.

**E.2 SoA support — only if needed.** The reference engine's skeleton is
struct-of-arrays. Rae can express that today with parallel `List`s; it is
verbose but correct. **Recommendation: do not add SoA syntax yet.** Get
array-of-structs fast and allocation-free first (Track A), then measure. SoA
sugar designed before there is a consumer will almost certainly be wrong.

**E.3 Function references — only if a data-driven graph demands it** (§5).

---

## 7. Sequencing: why this is not a waterfall

The tracks are dependency-ordered, but only Track A gates anything. The
failure mode to avoid is treating A → C → D → E as phases and completing
each before starting the next.

**Once fixed-size values are designed, use them in the target renderer
immediately.** Track B is not a validation step bolted onto the end of A —
it runs concurrently and is the mechanism by which the remaining
requirements are discovered. SIMD (D.1), threading hand-off (E.1) and any
SoA need (E.2) should be specified by what the slice actually strains
against, not by this document's predictions.

**The methodological warning.** "Finish the language, then build the
renderer" is the right *direction* but a dangerous *literal plan*. Features
designed without a real consumer get their details wrong, and the details
are what cost time later. Recent evidence from this codebase: the missing
`AST_EXPR_INTERP` case in `infer_expr_type_ref` survived indefinitely
because nothing exercised chained generic calls until an app did; the
`List.set` ownership hazard only surfaced when a real ring buffer used it.
Both were interface-level mistakes invisible until something real used them.

**The exit criterion that matters.** Track A is done when the *target
renderer's* data path — G-buffer, depth pyramid, lighting, transforms and a
skinning palette through the render graph — is expressible and
allocation-free. It is explicitly **not** done when `Mat4` compiles, nor
when `math3d.rae` merely gets faster. Making the existing forward
prototype's maths faster would be optimising the thing we have already
decided to replace.

---

## 8. Summary

| Track | Item | Gates renderer? | Risk |
|---|---|---|---|
| **A** | **Fixed-size value aggregates** | **Yes — everything** | **Medium** |
| **A** | **Ownership semantics at container boundaries** | **Yes — correctness** | **Medium** |
| A | `Vec2/4`, `Quat`, `Mat3x4/4` value types | Yes | Low, after A.1 |
| **B** | **Target-architecture vertical slice** | — it *is* the target | Medium |
| C | `continue`/`break` | No — do anytime | Low |
| C | `List` removal ops | No — do anytime | Low |
| C | `popcount` and friends | No — do anytime | Low |
| D | SIMD lowering for `Vec4`/`Quat` | Pulled in by B | Medium |
| D | WebGPU `shader-f16` feature | No (runtime only) | Low |
| D | `Float16` storage type | Probably never | Low |
| E | Threading hand-off / safe snapshots | Yes, for the worker | High |
| E | SoA support | Not yet | High if done early |
| E | Function references | Not yet | Medium |

Two sentences, if only two are read:

**Fixed-size value types and checked ownership transfer are the two
foundations; matrices allocating on the heap is the bug that proves the
first, and a silent aliasing double-free across a thread boundary is the bug
that would prove the second.** Everything after them should be demanded by
the real renderer, not predicted by this document.
