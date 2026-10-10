# Rae kernels: one source, scalar, SIMD and GPU (design only)

Status: **proposal, nothing implemented. The maintainer decides** (forks in §10).
Prompted by linebender/fearless_simd (2026-10-08) and Bend 2 (one source to C
and CUDA).

## 1. The question

Can a kernel be written ONCE, in a restricted Rae subset, and run as:

1. plain scalar Rae (the reference, and the fallback everywhere);
2. Float4 / Float8 SIMD (docs/float4-design.md);
3. a GPU compute shader (Rae → WGSL, dispatched through gpu/GpuCompute)?

Today each form is written by hand.
- **Scalar and SIMD:** the scalar loop and the Float4 loop, with their
  tails (§9 of the Float4 doc).
- **GPU:** a WGSL file composed with `shader(files:)`, with its own
  bounds check (lib/gpu/computeBounds.wgsl).

Three copies of one computation can drift, and only fixtures 967 / 1088 keep
the CPU pair honest.

**Short answer:** yes for a narrow, useful class: element-wise maps and simple
reductions over flat Float / Int columns. That class is where Rae already has
all three forms by hand: grass blades, water, physics wide-solver rows,
particle updates. A general "any Rae function on the GPU" is out of reach
and not worth it (§3, §9).

## 2. What a kernel is

A **kernel** is a function over ONE element index, with:

- **inputs:** `view List(Float)` / `List(Int)` / `List(UInt32)` columns,
  scalars (`Float`, `Int`, `Bool`), and plain-value structs of those
  (`Vec3`, `Quat`);
- **outputs:** `mod List(...)` columns, written only at the element's own
  index;
- **a body** in the subset (§3).

The compiler then emits up to three forms:

- **scalar:** a loop over `0 ..< count` calling the body;
- **SIMD:** the body with every Float lane-widened to Float4 / Float8, plus the
  masked tail. The kernel is "vectorized across elements", ISPC's model.
- **GPU:** a WGSL compute entry point. Columns become `array<f32>` storage
  bindings, scalars go into a uniform, and the body is the guarded
  `computeElementIndex` invocation.

How a kernel is declared is fork F1. Rae spells modifiers as bare keywords,
never `@` attributes, so the candidates are a `kernel` modifier on a `func`,
or no marker at all, with the compiler checking the subset wherever such a
function is dispatched.

## 3. The subset (what it must exclude)

**Allowed:**

- arithmetic on `Float` / `Int` / `UInt32` / `Bool`;
- `if` / `else` / `match` on those (lowered as select on SIMD/GPU, §5);
- `let` / `var` locals;
- calls to other kernel-subset functions (inlined);
- reads of input columns at the element's index or at a computed index (a
  gather);
- writes of output columns at the element's own index;
- `loop` with a bound the compiler can see (a count of constants or
  scalar parameters).

**Excluded, and why:**

| excluded | reason |
|---|---|
| heap types (`String`, `List` creation, `Map`, `Box`, `Any`) | no allocator on the GPU; a SIMD lane cannot own heap |
| recursion | WGSL forbids it; on SIMD each lane would need its own stack |
| unbounded `loop` (data-dependent exit) | allowed on scalar, but on SIMD/GPU it means divergence: every lane runs until all are done. Allowed with a declared bound (F4) |
| writes to another element's slot (scatter) | a race on SIMD/GPU; needs atomics (F6) |
| `spawn`, `taskScope`, `parallelLoop`, I/O, `log` | the kernel IS the parallel unit |
| `Float64` on the GPU | WGSL has no f64 (mobile, web); a Float64 kernel stays CPU-only |
| externs, `unsafe` | no C on the GPU; the CPU forms could, but then the forms diverge |

Every rejection is a sema error naming the construct and the form it blocks
("kernel 'spread' cannot run on the GPU: `log` at line 12"), so a kernel
that is only scalar+SIMD still compiles.

## 4. How data crosses

