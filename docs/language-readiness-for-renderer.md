# Language readiness for the target renderer

Status: **proposal**. Nothing here is implemented. This document audits what
Rae lacks for the renderer we actually want, and proposes the minimum
language/runtime work — in dependency order — to make that renderer
expressible *naturally* rather than through workarounds.

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

Not "missing features", but they are language-level hazards that will recur
under renderer load and belong in this audit:

- `List(T).add`/`set` take `value: own T`; passing a `view String` makes the
  container alias caller memory and double-free. Hit in `log_overlay`.
- `List(Struct)` does not reliably deep-copy inner heap fields.
- Chained generic-method calls mis-resolved silently until #349.

These share a root: **ownership transfer at container boundaries is not
checked, it is assumed.** Under a renderer that moves thousands of
transforms per frame, silent aliasing is a class of bug that will be very
expensive to find.

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

## 4. What is *not* a language feature

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

## 5. Proposed work, in dependency order

### Tier 0 — cheap, unblocking, no design risk

Small, self-contained, and each removes an active workaround.

**0.1 `continue` and `break`.** Loop control with the obvious semantics.
Parser + codegen; `break` interacts with `defer` and scope-exit drops, which
is the only subtlety.

**0.2 `List` removal operations.** `removeLast()`, `truncate(len)`,
`removeAt(index)`. Must run element drop for heap element types — the same
path `set` already uses via `rae_ext_rae_buf_drop_at`.

**0.3 Bit helpers.** `popcount`, `leadingZeros`, `trailingZeros`. Needed for
visibility-bitmask work on the CPU side and generally useful for packed
data. Lowers to compiler intrinsics.

*Exit criterion: the three existing workarounds named in §1 are deleted.*

### Tier 1 — the foundation (the real work)

**1.1 Fixed-size value aggregates.** The core addition. Some form of
`[N]T` — a value type, no heap, copied by value, embeddable in structs and
in `List(T)` elements. Design questions to settle deliberately:

- Spelling and whether `N` is a const-expression
- Interaction with ownership modes (`view`/`mod`/`own`/`copy`) — a value
  array should be trivially copyable and never need cascade-drop when `T`
  is primitive
- Indexing bounds behaviour (checked in debug, unchecked in release?)
- Whether it is a distinct kind or sugar over an internal `Buffer` with a
  compile-time length

**1.2 Value math types built on 1.1.** `Vec2`, `Vec4`, `Quat`, `Mat3x4`,
`Mat4` as `type` declarations with value semantics and no allocation.
`Mat3x4` specifically because affine transforms need no fourth row — the
reference engine passes `mat3x4` across its render boundary for exactly this
reason, saving 25% of transform bandwidth.

**1.3 Port `math3d.rae` to the value types.** Delete the `List(Float)`
matrix representation. This is the payoff step and the proof that 1.1 works:
`modelMatrix()` should go from eight allocations to zero.

*Exit criterion: a transform-heavy microbenchmark shows zero allocations per
object per frame.*

### Tier 2 — performance, once the foundation is real

**2.1 SIMD lowering.** Not intrinsics in user code. The C backend should
lower `Vec4`/`Quat` operations to `__m128` / `float32x4_t` where the target
supports it, with a scalar fallback. The reference engine hand-wrote SIMD
for exactly one operation — quaternion multiply — because that is the hot
inner loop of skeletal animation. **Start there; measure before widening.**

**2.2 WebGPU `shader-f16` device feature.** Runtime change only (§4).
Enables half-precision in AO, blur, bloom, and lighting shaders.

**2.3 `Float16` as a storage type.** Only if profiling shows CPU-side
packing is hot. Conversion helpers, not arithmetic — f16 arithmetic on the
CPU is rarely worthwhile.

### Tier 3 — structural, driven by the renderer's actual shape

**3.1 Threading hardening for a render worker.** The target architecture
puts the renderer on its own thread behind a command stream. `spawn` already
produces real OS threads in the compiled target, but a render worker needs
more than thread creation:

- A safe hand-off type: ownership transfer across threads that the compiler
  checks, so a scene packet cannot be mutated after being queued
- Some notion of "this data is immutable now" for snapshot/interpolation
- Bounded queues with backpressure

This is the largest and least-specified item. It should be designed against
the actual command-stream protocol, not in the abstract.

**3.2 SoA support — only if needed.** The reference engine's skeleton is
struct-of-arrays. Rae can express that today with parallel `List`s; it is
verbose but correct. **Recommendation: do not add SoA syntax yet.** Get
array-of-structs fast and allocation-free first (Tier 1), then measure. SoA
sugar designed before there is a consumer will almost certainly be wrong.

**3.3 Function references — only if a data-driven graph demands it** (§4).

---

## 6. Ordering rationale, and one methodological warning

The tiers are strictly dependency-ordered: Tier 2's SIMD lowering is
meaningless without Tier 1's value types, and Tier 1's value types are
easier to implement once Tier 0's loop control exists (the codegen for
`break` touches the same scope-exit machinery as value-array drops).

**The warning.** "Finish the language, then build the renderer" is the right
*direction* but a dangerous *literal plan*. Language features designed
without a real consumer get their details wrong, and the details are what
cost time later. Recent evidence from this codebase: the missing
`AST_EXPR_INTERP` case in `infer_expr_type_ref` survived indefinitely
because nothing exercised chained generic calls until an app did; the
`List.set` ownership hazard only surfaced when a real ring buffer used it.

**Recommendation: implement each tier against a thin vertical slice of the
target renderer, not against a specification.** Concretely — Tier 1 is
"done" when the transform path of a real draw call is allocation-free, not
when `Mat4` compiles. That keeps the work honest without reintroducing
incremental migration of the old renderer.

---

## 7. Summary

| Tier | Item | Blocking? | Risk |
|---|---|---|---|
| 0 | `continue`/`break` | Convenience | Low |
| 0 | `List` removal ops | Convenience | Low |
| 0 | `popcount` and friends | Convenience | Low |
| **1** | **Fixed-size value aggregates** | **Yes — everything** | **Medium** |
| 1 | `Vec2/4`, `Quat`, `Mat3x4/4` value types | Yes | Low, after 1.1 |
| 1 | Port `math3d` off `List(Float)` | Yes | Low |
| 2 | SIMD lowering for `Vec4`/`Quat` | Performance | Medium |
| 2 | WebGPU `shader-f16` feature | Performance | Low (runtime only) |
| 2 | `Float16` storage type | Probably not | Low |
| 3 | Threading hand-off / safe snapshots | Yes, for the worker | High |
| 3 | SoA support | Not yet | High if done early |
| 3 | Function references | Not yet | Medium |

The one-line version: **fixed-size value types are the prerequisite for
everything else, and matrices allocating on the heap is the bug that proves
it.**
