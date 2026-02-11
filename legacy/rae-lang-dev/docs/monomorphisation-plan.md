# Monomorphisation Plan

## Goal

Make generics fully monomorphised for both the Compiled C backend and the Live bytecode VM. Eliminate `RaeAny` erasure for generic types like `List(T)` and `Map(K, V)`.

## Current Audit (Intent)

- **C Backend:** Currently uses a "hybrid erasure" where most Rae generics are forced to `Any_` to share C structs, while `Buffer` is specialized. Redefinition errors occur because the registration and emission loops aren't perfectly synced.
- **VM Backend:** Mostly relies on `RaeAny` for generic elements. This is slow and prevents the VM from being a true "mirror" of the compiled performance.
- **Pipeline:** `Parse -> Typecheck -> [MISSING: Monomorphisation Pass] -> Codegen`.

## Tasks

### 1. Audit & Analysis

- [ ] Audit current implementation: Find where generics are currently “erased” or routed through `RaeAny`.
- [ ] Identify hardcoded checks for "List"/"Buffer"/"Any" in backends.
- [ ] Document current specialization/mangling mismatches.

### 2. Unified Specialization Pipeline (Middle-End)

- [ ] Introduce a `src/specializer.c` pass that runs after typechecking.
- [ ] Output a graph of "Concrete Instantiations".
- [ ] Ensure BOTH C and VM backends consume the same `ConcreteType` and `ConcreteFunc` nodes. No more name-checking strings in the backend.

### 3. VM Changes (Bytecode)

- [ ] Concrete Metadata: Bytecode type headers must store the actual size/layout of `List(Int)` (packed 8-byte) vs `List(Vec3)` (packed 24-byte).
- [ ] Opcode Updates: `OP_GET_ELEMENT` needs to know the stride of the concrete instantiation.
- [ ] Eliminate Boxing: Stop forcing `T` into `RaeAny`. Stack slots should be sized based on the concrete type.

### 4. C Backend Changes

- [ ] Remove "List" / "Buffer" hardcoding: Rely entirely on the `Specializer` output.
- [ ] Centralized Mangler: Use a single mangler shared with the VM (for native FFI matching).
- [ ] Safe Punning: Keep the `union` punning trick for `opt` types, but remove it for generic containers once they are fully concrete.

### 5. Remove RaeAny from Generics

- [ ] `RaeAny` should be reserved for `opt T` (nullability) and future dynamic features.
- [ ] It must NOT be the backing store for `List(T)`.

## My Opinions on Choices

- **On Monomorphisation vs Erasure:** Monomorphisation is the right choice for Rae. Since we prioritize C-like performance and memory safety (view/mod), knowing the exact layout at runtime/compile-time is essential. Erasure with `RaeAny` was a useful prototype shortcut but is now a bottleneck.
- **On Implementation:** We should avoid "Template Bloat" by only instantiating what is actually called. The `Specializer` should be a demand-driven pass.
- **On `RaeAny`:** We should keep `RaeAny` but rename it to `Box` or `Dynamic` to clarify that it is an explicit choice, not an implicit fallback for generics.

## Sequencing Note

**IMPORTANT:** This phase (Monomorphisation) should only begin AFTER the "Architectural File Splitting" from the LLM Iteration Plan is stabilized.

## PR/Patch Plan

### Phase 1: Context & Mangler (Initial PR)

- [ ] Implement `CompilerContext` to hold type/symbol tables.
- [ ] Move mangling to `src/mangler.c`.
- [ ] *Compiles but behavior is unchanged.*

### Phase 2: The Specializer

- [ ] Implement the "Specializer" pass that identifies all `List(Int)`, `List(String)` etc.
- [ ] Assign unique `TypeId` to each.
- [ ] *Compiles, but backends don't use it yet.*

### Phase 3: Backend Unification

- [ ] Update `c_backend.c` to emit a struct for every unique `TypeId` from the specializer.
- [ ] Update `vm_compiler.c` to emit calls to specialized `FuncId`s.
- [ ] *Verify with `List(Int)` and `List(String)` coexistence.*

## Follow-up

- [ ] Add "no-erasure" assertions: verify that `sizeof(List_Int)` != `sizeof(List_Any)` in the VM.
- [ ] Standard library migration: update `core.rae` to remove `RaeAny` hints.

