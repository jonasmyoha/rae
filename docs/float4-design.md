# Float4 — a 4-wide float type for SIMD (design + measured prototype)

Status: **approved 2026-10-04, with changes** (§0 is the decision; it
overrides §2 and answers §7). Written 2026-10-04 after the scalar contact
solver landed (physics port P4). Background: `docs/physics-performance-plan.md`
§2-§3 and §5 item 4.

## 0. The decision

The maintainer approved Float4 with two of the four §7 questions answered
differently from the proposal:

1. **`Mask4` stays a separate type.** A mask stored as a Float4 would be NaN
   bit patterns that only work by accident, and its type would allow
   arithmetic on a comparison. Mask4 exposes only `select`, `and` / `or` /
   `not`, and `any` / `all`.
2. **Named functions only — no lane-wise operators.** Rae has no operator
   overloading on structs (`docs/timestamp-and-duration.md`), and `Vec4` uses
   `vadd` / `vmul`; `+` working on Float4 but not on the scalar fallback or
   Vec4 would make what compiles depend on the type. The prototype agrees:
   the speedup comes from inline primitives, not syntax. Named functions also
   make the bit-exact contract visible — `mulAdd` is unfused because its name
   says so, and there is no `*` then `+` for a backend to contract:

   ```rae
   let d: Float4 = Float4.sub(a: p, b: q)
   let r: Float4 = Float4.mulAdd(a: d, b: n, c: bias)
   let m: Mask4 = Float4.less(a: r, b: zero)
   let out: Float4 = Float4.select(mask: m, ifTrue: r, ifFalse: zero)
   ```

3. **The load returns `opt Float4`**, matching `copyAt` and the
   no-escape-hatch rule; a hot loop does one `if let` per batch and hoists
   the bounds check. The Array-indexed form waits until something like
   `Array(Float, cap: 4)` needs it.
4. **Not in the prelude:** an imported module, `lib/Float4.rae`, like `Vec4`
   and `Channel`, with the builtin lowering behind it. Only the physics and
   renderer kernels need it.

Go-ahead for §8 (the compiler work and the FloatWide switch), with the
bit-exact check against the scalar oracle kept as a **permanent fixture**, not
only a benchmark assertion. `Vec4` is left alone; re-implementing it over
Float4 is a separate ticket, to wait until a measurement asks for it.

### 0.1 As implemented (2026-10-04)

`lib/Float4.rae` (imported, not in the prelude) declares `type Float4 { x y z
w: Float }` and `type Mask4` and the named functions; every function is an
`unsafe extern("rae_f4_*")` native implemented as inline code in
`compiler/runtime/runtime_float4.h` — NEON, SSE2, wasm SIMD128, and the scalar
definitions as the fallback. The generated C includes that header only when a
program uses Float4. `rae run --float4-scalar` (-DRAE_FLOAT4_SCALAR) forces the
scalar path. Fixtures 967_float4_bit_exact (the SIMD lowering) and
968_float4_bit_exact_scalar (the same program, scalar) check every function
lane by lane against its scalar definition, bit for bit, over inputs that
include -0, +-inf, NaNs with a sign and payload, denormals and the extremes,
and masks with arbitrary bits. Verified on arm64 (NEON and scalar) and on wasm
SIMD128 (emcc -msimd128, run in node: the same output); the SSE2 path compiles
for x86-64 but was not run (no x86 machine or Rosetta here).

Where it differs from the text above:

- **Mask names.** `and`, `or`, `not` and `any` are Rae keywords, so the mask
  functions are `both`, `either`, `invert`, `anyLane` and `allLanes`. Mask4's
  four lanes are `UInt32` fields (`bitsX` ...) because Rae has no private
  fields; nothing operates on them except these functions and `select`.
- **A lane is true when any of its bits is set**, and `select`, `both`,
  `either` and `invert` are bitwise in every lowering, so a hand-built Mask4
  gives the same result on every path.
