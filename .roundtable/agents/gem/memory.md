# Gem's Memory

## Decisions & Insights
- **Live Mode Consensus**: The failure is a "View Leak" caused by enums being treated as references and `OP_SET_LOCAL` conflating initialization with assignment.
- **Refined Design**: Implement "Bytecode Semantic Separation".
  - **Layer 1**: Add `vm_is_value_type` to handle Primitives + Enums + Strings as value-copies in the Live ABI.
  - **Layer 2**: Introduce `OP_INIT_LOCAL` for all declarations and temporaries. This opcode always overwrites the local slot, preventing stale `VAL_REF` interference.
  - **Layer 3**: Ensure standard `let` assignments (`=`) explicitly dereference RHS references to values before storage.
- **Visual Status**: Compiled mode is confirmed fixed; Live mode is blocked by this reference semantics bug.

## Action Items
- Add `OP_INIT_LOCAL` to VM and Emitters.
- Update `vm_compiler.c` to treat enums as value types.
- Verify fix with Mobile UI Live mode and focused slot-reuse test cases.

## Concurrency Design (Round 1)
### Decisions & Insights
- **Synchronous by Default**: All function calls block/wait by default to preserve sequential simplicity and determinism.
- **Explicit Asynchrony**: The `spawn` keyword starts asynchronous execution and returns a `Task(T)` handle. Awaiting is done explicitly via `task.wait()`.
- **Global Thread Pool**: VM and C backend will share a global thread pool instead of spawning raw threads per call.
- **Scoped Data Parallelism**: Introduce `parallel for` to partition loops across the thread pool. Because it blocks the caller, it safely allows passing `view` and `mod` references to outer scopes.
- **Channels**: Introduce `Channel(T)` for thread-safe message passing between long-running threads (e.g. main/render thread and background systems).

### Action Items
- Extend parser to support `parallel for` syntax.
- Extend semantic checker (`sema.c`) to validate rules (no references in `spawn` or `Channel` data).
- Update VM to implement a thread pool and task runner instead of raw threads, returning a `VAL_TASK`.
- Implement `Task(T)` and `Channel(T)` in C runtime.

