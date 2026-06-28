#ifndef VM_H
#define VM_H

#include <time.h>
#include "vm_chunk.h"
#include "vm_registry.h"

#define STACK_MAX 2048

typedef enum {
  OP_CONSTANT = 0x01,
  OP_LOG = 0x02,
  OP_LOG_S = 0x03,
  OP_CALL = 0x04,
  OP_RETURN = 0x05,
  OP_GET_LOCAL = 0x06,
  OP_SET_LOCAL = 0x07,
  OP_ALLOC_LOCAL = 0x08,
  OP_POP = 0x09,
  OP_JUMP = 0x0A,
  OP_JUMP_IF_FALSE = 0x0B,
  OP_ADD = 0x10,
  OP_SUB = 0x11,
  OP_MUL = 0x12,
  OP_DIV = 0x13,
  OP_MOD = 0x14,
  OP_NEG = 0x15,
  OP_LT = 0x16,
  OP_LE = 0x17,
  OP_GT = 0x18,
  OP_GE = 0x19,
  OP_EQ = 0x1A,
  OP_NE = 0x1B,
  OP_NOT = 0x1C,
  OP_NATIVE_CALL = 0x1D,
  OP_GET_FIELD = 0x1E,
  OP_SET_FIELD = 0x1F,
  OP_CONSTRUCT = 0x20,
  OP_SPAWN = 0x21,
  OP_BIND_LOCAL = 0x22,
  OP_BIND_FIELD = 0x23,
  OP_REF_VIEW = 0x24,
  OP_REF_MOD = 0x25,
  OP_VIEW_LOCAL = 0x26,
  OP_MOD_LOCAL = 0x27,
  OP_VIEW_FIELD = 0x28,
  OP_MOD_FIELD = 0x2A,
  OP_SET_LOCAL_FIELD = 0x2B,
  OP_DUP = 0x2C,
  OP_LOAD_REF = 0x2D,
  OP_STORE_REF = 0x2E,
  
  /* Buffer Ops */
  OP_BUF_ALLOC = 0x30,
  OP_BUF_FREE = 0x31,
  OP_BUF_GET = 0x32,
  OP_BUF_SET = 0x33,
  OP_BUF_COPY = 0x34,
  OP_BUF_LEN = 0x35,
  OP_BUF_RESIZE = 0x36,
  /* Push a VAL_REF aliasing `&buf->items[index]`. Used by
     componentMod/componentView (`ret mod/view rae_ext_rae_buf_get(...)`)
     so writes through the resulting borrow land directly in the
     buffer's backing storage. The C backend handles the same form by
     inlining buf_get as `*((T*)((char*)buf + i*sizeof(T)))`, which is
     already an lvalue; the VM previously had no way to express this
     and produced a detached value-copy, silently losing all writes
     (e.g. scene-instance overrides on Sprite.textureKey). */
  OP_BUF_REF = 0x37,

  /* Globals */
  OP_GET_GLOBAL = 0x40,
  OP_SET_GLOBAL = 0x41,
  OP_GET_GLOBAL_INIT_BIT = 0x42,
  OP_SET_GLOBAL_INIT_BIT = 0x43,

  /* Like OP_BIND_LOCAL, but if the popped value is a VAL_REF it is
     dereferenced first. Used by `let x: T = expr` (non-bind, `=`)
     so the new local always owns a value, even when the RHS resolves
     to a view/mod reference (e.g. `view Int` param). The explicit
     `=>` bind path keeps OP_BIND_LOCAL so the slot can intentionally
     hold a VAL_REF. */
  OP_BIND_LOCAL_VALUE = 0x44,

  /* Stage 1 step 5 — scope-exit cleanup for bare owned leaf locals
   * (String / List(T) / Buffer(T) / StringMap(V) / IntMap(V)).
   * Reads the slot, walks one level of VAL_REF if the slot is a
   * mod-ref, calls value_free on the underlying value, then leaves
   * the slot in VAL_NONE. Reentrant-safe: a second OP_DROP_LOCAL
   * on the same slot finds VAL_NONE and no-ops, so a hypothetical
   * future slot-reuse cannot trip a double-free. Distinct from
   * native-call-based drops (used for user-struct cascades) so
   * leaf cleanup stays free of the registry lookup + descriptor
   * indirection. */
  OP_DROP_LOCAL = 0x45,

  /* Task(T) synchronization. Pops a VAL_TASK from the stack, joins its
   * thread (once), and pushes a copy of the captured result. Emitted
   * for `task.get()`. No operands. */
  OP_TASK_GET = 0x46,

  /* Pop the top of stack and value_free it (unlike OP_POP, which just
   * discards the slot). Emitted for a discarded expression-statement
   * result that owns a resource — currently a bare `spawn f()` whose
   * Task must be joined-on-drop rather than leaked. No operands. */
  OP_DROP_TOP = 0x47,
  /* Bitwise ops on Int (Erlang-style word operators: bitand/bitor/bitxor/shl/
     shr binary, bitnot unary). Int-only — see project bitwise design. */
  OP_BITAND = 0x48,
  OP_BITOR  = 0x49,
  OP_BITXOR = 0x4A,
  OP_SHL  = 0x4B,
  OP_SHR  = 0x4C,
  OP_BITNOT = 0x4D
} OpCode;

typedef enum {
  VM_RUNTIME_OK = 0,
  VM_RUNTIME_ERROR,
  VM_RUNTIME_TIMEOUT,
  VM_RUNTIME_RELOAD
} VMResult;

typedef struct {
  struct CodeSegment* segment;
  uint8_t* return_ip;
  Value* slots;
  uint32_t slot_count;
  Value locals[256];
} CallFrame;

typedef struct VM {
  Chunk* chunk;
  uint8_t* ip;
  Value stack[STACK_MAX];
  Value* stack_top;
  CallFrame* call_stack; // Heap allocated
  size_t call_stack_top;
  size_t call_stack_capacity;
  VmRegistry* registry;
  int timeout_seconds;
  time_t start_time;
  
  // Hot-Reload State
  volatile bool reload_requested;
  char pending_reload_path[1024]; // Path of the changed file

  // When non-NULL, the top-frame OP_RETURN moves its result here instead
  // of freeing it. Set by a spawned task's sub-VM so the task can capture
  // the function's return value. NULL in the main VM.
  Value* result_capture;
} VM;

void vm_init(VM* vm);
void vm_free(VM* vm);
VMResult vm_run(VM* vm, Chunk* chunk);
void vm_reset_stack(VM* vm);
void vm_set_registry(VM* vm, VmRegistry* registry);
bool vm_hot_patch(VM* vm, Chunk* new_chunk);

#endif /* VM_H */