- **`mulAdd(a:b:c:)` is `a * b + c`** (two roundings), the usual argument order
  of a multiply-add.
- **Two NaN inputs.** When two or more inputs of an arithmetic function are
  NaN, which one the result carries is not fixed: IEEE 754 leaves it open, and
  the C compiler may swap the operands of a commutative `x + y` even in plain
  scalar code (the fixture's first run caught exactly that). The result is
  then a quiet NaN equal to one of the NaN inputs; everything else is
  bit-exact.
- **Added:** `negate` (FloatWide needs it), `embedIndex` (the other half of
  `minLaneIndex`, Box3D's b3EmbedIndexW) and `lowering()` (which path the
  program was compiled with).

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

## 2. The proposed surface (superseded where §0 differs)

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
   This is independent of Float4. **Done 2026-10-04: the release profile now
   passes `-mllvm -inline-threshold=400`** (clang's default is 225; only when
   `rae` itself was built by clang, since the flag is clang's). Measured on an
   Apple M1 Max at load average ~4 (indicative):

   | threshold | rae_scalar | / C scalar | rae_float4 | / C NEON |
   |---|---|---|---|---|
   | 225 (default) | 589.7 ms | 1.14x | 448.5 ms | 1.40x |
   | 300 | 542.9 ms | 1.06x | 450.8 ms | 1.41x |
   | **400** | 510.3 ms | 1.00x | 373.9 ms | 1.16x |
   | 500 | 510.5 ms | 0.98x | 373.8 ms | 1.18x |
   | 1000 | 502.2 ms | 0.97x | 373.9 ms | 1.17x |
   | 3000 | 507.4 ms | 0.98x | 371.8 ms | 1.16x |

   The cost, compiling the generated C of all 77 examples to objects (two at
   a time) and summing:

   | threshold | C compile time | code (`__TEXT`) |
   |---|---|---|
   | 225 | 102 s | 9.72 MB |
   | **400** | 123 s (+20%) | 11.90 MB (+22%) |
   | 500 | 131 s (+27%) | 12.38 MB (+27%) |
   | 1000 | 157 s (+52%) | 14.44 MB (+49%) |
   | 3000 | 203 s (+96%) | 18.97 MB (+95%) |

   The largest, 121_ui_editor, compiles in 15.8 s at 225 and 32.5 s at 3000.
   The runtime object (`rae_runtime.c`, compiled with every program) costs
   +9% time and +9% code at 400. 400 is where the solver gain stops, so it is
   the release default; everything above it is cost only. Forcing small
   loop-free functions inline instead (`always_inline` on bodies of at most
   6 or 12 statements) made the solver *slower* (622 ms): the threshold lets
   clang weigh callers too. `RAE_EXTRA_CFLAGS` appends flags to the release C
   compile, for measuring a flag across programs before it becomes a
   default.
4. **Scalar `Math.sqrt` was a runtime call** (`rae_ext_Math_sqrt`, an extern
   into the runtime object), visible in the scalar solver's profile. Done
   2026-10-04: the exactly specified scalar functions (sqrt, floor, ceil,
   round, remainder, floatBits/floatFromBits) are `static inline` in
   `rae_runtime.h`, so `sqrt` compiles to `fsqrt`; the transcendental ones
   (sin … log, pow) stay out of line, because a C compiler folding
   `sinf(constant)` with its own implementation could change the bits.
   `rae_scalar` went from 603.6 to 588.1 ms on this benchmark (load average
   ~4, indicative), checksums unchanged.
5. The remaining 1.15x to C SIMD is the gather/scatter of body states through
   `List` (bounds-checked copies of 64-byte states, per lane) and `Array`
   lane reads; `Float4.load` from a flat `List(Float)` is the obvious next
   measurement once the type exists.

## 7. Questions for the maintainer (answered in §0)

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
- Independent of the surface, both done 2026-10-04: the release build's
  inline threshold is 400 (§6.3) and the exactly specified scalar math is
  inline (§6.4).
