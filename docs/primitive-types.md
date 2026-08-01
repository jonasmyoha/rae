# Rae primitive types

This is the normative reference for Rae's primitive numeric model. It is a
**permanent language-design decision**, not an implementation detail.

## The model

| Rae type  | Representation            | C type     | Bits |
| --------- | ------------------------- | ---------- | ---: |
| `Float`   | IEEE-754 binary32 (alias) | `float`    |   32 |
| `Float32` | IEEE-754 binary32         | `float`    |   32 |
| `Float64` | IEEE-754 binary64         | `double`   |   64 |
| `Int`     | signed integer (alias)    | `int64_t`  |   64 |
| `Int64`   | signed integer            | `int64_t`  |   64 |
| `Int32`   | signed integer            | `int32_t`  |   32 |
| `UInt64`  | unsigned integer          | `uint64_t` |   64 |
| `UInt32`  | unsigned integer          | `uint32_t` |   32 |
| `Bool`    | boolean                   | `rae_Bool` |    — |
| `Char`    | Unicode scalar            | `uint32_t` |   32 |

There is no `Double`, `Single`, `Real` or `Long`. They would be redundant
spellings of types that already exist, and `Long` in particular means
different widths on different platforms — exactly the ambiguity Rae avoids.

## `Float` is an alias, not a third type

```
Float == Float32     // the same type, not merely the same width
Float != Float64
```

Both spellings resolve to **one interned type** in the compiler, so they
share type identity, equality, ABI, layout and monomorphization identity.
That last one matters most:

```
List(Float)     and  List(Float32)    →  one instantiation
List(Float64)                          →  a distinct instantiation
```

This is guaranteed by construction rather than by convention: `Float` and
`Float32` both resolve to `type_get_float()`, while `Float64` is a separate
`TypeKind` (`TYPE_FLOAT64`) that interns and mangles separately.

Write `Float` in ordinary code. Write `Float32` when you specifically want to
signal representation intent — a vertex buffer, a GPU-facing struct, a
packing layout. They compile to exactly the same thing.

## Why the default is f32

Most languages default floating point to f64: Python's `float`, JavaScript's
`number`, C's promotion rules. Rae deliberately does not, because Rae targets
a different domain.

1. **Rae is a game/graphics language.** `Float` overwhelmingly appears in
   vectors, matrices, transforms, geometry, UI coordinates, colors, vertices
   and particles. The native representation for that data is f32.
2. **GPU compatibility.** WGSL's ordinary float is `f32`. With `Float` = f32,
   CPU-side graphics data already matches the GPU representation, instead of
   crossing a routine f64 → f32 narrowing boundary on every upload.
3. **Memory footprint.** f32 is half the size of f64 — decisive for large
   arrays of vectors, transforms, ECS components, particles, vertices and
   animation data.
4. **Cache and bandwidth.** Twice the values per cache line, half the memory
   traffic, for exactly the data-oriented workloads Rae is built around.
5. **SIMD.** A SIMD register holds twice as many f32 lanes as f64 lanes, so
   f32 is the natural width for vectorized graphics and numeric work.
6. **"Wider is safer" is not a good default.** f64 is not inherently safer
   for graphics data. It costs memory and bandwidth and does not match the
   GPU. A default should represent the *common* case; explicit widths give
   deliberate control for the rest.

## When to choose `Float64`

Reach for `Float64` when precision is a genuine requirement, not as a
reflex:

- **Absolute timestamps.** Epoch seconds (~1.7 × 10⁹) in f32 have roughly
  128-second granularity — two calls one frame apart return the *same*
  value, making every delta exactly zero. `gpu2d.nowSeconds()` and
  `sys.fileMtime()` return `Float64` for this reason.
- **Large-world coordinates.** See below.
- **Scientific / numerical work**, accumulations over many terms, or any
  computation where error compounds.

## Open worlds do not need f64 vectors

Large worlds are *not* a reason to make ordinary `Vec3` f64. The established
techniques — floating origin, camera-relative rendering, or a dedicated
high-precision world-position type — keep world coordinates precise while
rendering and local-space math stay f32.

The intended split:

```
ordinary graphics / local-space math   →  Float   (f32)
world coordinates, high precision      →  Float64 (f64), explicitly
```

Doubling the size of every vector and transform in the engine to solve a
problem that only affects position-in-world is the wrong trade.

## UI

UI arithmetic is rarely the bottleneck, so performance alone would not
decide this. But UI geometry ends up in the same GPU pipeline as everything
else, and f32 is the natural representation there — so UI uses `Float` like
the rest of the language.

## Vectors and math types

`Vec2`, `Vec3` and the matrix/transform helpers declare their fields as
`Float`, so they are f32-backed:

```rae
type Vec3 {
    x: Float   # f32
    y: Float   # f32
    z: Float   # f32
}
```

A `Vec3` is 12 bytes, not 24, and uploads to a GPU buffer without
conversion.

## FFI and layout

The FFI boundary is literal — there is no hidden widening:

```
Rae Buffer(Float)   ↔  C float*
Rae Buffer(Float32) ↔  C float*
Rae Buffer(Float64) ↔  C double*
Rae Float           ↔  C float
Rae Float64         ↔  C double
```

Generated C uses plain `float` and `double`. Rae introduces **no** numeric
typedefs (`rae_f32`, `f64`, `float32_t`, …) — the compiler knows the
semantic width, and idiomatic C expresses it directly.

When writing a C function for a Rae `extern`, the signature must match
exactly. A Rae `Buffer(Float)` reaching a C `const double*` is not a
precision issue — it is memory misinterpretation, reading 8-byte doubles out
of 4-byte floats. Declare externs in the shared runtime header where
practical, so the C compiler catches a mismatch instead of leaving it
silent.

## Literals

An unsuffixed float literal (`0.1`) has type `Float`, i.e. f32. Assigning it
to a `Float64` binding gives an f64 value:

```rae
let a: Float   = 0.1   # f32
let b: Float64 = 0.1   # f64
```

## Verifying the semantics

The discriminator used by the test suite is 2²⁴ = 16777216, the last integer
f32 represents exactly:

```rae
let a: Float = 16777216.0
# In f32 this comparison is TRUE; in f64 it would be false.
if a + 1.0 is a { log("Float is f32") }
```

See `compiler/tests/cases/550_float_is_f32`.
