# Plan: Full Generics Monomorphisation (VM & C)

## Goal
Make generics fully monomorphised for both the Compiled C backend and the Live bytecode VM. Eliminate `RaeAny` erasure for generic types like `List(T)` and `Map(K, V)`.

## Tasks

### 1. Audit & Analysis
- [ ] Audit current implementation: Find where generics are currently “erased” or routed through `RaeAny`.
- [ ] Identify hardcoded checks for "List"/"Buffer"/"Any" in backends.
- [ ] Document current specialization/mangling mismatches.

### 2. Unified Specialization Pipeline (Middle-End)
- [ ] Introduce a `src/specializer.c` pass that runs after typechecking.
- [ ] Output a graph of "Concrete Instantiations".
- [ ] Ensure BOTH C and VM backends consume the same instantiated graph.

### 3. VM Changes (Bytecode)
- [ ] Concrete Metadata: Store actual size/layout per concrete instantiation.
- [ ] Opcode Updates: `OP_GET_ELEMENT` etc. must use concrete type strides.
- [ ] Eliminate Boxing: Stop forcing `T` into `RaeAny` on stack.

### 4. C Backend Changes
- [ ] Remove "List" / "Buffer" hardcoding: Rely entirely on `Specializer` output.
- [ ] Centralized Mangler: Share a single mangler with the VM.
- [ ] Cleanup `union` punning hacks once types are fully concrete.

### 5. Remove RaeAny from Generics
- [ ] Reserve `RaeAny` for `opt T` and dynamic features only.
- [ ] Rewrite standard library `List(T)` etc. to be fully monomorphised.

### 6. Verification
- [ ] Add compiler tests asserting identical behavior in both backends for generics.
- [ ] Add "no erasure" assertions (verify distinct layouts).

## Sequencing Note
**IMPORTANT:** This phase (Monomorphisation) should only begin AFTER the "Architectural File Splitting" from the LLM Iteration Plan is stabilized.

## PR/Patch Plan
- [ ] Phase 1: Context & Mangler (Initial PR)
- [ ] Phase 2: The Specializer (Demand-driven pass)
- [ ] Phase 3: Backend Unification (C and VM migration)