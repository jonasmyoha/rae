# Monomorphisation Plan: Full Concrete Generics

STOP. We are NOT implementing erased generics. We are NOT routing generic T through RaeAny. We are moving toward FULL MONOMORPHISATION for BOTH backends (C backend and Bytecode VM).

## Architecture Direction

1. **Concrete Types for Generics**
   - `List(Int)` and `List(String)` must become distinct concrete types.
   - Their element types must remain `Int` and `String` respectively.
   - No `RaeAny` boxing for generic parameters.

2. **No RaeAny for Generic T**
   - Do not add `RaeAny`-based `buf_get`/`buf_set` for generics.
   - Do not erase single-letter generic parameters to `RaeAny`.
   - Do not treat "specialized containers" as erased containers.

3. **Byte-Based Buffer Primitives**
   - Buffer runtime primitives must operate on `void*` + `elem_size`.
   - Use `memcpy`.
   - No `RaeAny` in buffer ops.
   - The C backend must emit typed code using concrete types.

4. **Error Resolution**
   - Fix "undeclared function" errors in a way that preserves monomorphised semantics.
   - Do NOT introduce `RaeAny` to make compilation pass.

5. **RaeAny Usage**
   - `RaeAny` remains ONLY for true dynamic features (if any).
   - It must NOT be used for generics.

6. **Unified Goal**
   - Same concrete generic instantiation model in C backend and VM.
   - No erased generics.
   - No `Any`-based element storage in `List(T)`, `Buffer(T)`, etc.

---

## Current State Analysis

### C Backend
- **Representation:** Generics are partially monomorphised via mangled names (e.g., `rae_List_Int_`), but the implementation logic often falls back to "erasure" by treating elements as `RaeAny` or `int64_t` in the runtime primitives.
- **Erasure Points:** 
    - `emit_type_ref_as_c_type` frequently erases single-letter generics to `RaeAny`.
    - `mangle_type_recursive` has logic to force erasure for built-in containers.
    - Runtime primitives (`rae_ext_rae_buf_set/get`) were recently modified to use `RaeAny`.

### Bytecode VM
- **Representation:** The VM currently uses a single `OP_LIST_NEW` and similar opcodes that don't differentiate between `List(Int)` and `List(String)` at the instruction level. Values are stored as `Value` types (which is the VM's equivalent of `RaeAny`).
- **Erasure Points:**
    - The VM registry and native functions use the `Value` union for all generic arguments.
    - Method lookup for generics doesn't currently check the concrete type of `T`.

## Proposed Changes for Monomorphisation

### 1. C Backend
- **Remove Erasure Logic:** Delete code in `c_backend.c` and `mangler.c` that maps `T` to `RaeAny`.
- **Concrete Struct Emission:** Ensure `emit_specialized_struct_def` always emits a struct with the actual concrete type for its `data` pointer (e.g., `Int* data` for `rae_List_Int_`).
- **Typed Method Emission:** Emit specialized methods (`rae_add_rae_List_Int_`, etc.) that take and return concrete types.

### 2. Bytecode VM
- **Type-Aware Opcodes:** (Future) Move toward instructions that understand the layout of the concrete generic.
- **Registry Alignment:** Register native functions that handle raw buffers via `elem_size` rather than `Value` boxing.

### 3. Runtime Library
- **Revert RaeAny Primitives:** Change `rae_ext_rae_buf_get/set` to byte-based operations or remove them in favor of direct C array access in emitted code.
- **Standardize Buffer Ops:** Ensure all buffer operations in `rae_runtime.c` use `void*` and `elem_size`.