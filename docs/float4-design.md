# Float4 — a 4-wide float type for SIMD (design + measured prototype)

Status: **proposal, awaiting the maintainer's approval of the surface** (§7).
Written 2026-10-04 after the scalar contact solver landed (physics port P4).
Background: `docs/physics-performance-plan.md` §2-§3 and §5 item 4.

## 1. Why

Box3D's NEON build is 1.35-1.5x faster than its scalar build on contact-heavy
scenes (the 5 050-box pyramid: 8.1 vs 11.8 ms per step single-threaded, 2.75
vs 3.8 ms at 4 workers), and that margin is what keeps the featured physics
example within its 4 ms budget unless the port runs within ~1.05x of C
(performance plan §3). The port already has the wide solver's *structure*:
`lib/physics/dynamics/FloatWide.rae` is Box3D's scalar `b3FloatW`, a struct
of four Floats operated on lane by lane. Float4 is the type that lets that
same code run on SIMD registers.

Box3D keeps its SIMD path **bit-identical** to its scalar one (no approximate
reciprocal square root in `contact_solver.c`; "needs to match the SIMD
version" in `convex_manifold.c`). Float4 must do the same: the port's
determinism rule (performance plan §7) says identical bits for scalar and
Float4, and the oracle fixtures (948-959) keep checking it.

## 2. The proposed surface

A builtin value type in the prelude, beside `Float`:

```rae
let lanes: Float4 = { x: 1.0, y: 2.0, z: 3.0, w: 4.0 }
let half: Float4 = Float4.splat(value: 0.5)
let scaled: Float4 = lanes * half + Float4.splat(value: 1.0)    # lane-wise, never fused
let second: Float = scaled.y                                     # lanes read like fields
let above: Mask4 = Float4.greaterThan(left: scaled, right: half)
let chosen: Float4 = Float4.select(ifFalse: lanes, ifTrue: scaled, mask: above)
if Mask4.any(mask: above) { ... }
```

- **`Float4`**: a value type of four Float lanes named `x`, `y`, `z`, `w`
  (readable like fields; built with the usual typed literal, LHS type on the
  left). Arithmetic operators `+ - * /` and unary `-` work lane-wise, exactly
  as on `Float`; there are no other operators.
- **Functions** (lane-wise unless stated): `splat(value:)`, `sqrt(value:)`,
  `min(left:right:)` = `left <= right ? left : right`, `max(left:right:)` =
  `left >= right ? left : right`, `abs(value:)` = `value < 0 ? -value : value`,
  `mulAdd(base:left:right:)` = `base + left * right` (two roundings, never
  fused), `clampSymmetric(value:limit:)`.
- **`Mask4`**: the result of a comparison, a separate type so a mask can
  never be mistaken for numbers: `greaterThan`, `lessThan`, `greaterOrEqual`,
  `lessOrEqual`, `equal` (all `ret Mask4`); `Mask4.and`, `or`, `andNot`;
  `Mask4.any(mask:)`, `Mask4.all(mask:)`; `Float4.select(ifFalse:ifTrue:mask:)`.
- **Horizontal operations** with a defined order, so they are deterministic
  everywhere: `sum(value:)` = `((x + y) + z) + w`, `horizontalMin`,
  `horizontalMax`, and `minLaneIndex(value:bitCount:)` for Box3D's
  index-in-mantissa support search (`b3EmbedIndexW` / `b3MinIndexW`, used by
  the hull SAT).
- **Load and store** from Float storage, following List's no-trap rule:
  `Float4.load(from: view List(Float), index:) ret opt Float4` (`none` unless
  `index + 3 < length`), `Float4.store(into: mod List(Float), index:, value:)`
  (out of range: ignored with a warning, like `List.set`); the same for
  `Array(Float, cap: N)` with the range checked at compile time where the
  index is constant.

`Float4` is not an `@`-anything and needs no new keyword: it is a type name
and a set of functions, like `List`.

## 3. Lowering (C backend)

- `Float4` is emitted as the platform's 128-bit vector type, `Mask4` as its
  integer vector: `float32x4_t` / `uint32x4_t` on arm64 (NEON), `__m128` on
  x86-64 (SSE2, the x86-64 baseline: no `-msse4` needed), `v128_t`
  (`wasm_simd128.h`) on wasm32 with `-msimd128`. Any other target gets a
  struct of four floats and the scalar definitions — the lib's `FloatWide`
  semantics, which is also the reference every other lowering must match.
- Each operation is a `static inline` function or macro in a runtime header
  the generated C includes (as `c/float4_prototype.h` does in the
  prototype), never a call into the separately compiled runtime.
- Lane reads (`v.y`) lower to a lane extract; the typed literal to a lane
  set; `+ - * /` to the vector instructions.

## 4. Bit-exactness rules

Every lowering produces the scalar definition's bits:

- `+ - * /` and `sqrt` are IEEE-754 single precision in every lane (true of
  NEON, SSE2 and wasm SIMD128). `-ffp-contract=off` (already on for every
  Rae build) keeps the C compiler from fusing them.
- `mulAdd` is a multiply then an add — never `vfmaq` / FMA.
- `min` / `max` are compare-and-select, not `vminq` / `_mm_min_ps`: the
  instructions differ from `a <= b ? a : b` on `-0` and NaN.
- `abs` is compare-and-select too: `vabsq` turns `-0` into `+0`, the scalar
  definition keeps `-0`.
- No approximate reciprocal or reciprocal square root.

The prototype follows these rules (`benchmarks/float4_solver/c/
float4_prototype.h`) and is bit-exact against the scalar path and against
Box3D's own scalar and NEON builds (§5).

## 5. The prototype and its numbers

`benchmarks/float4_solver/run.sh` runs the wide convex contact kernels (warm
start, push, relax; 8 192 wide constraints = 32 768 contacts over 16 384
bodies; 30 steps of 4 sub-steps) five ways on identical synthetic data:

- `c_scalar`, `c_simd` — Box3D's own `contact_solver.c` from the oracle
  cache, `BOX3D_DISABLE_SIMD` and NEON;
- `rae_scalar` — `lib/physics/dynamics/ContactSolverWideSolve` as it is;
- `rae_float4` — the same solver modules over a copy of `FloatWide` whose
  primitives are SIMD (`benchmarks/float4_solver/float4/`), lowered by the
  prototype header — what the builtin would emit, written by hand;
- `rae_float4_vector` — the same, with `FloatWide` retyped as a clang vector
  in the generated C.

Apple M1 Max, best of 5, **load average ~8 from unrelated work** (indicative,
re-run on a quiet machine before quoting). Every build printed the same
checksum (FNV over all body velocities) — **bit-exact across all five**.

| build | release (`-O2`) | + `-mllvm -inline-threshold=3000` |
|---|---|---|
| c_scalar | 532.7 ms | 534.0 ms |
| c_simd (NEON) | 321.3 ms | 330.0 ms |
| rae_scalar | 610.9 ms | 510.4 ms |
| rae_float4 | 459.9 ms | 379.1 ms |
| rae_float4_vector | 461.6 ms | 382.4 ms |

| ratio | `-O2` | + inlining |
|---|---|---|
| C: scalar / SIMD | 1.66x | 1.62x |
| rae_scalar / c_scalar | 1.15x | 0.96x |
| **rae_scalar / rae_float4** | **1.33x** | **1.35x** |
| rae_float4 / c_simd | 1.43x | 1.15x |

## 6. What the prototype shows

1. **Float4 buys Rae what NEON buys Box3D on this kernel**: 1.33-1.35x over
   the scalar Rae solver, against C's 1.62-1.66x, with the same bits.
2. **The representation is not the bottleneck.** `rae_float4_vector` (a real
   vector type) is no faster than `rae_float4` (a struct of four floats with
   SIMD primitives): once the primitives inline, the C compiler keeps the
   struct in registers. What matters is that the primitives are *compiler
   intrinsics* — inline C — and not calls into the separately compiled
   runtime. The builtin lowering of §3 is therefore the simple, robust
   choice, not a requirement for speed.
3. **Inlining is worth as much as SIMD here.** At the release profile's
   `-O2`, clang leaves the solver's small helpers (`applyWideImpulse`,
   `multiplySym3Wide`, `gatherBodies`, …) out of line, passing structs
   through memory; a higher inline threshold makes `rae_scalar` *faster than
   C scalar* (0.96x) and closes the Float4 gap to C SIMD from 1.43x to 1.15x.
   This is independent of Float4 (queued separately, §8).
