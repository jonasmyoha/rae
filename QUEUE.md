# Queue

- [x] Fix VM stack overflow crash in Advanced Pong (vm.c, vm.h)
- [x] Fix "unknown opcode" crash during hot-reload exit (vm_patch.c)
- [x] Fix C backend struct ordering and function signature resolution (c_backend.c)
- [x] Implement implicit project imports (main.c)
- [x] **Phase 1: Buffer Infrastructure**
    - [x] Add `VAL_BUFFER` to `vm_value.h` and implement its lifecycle (alloc/free).
    - [x] Add `OP_BUF_ALLOC`, `OP_BUF_FREE`, `OP_BUF_GET`, `OP_BUF_SET`, `OP_BUF_COPY` opcodes to VM.
    - [x] Expose these as `priv` built-in functions in the compiler.
    - [x] Implement C-backend mapping for raw buffers.
- [x] **Phase 2: List2 (Non-Generic Prototype)**
    - [x] Implement `List2Int` in Rae using `Buffer`.
    - [x] Implement `add`, `get`, `length`, and `remove` (with shifting) in Rae.
    - [x] Create benchmark comparing `List` (C) vs `List2Int` (Rae).
- [ ] **Phase 3: Heterogeneous Lists (Any Type)**