- **CPU forms:** columns are `List(Float)` (or an ECS component column, which
  is a list of structs).
  - **Scalar:** reads the list directly.
  - **SIMD:** needs struct-of-arrays. A column of `Vec3` is three Float
    columns, x / y / z (the physics solver's FloatWide layout). An
    array-of-structs component column is either transposed into scratch
    lists per dispatch (cost O(n), F5) or stored SoA from the start (an ECS
    layout decision).
- **GPU:**
  - each input column is uploaded into a storage buffer (`writeBuffer`),
    each output is read back (gpu/GpuReadback), or stays on the GPU when
    the next consumer is a GPU pass (grass instances, draw-indirect args);
  - the generated dispatch helper owns the buffers per call site, so a
    per-frame kernel does not reallocate (gpu/GpuLifetime handles retire);
  - **the cost that decides placement:** a 1M-Float upload and readback is
    ~8 MB of traffic. A kernel cheaper than its transfer belongs on the CPU.
    Only kernels whose data already lives on the GPU, or that compute a lot
    per element, are worth dispatching (F7: who decides).

## 5. The remainder at each level

| level | the remainder | today's tool |
|---|---|---|
| scalar | none: the loop runs to `count` | — |
| SIMD | the last `count % 4` (or `% 8`) elements | loadPartial / storePartial / storeMasked + firstLanes (float4-design §9) |
| GPU | the last, partial workgroup | lib/gpu/computeBounds.wgsl `computeElementInRange` + recordBoundedCompute |

The generator emits all three, so a kernel author never writes a tail. Control
flow in the body becomes masks the same way on SIMD and GPU:

- `if c { a } else { b }` computes both sides and selects (`Float4.select` /
  WGSL `select`);
- a side with a write writes under the mask (`storeMasked` / an `if` in
  WGSL, which is free there).

This is ISPC's and Halide's lowering, and the reason the subset forbids side
effects other than the element's own writes.

## 6. Bit-exactness

Rae's CPU rule is that every lowering gives the scalar definition's bits:
-ffp-contract=off, no FMA, compare-and-select min/max (float4-design §4).
The GPU cannot fully promise that:

- **WGSL arithmetic:** WGSL gives f32 `+ - * /` correctly rounded only as
  "acceptable precision". `/` may be up to 2.5 ULP; `sqrt`, `exp`, `sin`
  are implementation-defined within error bounds. Denormals may flush to
  zero. FMA contraction is allowed unless a shader avoids it, and WGSL has
  no `-ffp-contract=off`.
- **What Rae can do:**
  - require bit-exactness only CPU-side (scalar = SIMD, as today);
  - state a tolerance contract for the GPU form (F8): results within N ULP
    of scalar, with no denormal guarantees;
  - test it as fixture 1092 does: run on the GPU, read back, and compare
    against the scalar form within the contract;
  - keep integer and bitwise kernels exact on every form, since WGSL i32/u32
    arithmetic is exact (wrapping).
- **Hand-written WGSL keeps its own rules:** shaders that only render (no
  readback) need no contract at all.

So "one source" means one SEMANTICS, exact on CPU and within a stated bound on
GPU — the same promise CUDA, Slang and Halide make.

## 7. Interplay with hand-written WGSL and shader(files:)

- **Generated WGSL is another composition part:** `rae build` writes it as
  a generated file, and it is composed and naga-validated like any part. A
  kernel can then be combined with hand-written helpers (the noise and
  biome functions):
  `shader(files: ["lib/gpu/computeBounds.wgsl", "lib/noise.wgsl", kernelPart(spread)])`.
  How a kernel names its part is F3. The `// compose-after:` header rule
  (shader composer, 0.1.267) already lets a generated part require the
  bounds file.
- **Calling hand-written WGSL from a kernel:** the reverse direction. A
  kernel would declare an "external WGSL function" signature that only the
  GPU form can call. That breaks the scalar/SIMD forms unless the function
  also has a Rae body. Proposed: allowed only with both, i.e. the WGSL
  function is the GPU implementation of a Rae function, checked by a
  fixture. It's the same pattern as the runtime C rule (a fast platform
  version, a Rae reference).
- **Not replaced:** the renderer's vertex/fragment shaders. Kernels are
  compute only.

## 8. How others do it

| system | model | what Rae can take |
|---|---|---|
| CUDA C++ | `__global__`/`__device__` functions in C++; one compiler, separate host/device code | the split into host-side dispatch plus a device body; Rae avoids the annotations |
| ISPC | C-like language, "programs" run in SIMD gangs; `uniform` vs `varying` types; masks for control flow | the SIMD lowering of §5: varying = per element, uniform = scalar parameter |
| Halide | algorithm vs schedule split; the same pipeline vectorized, parallelized or put on the GPU by a schedule | placement as a separate decision (F7), not part of the kernel |
| Mojo | SIMD types in the language, `vectorize` over a width, GPU kernels from one language | a width parameter instead of hand-written Float4/Float8 bodies |
| Bend 2 | one functional source compiled to C (CPU) and CUDA (GPU) | the prompt: one source, several targets, the same language |
| rust-gpu | a Rust subset compiled to SPIR-V; `#![no_std]`, no heap | the subset rule: no allocator, no recursion, enforced by the compiler |
| Slang | a shading language with modules and generics, compiled to HLSL / SPIR-V / Metal / WGSL / CUDA / C++ | generating WGSL (not SPIR-V) through one IR, and a CPU target for testing |
| fearless_simd | Rust SIMD with levels chosen per platform, masked tails | the width-per-level idea and the tail (§5) |