4. **Scalar `Math.sqrt` is a runtime call** (`rae_ext_Math_sqrt`, an extern
   into the runtime object), visible in the scalar solver's profile. It
   should be an inline intrinsic like Float4's (queued separately, §8).
5. The remaining 1.15x to C SIMD is the gather/scatter of body states through
   `List` (bounds-checked copies of 64-byte states, per lane) and `Array`
   lane reads; `Float4.load` from a flat `List(Float)` is the obvious next
   measurement once the type exists.

## 7. Questions for the maintainer

1. The surface of §2: a builtin `Float4` with lane fields `x y z w`,
   lane-wise `+ - * /`, the named functions, and a separate `Mask4` type for
   comparisons — or masks as Float4 (Box3D's scalar path uses 1.0 / 0.0)?
2. Operators on `Float4` at all, or named functions only (`Float4.add(left:
   right:)`)? The prototype's solver uses named functions throughout; the
   operators would only read better.
3. `Float4.load` returning `opt Float4` (List's rule) — acceptable on hot
   paths, or should a load with a statically known in-range index (Array)
   be the main form?
4. Where it lives: prelude (`lib/core`, like `List`, because the compiler
   lowers it) — consistent with the core-vs-collections rule in AGENTS.md.

## 8. After approval

- Implement `Float4` / `Mask4` in the compiler (type, operators, the
  functions as intrinsics, the three lowerings + the scalar fallback), with
  fixtures checking each operation's bits against the scalar definitions.
- Make `lib/physics/dynamics/FloatWide.rae` a thin layer over Float4 (the
  solver does not change), then re-run fixtures 948-959 and this benchmark.
- Independent of the surface, queued now: raise the release build's inline
  threshold (or emit small functions `static inline`) after measuring the
  whole suite; make `Math.sqrt` (and the other scalar math the solver uses)
  an inline intrinsic.
