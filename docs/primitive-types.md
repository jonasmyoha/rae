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
| `Int16`   | signed integer            | `int16_t`  |   16 |
| `Int8`    | signed integer            | `int8_t`   |    8 |
| `UInt64`  | unsigned integer          | `uint64_t` |   64 |
| `UInt32`  | unsigned integer          | `uint32_t` |   32 |
| `UInt16`  | unsigned integer          | `uint16_t` |   16 |
| `UInt8`   | unsigned integer          | `uint8_t`  |    8 |
| `Bool`    | boolean                   | `rae_Bool` |    — |
| `Char`    | Unicode scalar            | `uint32_t` |   32 |

Each fixed-width integer is a **distinct first-class type**, not a display alias
of `Int`: it interns separately, so `List(Int32)`, `List(Int16)` and `List(Int)`
are three different monomorphizations with byte-accurate element layout
(`int32_t[N]`, `int16_t[N]`, `int64_t[N]`) — essential for GPU vertex/index/
instance buffers (#507). `Int` and `Int64` are the same type (both 64-bit
signed), like `Float`/`Float32`. Two display caveats, harmless for storage and
ABI (values pass to C, not `log`): interpolating a `UInt32`/`Char32` formats it
as a character (shared `uint32_t`), and interpolating an `Int8` formats it as a
bool. See `compiler/tests/cases/630_fixed_width_int_lists`.

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

## No implicit numeric conversions

**Numeric types never silently change representation.** Rae has no implicit
numeric conversions — not narrowing, and not the "safe" widening ones
either. Every conversion between distinct numeric types is written out by
the programmer. Attempting one implicitly is a hard compile-time error, not
a warning:

```
cannot implicitly convert Float64 to Float; Rae has no implicit numeric
conversions - write `value as Float`
```

This is not pedantry. The f32 migration produced a concrete instance of the
bug class it prevents: an absolute `Float64` epoch timestamp assigned into a
`Float` was silently quantised to ~128-second granularity, so every
per-frame delta became exactly `0.0`. Animation froze and the FPS readout
showed zero, with no diagnostic anywhere. A warning would have scrolled past.
An error stops it.

Note that widening is rejected too. `Float` → `Float64` loses nothing
numerically, but permitting it would mean the compiler quietly decides where
representations change — and the moment that is allowed in one direction,
"where did this value's precision come from?" stops being answerable by
reading the code.

## Explicit conversion: `as`

```rae
value as TargetType
```

```rae
let elapsed: Float    = timestamp as Float
let precise: Float64  = value as Float64
let count: Float      = total as Float
```

`as` means *convert this expression to that type using Rae's conversion
rules*. It is not an escape hatch for making something type-check:
non-numeric conversions are rejected.

`as` binds tighter than every binary operator, so `b as Float * 2.0` is
`(b as Float) * 2.0` — the reading you expect. Conversions chain:
`i as Float as Float64`.

### Aliases are not conversions

Because `Float` **is** `Float32`, these are not conversions and need no
`as`:

```rae
let a: Float   = 1.5
let b: Float32 = a     # same type — no cast
let c: Float   = b     # same type — no cast
```

### Literals are not conversions

An unsuffixed literal is materialised directly in the destination type, so
it never needs a cast:

```rae
let x: Float   = 1.5   # fine
let y: Float64 = 1.5   # fine
let i: Int     = 42    # fine
```

`let x: Float = 1.5 as Float` is unnecessary noise.

### The timestamp pattern

This is the canonical case, and the reason the rule exists:

```rae
# Absolute times are Float64. The subtraction keeps full epoch precision;
# only the small delta narrows, and that narrowing is visible in the source.
var lastFrame: Float64 = gpu2d.nowSeconds()
let now: Float64       = gpu2d.nowSeconds()
let dt: Float          = (now - lastFrame) as Float
```

A `Vec3` position stays `Float`; a precise clock stays `Float64`; the
boundary between them is spelled out:

```rae
let position: Vec3
let preciseTime: Float64
let renderTime: Float = preciseTime as Float   # deliberate: precision drops here
```

## Verifying the semantics

The discriminator used by the test suite is 2²⁴ = 16777216, the last integer
f32 represents exactly:

```rae
let a: Float = 16777216.0
# In f32 this comparison is TRUE; in f64 it would be false.
if a + 1.0 is a { log("Float is f32") }
```

See `compiler/tests/cases/550_float_is_f32` (widths and identity) and
`551_as_conversion` (explicit conversion).