**Rae's distinctive advantage:** whole-program compilation and no function
pointers. The compiler sees every kernel and every call. The may-wait
analysis already does that kind of transitive check.

## 9. Costs

- **A WGSL emitter for the subset:** expressions, locals, `if`/`match`,
  bounded loops, struct values, inlined calls. It reuses sema's types. A
  rough size is the twin codegen (c_twin.c, ~800 lines) plus WGSL layout
  rules (struct alignment in uniforms, f32 only).
- **A SIMD widener:** the Float4 body from the scalar one (per-op mapping
  plus masks). Plausibly the larger piece: every Rae operation needs its
  lane-wise form, and `match` and loops need masking.
- **Dispatch helpers:** buffers per call site, upload, readback. They are
  generated, over gpu/GpuCompute.
- **Testing:** every kernel fixture runs all forms and compares them,
  exactly on CPU and within a bound on GPU. Needs a GPU in CI, as today's
  1092 does.
- **Language surface:** at least one new keyword or rule (F1), and the subset
  errors.

## 10. Design forks (for the maintainer)

1. **F1 Marking:** a `kernel` modifier on `func`, or unmarked functions
   checked where they are dispatched. Recommended: the keyword. The subset
   is a contract a reader should see at the declaration.
2. **F2 Which forms first:** scalar + GPU (Rae → WGSL), or scalar + SIMD
   (widening). Recommended: GPU first. The SIMD forms already exist by hand
   and are bit-checked, and the GPU side has the larger copy-drift problem.
3. **F3 How a kernel's WGSL is named in a composition:** a compiler-known
   form like `shader(kernel: spread)`, or a generated file path. Recommended:
   `shader(kernels: [spread], files: [...])`, so a composition stays one
   declared list.
4. **F4 Loops:** only constant-bounded loops, or bounded by a scalar
   parameter with divergence accepted. Recommended: parameter-bounded. The
   grass and noise octave loops need it.
5. **F5 AoS columns on SIMD:** transpose per dispatch, or require SoA
   storage for kernel inputs. Recommended: transpose first, measured; an ECS
   SoA layout is its own decision.
6. **F6 Reductions and scatter:** only element-wise maps first, or also sums
   / min / max (a workgroup reduction on GPU, horizontal on SIMD) and
   atomic appends (grass's draw-indirect counter). Recommended: maps, then
   reductions; atomics last.
7. **F7 Placement:** the call site picks (`dispatch spread on gpu`, a
   function argument, not syntax), or the runtime picks by size. Halide's
   lesson is that this is a schedule, separate from the algorithm.
   Recommended: the call site picks, explicitly. Data transfer costs make
   automatic placement wrong as often as right.
8. **F8 GPU float contract:** exact bits required (then refuse
   transcendental and division kernels on the GPU), or a stated ULP bound
   with fixtures comparing within it. Recommended: the bound, documented per
   operation, integer kernels exact.
9. **F9 Width-generic SIMD:** generate Float4 and Float8 bodies from the
   kernel (the width becomes the compiler's, not the author's), which also
   settles the `FloatNative` question of float4-design §10. Recommended:
   yes, for kernels only. The hand-written Float4/Float8 API stays explicit.
10. **F10 Do nothing:** keep three hand-written forms, with fixtures that run
    the scalar and GPU forms on the same inputs and compare them, which
    catches drift without a compiler. This is the cheapest honest option;
    worth doing for grass and water regardless of the other forks.

**Recommended path if approved:**

1. F10's comparison fixtures for the existing pairs.
2. A Rae → WGSL generator for element-wise map kernels (F1 keyword, F3
   composition form, F7 explicit placement, F8 bound).
3. One real kernel converted, the grass placement field or a particle
   update, and measured against the hand-written WGSL.
4. Only then SIMD widening (F9) and reductions (F6).
