#include "vm.h"


#include <stdio.h>

#include <stdlib.h> // For malloc and free
#include <string.h>
#include <math.h>

#include "diag.h"
#include "vm_registry.h"
#include "vm_value.h" // For Value, value_list, value_list_add
#include "sys_thread.h"

typedef struct {
  Chunk* chunk;
  uint32_t target;
  Value args[256];
  uint8_t arg_count;
  VmRegistry* registry;
} SpawnData;

static void* spawn_thread_wrapper(void* arg) {
  SpawnData* data = (SpawnData*)arg;
  
  VM sub_vm;
  vm_init(&sub_vm);
  sub_vm.registry = data->registry;
  
  // Set up initial frame
  sub_vm.call_stack_top = 1;
  CallFrame* frame = &sub_vm.call_stack[0];
  frame->return_ip = NULL;
  frame->slots = sub_vm.stack;
  frame->slot_count = data->arg_count;
  
  // Note: we reversed them during pop, so we reverse them back during assign or vice versa
  for (int i = 0; i < data->arg_count; i++) {
    frame->locals[i] = data->args[data->arg_count - 1 - i];
  }
  for (int i = data->arg_count; i < 256; i++) {
    frame->locals[i] = value_none();
  }
  
  sub_vm.ip = data->chunk->code + data->target;
  
  vm_run(&sub_vm, data->chunk);
  
  vm_free(&sub_vm);
  free(data);
  return NULL;
}

static Value vm_pop(VM* vm) {
  if (vm->stack_top == vm->stack) {
    diag_error(NULL, 0, 0, "VM stack underflow");
    Value zero = value_int(0);
    return zero;
  }
  vm->stack_top -= 1;
  Value val = *vm->stack_top;
  return val;
}

static void vm_push(VM* vm, Value value) {
  if (vm->stack_top - vm->stack >= STACK_MAX) {
    diag_fatal("VM stack overflow");
  }
  *vm->stack_top++ = value;
}

static Value* vm_peek(VM* vm, size_t distance) {
  return vm->stack_top - 1 - distance;
}

// True when copying this Value is a trivial bitwise copy — no heap
// allocation in value_copy and no recursive free in value_free. Used to
// short-circuit VAL_REF wrapping at view-borrow sites for primitives
// (Int / Float / Bool / Char / None / ID / enum reified as Int): the
// callee never observes a difference between a `view Int` ref and the
// raw Int, but the VM avoids ref-then-deref churn entirely.
//
// Strings, objects, arrays, and buffers are deliberately NOT pod —
// copying them allocates, so we keep the VAL_REF indirection there.
static inline bool value_is_pod(const Value* v) {
  switch (v->type) {
    case VAL_INT:
    case VAL_FLOAT:
    case VAL_BOOL:
    case VAL_CHAR:
    case VAL_NONE:
    case VAL_ID:
      return true;
    default:
      return false;
  }
}

static bool value_is_truthy(const Value* value) {
  switch (value->type) {
    case VAL_BOOL:
      return value->as.bool_value;
    case VAL_INT:
      return value->as.int_value != 0;
    case VAL_FLOAT:
      return value->as.float_value != 0.0;
    case VAL_STRING:
      return value->as.string_value.length > 0;
    case VAL_CHAR:
      return value->as.char_value != 0;
    case VAL_NONE:
      return false;
    case VAL_OBJECT:
    case VAL_ARRAY:
    case VAL_BUFFER:
    case VAL_REF:
    case VAL_ID:
    case VAL_KEY:
      return true;
  }
  return false;
}

static bool values_equal(const Value* lhs, const Value* rhs) {
  if (lhs->type != rhs->type) return false;
  switch (lhs->type) {
    case VAL_BOOL:
      return lhs->as.bool_value == rhs->as.bool_value;
    case VAL_INT:
      return lhs->as.int_value == rhs->as.int_value;
    case VAL_FLOAT:
      return lhs->as.float_value == rhs->as.float_value;
    case VAL_STRING:
      if (lhs->as.string_value.length != rhs->as.string_value.length) return false;
      return memcmp(lhs->as.string_value.chars, rhs->as.string_value.chars,
                    lhs->as.string_value.length) == 0;
    case VAL_CHAR:
      return lhs->as.char_value == rhs->as.char_value;
    case VAL_NONE:
      return true;
    case VAL_OBJECT:
      // Identity equality for objects
      return lhs->as.object_value.fields == rhs->as.object_value.fields;
    case VAL_ARRAY:
      return lhs->as.array_value == rhs->as.array_value;
    case VAL_BUFFER:
      return lhs->as.buffer_value == rhs->as.buffer_value;
    case VAL_REF:
      // References are equal if they point to the same storage
      return lhs->as.ref_value.target == rhs->as.ref_value.target;
    case VAL_ID:
      return lhs->as.id_value == rhs->as.id_value;
    case VAL_KEY:
      if (lhs->as.key_value.length != rhs->as.key_value.length) return false;
      return memcmp(lhs->as.key_value.chars, rhs->as.key_value.chars,
                    lhs->as.key_value.length) == 0;
  }
  return false;
}

void vm_init(VM* vm) {
  if (!vm) return;
  vm->chunk = NULL;
  vm->ip = NULL;
  vm_reset_stack(vm);
  vm->call_stack_capacity = 256;
  vm->call_stack = malloc(sizeof(CallFrame) * vm->call_stack_capacity);
  vm->call_stack_top = 0;
  vm->registry = NULL;
  vm->timeout_seconds = 0;
  vm->start_time = 0;
  vm->reload_requested = false;
}

void vm_free(VM* vm) {
  if (!vm) return;
  if (vm->call_stack) {
    // Clear any remaining values in frames
    for (size_t i = 0; i < vm->call_stack_top; i++) {
      for (int j = 0; j < 256; j++) {
        value_free(&vm->call_stack[i].locals[j]);
      }
    }
    free(vm->call_stack);
  }
}

void vm_reset_stack(VM* vm) {
  if (!vm) return;
  vm->stack_top = vm->stack;
  vm->call_stack_top = 0;
}

void vm_set_registry(VM* vm, VmRegistry* registry) {
  if (!vm) return;
  vm->registry = registry;
}

static uint32_t read_uint32(VM* vm) {
  uint32_t value = ((uint32_t)vm->ip[0] << 24) |
                   ((uint32_t)vm->ip[1] << 16) |
                   ((uint32_t)vm->ip[2] << 8)  |
                   ((uint32_t)vm->ip[3]);
  vm->ip += 4;
  return value;
}

static CallFrame* vm_current_frame(VM* vm) {
  if (vm->call_stack_top == 0) {
    return NULL;
  }
  return &vm->call_stack[vm->call_stack_top - 1];
}

static const char* vm_value_type_name(ValueType type) {
  switch (type) {
    case VAL_INT: return "Int";
    case VAL_FLOAT: return "Float";
    case VAL_BOOL: return "Bool";
    case VAL_STRING: return "String";
    case VAL_CHAR: return "Char";
    case VAL_NONE: return "none";
    case VAL_OBJECT: return "object";
    case VAL_ARRAY: return "array";
    case VAL_BUFFER: return "buffer";
    case VAL_REF: return "ref";
    case VAL_ID: return "id";
    case VAL_KEY: return "key";
  }
  return "unknown";
}

static const char* vm_chunk_debug_name(const Chunk* chunk, size_t offset) {
  if (!chunk || chunk->functions_count == 0) {
    return "<top-level>";
  }

  const char* name = "<top-level>";
  size_t best_offset = 0;
  for (size_t i = 0; i < chunk->functions_count; ++i) {
    FunctionDebugInfo* info = &chunk->functions[i];
    if (info->offset <= offset && info->offset >= best_offset) {
      name = info->name ? info->name : "<function>";
      best_offset = info->offset;
    }
  }
  return name;
}

VMResult vm_run(VM* vm, Chunk* chunk) {
  if (!vm || !chunk) return VM_RUNTIME_ERROR;
  
  bool is_resume = (vm->chunk == chunk && vm->ip >= chunk->code && vm->ip < chunk->code + chunk->code_count);
  vm->chunk = chunk;
  
  if (!is_resume) {
      if (vm->ip == NULL) {
          vm->ip = chunk->code;
      }
      vm->start_time = time(NULL);
      
      // Set up initial frame for module-level execution if not already set up (e.g. by spawn)
      if (vm->call_stack_top == 0) {
          vm->call_stack_top = 1;
          CallFrame* frame = &vm->call_stack[0];
          frame->return_ip = NULL;
          frame->slots = vm->stack;
          frame->slot_count = 0;
          for (int i = 0; i < 256; i++) {
            frame->locals[i] = value_none();
          }
      }
  }

  for (;;) {
    // 1. Check for external signals
    if (vm->reload_requested) {
        return VM_RUNTIME_RELOAD;
    }

    // 2. Check for timeout
    if (vm->timeout_seconds > 0) {
        if (time(NULL) - vm->start_time > vm->timeout_seconds) {
            return VM_RUNTIME_TIMEOUT;
        }
    }

    size_t instruction_offset = (size_t)(vm->ip - chunk->code);
    int instruction_line = instruction_offset < chunk->code_count ? chunk->lines[instruction_offset] : 0;
    uint8_t instruction = *vm->ip++;
    switch (instruction) {
      case OP_CONSTANT: {
        uint32_t index = read_uint32(vm);
        if (index >= chunk->constants_count) {
          diag_fatal("bytecode constant index OOB");
        }
        vm_push(vm, value_copy(&chunk->constants[index]));
        break;
      }
      case OP_LOG:
      case OP_LOG_S: {
        Value value = vm_pop(vm);
        value_print(&value);
        if (instruction == OP_LOG) {
          printf("\n");
        }
        fflush(stdout);
        value_free(&value);
        break;
      }
      case OP_JUMP: {
        uint32_t target = read_uint32(vm);
        vm->ip = vm->chunk->code + target;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint32_t target = read_uint32(vm);
        Value* condition = vm_peek(vm, 0);
        Value resolved = value_copy(condition);
        while (resolved.type == VAL_REF) {
            Value next = value_copy(resolved.as.ref_value.target);
            value_free(&resolved);
            resolved = next;
        }
        if (!value_is_truthy(&resolved)) {
          vm->ip = vm->chunk->code + target;
        }
        value_free(&resolved);
        break;
      }
      case OP_CALL: {
        uint32_t target = read_uint32(vm);
        uint8_t arg_count = *vm->ip++;
        if (vm->stack_top - vm->stack < arg_count) {
          diag_error(NULL, 0, 0, "not enough arguments on stack for call");
          return VM_RUNTIME_ERROR;
        }
        if (vm->call_stack_top >= vm->call_stack_capacity) {
          diag_error(NULL, 0, 0, "call stack overflow");
          return VM_RUNTIME_ERROR;
        }
        CallFrame* frame = &vm->call_stack[vm->call_stack_top++];
        frame->return_ip = vm->ip;
        frame->slots = vm->stack_top - arg_count;
        frame->slot_count = arg_count;
        
        // Transfer arguments to stable locals storage (caller pops, callee owns)
        for (int i = 0; i < arg_count; i++) {
          frame->locals[i] = frame->slots[i];
          frame->slots[i] = value_none();
        }
        
        // Remaining locals will be initialized by OP_ALLOC_LOCAL if needed.
        // But for safety, we should ensure they are at least VAL_NONE if accessed before alloc.
        // Actually, let's just zero out up to 256 only if we really need to, or rely on ALLOC.
        // For now, let's just initialize the rest to NONE to avoid garbage value_free.
        for (int i = arg_count; i < 256; i++) {
          frame->locals[i].type = VAL_NONE;
        }
        
        if (target >= vm->chunk->code_count) {
          diag_error(NULL, 0, 0, "invalid function address");
          return VM_RUNTIME_ERROR;
        }
        vm->ip = vm->chunk->code + target;
        break;
      }
      case OP_SPAWN: {
        uint32_t target = read_uint32(vm);
        uint8_t arg_count = *vm->ip++;
        
        if (vm->stack_top - vm->stack < arg_count) {
          diag_error(NULL, 0, 0, "not enough arguments on stack for spawn");
          return VM_RUNTIME_ERROR;
        }
        
        SpawnData* data = malloc(sizeof(SpawnData));
        data->chunk = vm->chunk;
        data->target = target;
        data->arg_count = arg_count;
        data->registry = vm->registry;
        
        // Transfer arguments (pop from current VM, transfer to SpawnData)
        for (int i = 0; i < arg_count; i++) {
          data->args[i] = vm_pop(vm);
        }
        
        sys_thread_t thread;
        if (!sys_thread_create(&thread, spawn_thread_wrapper, data)) {
          diag_error(NULL, 0, 0, "failed to spawn thread");
          // cleanup
          for (int i = 0; i < arg_count; i++) value_free(&data->args[i]);
          free(data);
          return VM_RUNTIME_ERROR;
        }
        // We don't join here, it runs detached (or we'll manage it later)
        break;
      }
      case OP_NATIVE_CALL: {
        uint32_t const_index = read_uint32(vm);
        uint8_t arg_count = *vm->ip++;
        if (!vm->registry) {
          diag_error(NULL, 0, 0, "native call attempted without registry");
          return VM_RUNTIME_ERROR;
        }
        if (const_index >= chunk->constants_count) {
          diag_error(NULL, 0, 0, "native symbol index OOB");
          return VM_RUNTIME_ERROR;
        }
        Value symbol = chunk->constants[const_index];
        if (symbol.type != VAL_STRING || !symbol.as.string_value.chars) {
          diag_error(NULL, 0, 0, "native symbol constant must be string");
          return VM_RUNTIME_ERROR;
        }
        if ((size_t)(vm->stack_top - vm->stack) < arg_count) {
          diag_error(NULL, 0, 0, "not enough arguments on stack for native call");
          return VM_RUNTIME_ERROR;
        }
        const VmNativeEntry* entry = vm_registry_find_native(vm->registry, symbol.as.string_value.chars);
        if (!entry || !entry->callback) {
          char buffer[128];
          snprintf(buffer, sizeof(buffer), "native function not registered: %s", symbol.as.string_value.chars);
          diag_error(NULL, 0, 0, buffer);
          return VM_RUNTIME_ERROR;
        }
        
        // Dereference arguments if needed
        for (uint8_t i = 0; i < arg_count; i++) {
            Value* arg = vm->stack_top - arg_count + i;
            while (arg->type == VAL_REF) {
                Value next = value_copy(arg->as.ref_value.target);
                value_free(arg);
                *arg = next;
            }
        }
        
        const Value* args = vm->stack_top - arg_count;
        
        VmNativeResult result = {.has_value = false};
        if (!entry->callback(vm, &result, args, arg_count, entry->user_data)) {
          char buffer[160];
          snprintf(buffer, sizeof(buffer), "native function reported failure: %s", symbol.as.string_value.chars);
          diag_error(NULL, 0, 0, buffer);
          return VM_RUNTIME_ERROR;
        }
        
        // Clean up arguments
        for (uint8_t i = 0; i < arg_count; i++) {
            value_free(vm->stack_top - arg_count + i);
        }
        vm->stack_top -= arg_count;
        
        if (result.has_value) {
          vm_push(vm, result.value);
        } else {
          vm_push(vm, value_none());
        }
        break;
      }
      case OP_GET_LOCAL: {
        uint32_t slot = read_uint32(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local access outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (slot >= 256) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, value_copy(&frame->locals[slot]));
        break;
      }
      case OP_SET_LOCAL: {
        uint32_t slot = read_uint32(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local access outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (slot >= 256) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        Value val = vm_pop(vm);
        if (frame->locals[slot].type == VAL_REF) {
          if (frame->locals[slot].as.ref_value.kind == REF_VIEW) {
            char buffer[192];
            snprintf(buffer, sizeof(buffer),
                     "cannot assign to a read-only 'view' reference (chunk %s, bytecode offset %zu)",
                     vm_chunk_debug_name(chunk, instruction_offset), instruction_offset);
            diag_error(NULL, instruction_line, 0, buffer);
            value_free(&val);
            return VM_RUNTIME_ERROR;
          }
          value_free(frame->locals[slot].as.ref_value.target);
          *frame->locals[slot].as.ref_value.target = value_copy(&val);
        } else {
          value_free(&frame->locals[slot]);
          frame->locals[slot] = value_copy(&val);
        }
        vm_push(vm, val); // Result of assignment
        break;
      }
      case OP_BIND_LOCAL: {
        uint32_t slot = read_uint32(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local access outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (slot >= 256) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        Value value = vm_pop(vm);
        value_free(&frame->locals[slot]);
        frame->locals[slot] = value;
        break;
      }
      case OP_BIND_LOCAL_VALUE: {
        // Like OP_BIND_LOCAL, but derefs VAL_REF so the new local owns a
        // value instead of aliasing the source. Used for `let x: T = expr`
        // when the let does NOT use the `=>` bind syntax — so the new
        // local must be a fresh owning value even if the RHS happens to
        // be a `view T` / `mod T` reference (e.g. a `view Int` param).
        // The explicit `=>` bind path keeps OP_BIND_LOCAL.
        uint32_t slot = read_uint32(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local access outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (slot >= 256) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        Value value = vm_pop(vm);
        if (value.type == VAL_REF) {
          // Deep-copy the underlying target so the new local owns its
          // bytes. Then free the ref wrapper (no-op for VAL_REF in
          // value_free, but kept for symmetry / future-proofing).
          Value resolved = value_copy(value.as.ref_value.target);
          value_free(&value);
          value = resolved;
        }
        value_free(&frame->locals[slot]);
        frame->locals[slot] = value;
        break;
      }
      case OP_DROP_LOCAL: {
        /* Stage 1 step 5 — scope-exit cleanup for bare leaf locals.
         * The slot may hold the value directly (typical) or a
         * mod-ref into another frame's slot (rare — passed through
         * `mod` parameter chains). View refs are non-owning so we
         * never drop through them. After the free, the slot is
         * VAL_NONE; a second OP_DROP_LOCAL on the same slot is a
         * safe no-op. */
        uint32_t slot = read_uint32(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local access outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (slot >= 256) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        Value* target = &frame->locals[slot];
        if (target->type == VAL_REF) {
          if (target->as.ref_value.kind == REF_MOD
              && target->as.ref_value.target) {
            value_free(target->as.ref_value.target);
          }
          /* The ref itself is non-owning; just clear the slot. */
          target->type = VAL_NONE;
        } else {
          value_free(target);
        }
        break;
      }
      case OP_ALLOC_LOCAL: {
        uint32_t required = read_uint32(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local allocation outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (required > 256) {
          diag_error(NULL, 0, 0, "VM local storage overflow (max 256)");
          return VM_RUNTIME_ERROR;
        }
        if (required > frame->slot_count) {
          frame->slot_count = required;
        }
        break;
      }
      
      case OP_GET_GLOBAL: {
        uint32_t index = read_uint32(vm);
        if (!vm->registry || index >= vm->registry->global_count) {
          diag_error(NULL, 0, 0, "global index OOB");
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, value_copy(&vm->registry->globals[index]));
        break;
      }
      
      case OP_SET_GLOBAL: {
        uint32_t index = read_uint32(vm);
        if (!vm->registry || index >= vm->registry->global_count) {
          diag_error(NULL, 0, 0, "global index OOB");
          return VM_RUNTIME_ERROR;
        }
        Value val = vm_pop(vm);
        value_free(&vm->registry->globals[index]);
        vm->registry->globals[index] = value_copy(&val);
        vm_push(vm, val);
        break;
      }
      
      case OP_GET_GLOBAL_INIT_BIT: {
        uint32_t index = read_uint32(vm);
        if (!vm->registry || index >= vm->registry->global_count) {
          diag_error(NULL, 0, 0, "global index OOB");
          return VM_RUNTIME_ERROR;
        }
        bool is_init = (vm->registry->global_init_bits[index] != 0);
        vm_push(vm, value_bool(is_init));
        break;
      }
      
      case OP_SET_GLOBAL_INIT_BIT: {
        uint32_t index = read_uint32(vm);
        if (!vm->registry || index >= vm->registry->global_count) {
          diag_error(NULL, 0, 0, "global index OOB");
          return VM_RUNTIME_ERROR;
        }
        vm->registry->global_init_bits[index] = 1;
        break;
      }
      case OP_POP: {
        vm_pop(vm);
        break;
      }
      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV:
      case OP_MOD: {
        Value rhs = vm_pop(vm);
        Value lhs = vm_pop(vm);
        
        while (lhs.type == VAL_REF) {
            Value next = value_copy(lhs.as.ref_value.target);
            value_free(&lhs);
            lhs = next;
        }
        while (rhs.type == VAL_REF) {
            Value next = value_copy(rhs.as.ref_value.target);
            value_free(&rhs);
            rhs = next;
        }

        if ((lhs.type == VAL_INT || lhs.type == VAL_FLOAT) && (rhs.type == VAL_INT || rhs.type == VAL_FLOAT)) {
            if (lhs.type == VAL_FLOAT || rhs.type == VAL_FLOAT) {
                double l = (lhs.type == VAL_FLOAT) ? lhs.as.float_value : (double)lhs.as.int_value;
                double r = (rhs.type == VAL_FLOAT) ? rhs.as.float_value : (double)rhs.as.int_value;
                double res = 0;
                switch (instruction) {
                    case OP_ADD: res = l + r; break;
                    case OP_SUB: res = l - r; break;
                    case OP_MUL: res = l * r; break;
                    case OP_DIV: res = l / r; break;
                    case OP_MOD: res = fmod(l, r); break;
                }
                vm_push(vm, value_float(res));
            } else {
                int64_t l = lhs.as.int_value;
                int64_t r = rhs.as.int_value;
                int64_t res = 0;
                switch (instruction) {
                    case OP_ADD: res = l + r; break;
                    case OP_SUB: res = l - r; break;
                    case OP_MUL: res = l * r; break;
                    case OP_DIV: 
                        if (r == 0) { diag_error(NULL, 0, 0, "division by zero"); return VM_RUNTIME_ERROR; }
                        res = l / r; break;
                    case OP_MOD:
                        if (r == 0) { diag_error(NULL, 0, 0, "modulo by zero"); return VM_RUNTIME_ERROR; }
                        res = l % r; break;
                }
                vm_push(vm, value_int(res));
            }
        } else {
          char buffer[160];
          snprintf(buffer, sizeof(buffer), "arithmetic operands must be numbers, got %s and %s",
                   vm_value_type_name(lhs.type), vm_value_type_name(rhs.type));
          diag_error(NULL, instruction_line, 0, buffer);
          return VM_RUNTIME_ERROR;
        }
        break;
      }
      case OP_NEG: {
        Value operand = vm_pop(vm);
        while (operand.type == VAL_REF) {
            Value next = value_copy(operand.as.ref_value.target);
            value_free(&operand);
            operand = next;
        }
        if (operand.type == VAL_INT) {
          vm_push(vm, value_int(-operand.as.int_value));
        } else if (operand.type == VAL_FLOAT) {
          vm_push(vm, value_float(-operand.as.float_value));
        } else {
          diag_error(NULL, 0, 0, "negation expects numeric operand");
          value_free(&operand);
          return VM_RUNTIME_ERROR;
        }
        value_free(&operand);
        break;
      }
      case OP_LT:
      case OP_LE:
      case OP_GT:
      case OP_GE: {
        Value rhs = vm_pop(vm);
        Value lhs = vm_pop(vm);

        while (lhs.type == VAL_REF) {
            Value next = value_copy(lhs.as.ref_value.target);
            value_free(&lhs);
            lhs = next;
        }
        while (rhs.type == VAL_REF) {
            Value next = value_copy(rhs.as.ref_value.target);
            value_free(&rhs);
            rhs = next;
        }

        if ((lhs.type == VAL_INT || lhs.type == VAL_FLOAT) && (rhs.type == VAL_INT || rhs.type == VAL_FLOAT)) {
          double l = (lhs.type == VAL_FLOAT) ? lhs.as.float_value : (double)lhs.as.int_value;
          double r = (rhs.type == VAL_FLOAT) ? rhs.as.float_value : (double)rhs.as.int_value;
          bool result = false;
          switch (instruction) {
            case OP_LT: result = l < r; break;
            case OP_LE: result = l <= r; break;
            case OP_GT: result = l > r; break;
            case OP_GE: result = l >= r; break;
          }
          vm_push(vm, value_bool(result));
        } else {
          fprintf(stderr, "error: comparison expects numbers, got: ");
          value_print(&lhs);
          fprintf(stderr, " and ");
          value_print(&rhs);
          fprintf(stderr, "\n");
          diag_error(NULL, 0, 0, "comparison operands must be numbers");
          return VM_RUNTIME_ERROR;
        }
        break;
      }
      case OP_EQ:
      case OP_NE: {
        Value rhs = vm_pop(vm);
        Value lhs = vm_pop(vm);

        Value* l = &lhs;
        if (lhs.type == VAL_REF) l = lhs.as.ref_value.target;
        Value* r = &rhs;
        if (rhs.type == VAL_REF) r = rhs.as.ref_value.target;

        bool equal = values_equal(l, r);
        if (instruction == OP_NE) {
          equal = !equal;
        }
        vm_push(vm, value_bool(equal));
        value_free(&lhs);
        value_free(&rhs);
        break;
      }
      case OP_NOT: {
        Value operand = vm_pop(vm);
        while (operand.type == VAL_REF) {
            Value next = value_copy(operand.as.ref_value.target);
            value_free(&operand);
            operand = next;
        }
        vm_push(vm, value_bool(!value_is_truthy(&operand)));
        value_free(&operand);
        break;
      }
      case OP_GET_FIELD: {
        uint32_t field_index = read_uint32(vm);
        Value obj_val = vm_pop(vm);
        Value* target = &obj_val;
        
        while (target->type == VAL_REF) {
          target = target->as.ref_value.target;
        }
        
        if (target->type == VAL_NONE) {
          vm_push(vm, value_none());
        } else if (target->type != VAL_OBJECT) {
          char buf[128];
          snprintf(buf, sizeof(buf), "GET_FIELD on non-object (got %s)", vm_value_type_name(target->type));
          diag_error(NULL, instruction_line, 0, buf);
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        } else if (field_index >= target->as.object_value.field_count) {
          diag_error(NULL, 0, 0, "GET_FIELD index OOB");
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        } else {
          vm_push(vm, value_copy(&target->as.object_value.fields[field_index]));
        }
        
        value_free(&obj_val);
        break;
      }
      case OP_SET_FIELD: {
        uint32_t index = read_uint32(vm);
        Value val = vm_pop(vm);
        Value obj_val = vm_pop(vm);
        Value* target = &obj_val;
        if (obj_val.type == VAL_REF) {
          if (obj_val.as.ref_value.kind == REF_VIEW) {
            diag_error(NULL, 0, 0, "cannot assign to field of a read-only 'view' reference");
            value_free(&obj_val);
            value_free(&val);
            return VM_RUNTIME_ERROR;
          }
          target = obj_val.as.ref_value.target;
        }
        
        if (target->type != VAL_OBJECT) {
          diag_error(NULL, 0, 0, "SET_FIELD on non-object");
          value_free(&val);
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        }
        if (index >= target->as.object_value.field_count) {
          diag_error(NULL, 0, 0, "SET_FIELD index out of range");
          value_free(&val);
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        }
        value_free(&target->as.object_value.fields[index]);
        target->as.object_value.fields[index] = value_copy(&val);
        vm_push(vm, value_copy(&val)); // Return result of assignment
        value_free(&val);
        value_free(&obj_val);
        break;
      }
      case OP_SET_LOCAL_FIELD: {
        uint32_t slot = read_uint32(vm);
        uint32_t index = read_uint32(vm);
        Value val = vm_pop(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) return VM_RUNTIME_ERROR;
        
        Value* target = &frame->locals[slot];
        if (target->type == VAL_REF) {
          target = target->as.ref_value.target;
        }
        
        if (target->type != VAL_OBJECT) {
          diag_error(NULL, 0, 0, "SET_LOCAL_FIELD on non-object");
          value_free(&val);
          return VM_RUNTIME_ERROR;
        }
        
        if (index >= target->as.object_value.field_count) {
          diag_error(NULL, 0, 0, "SET_LOCAL_FIELD index out of range");
          value_free(&val);
          return VM_RUNTIME_ERROR;
        }
        
        value_free(&target->as.object_value.fields[index]);
        target->as.object_value.fields[index] = value_copy(&val);
        vm_push(vm, value_copy(&val)); // Return result
        value_free(&val);
        break;
      }
      case OP_BIND_FIELD: {
        uint32_t index = read_uint32(vm);
        Value val = vm_pop(vm); // The reference to bind
        Value obj_val = vm_pop(vm); // Target object
        Value* target = &obj_val;
        
        if (target->type == VAL_REF) {
          target = target->as.ref_value.target;
        }
        
        if (target->type != VAL_OBJECT) {
          diag_error(NULL, 0, 0, "BIND_FIELD on non-object");
          value_free(&val);
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        }
        if (index >= target->as.object_value.field_count) {
          diag_error(NULL, 0, 0, "BIND_FIELD index out of range");
          value_free(&val);
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        }
        
        value_free(&target->as.object_value.fields[index]);
        target->as.object_value.fields[index] = value_copy(&val);
        vm_push(vm, value_copy(&val)); // Return result
        value_free(&val);
        value_free(&obj_val);
        break;
      }
      case OP_REF_VIEW: {
        Value* target = vm_peek(vm, 0);
        if (target->type == VAL_NONE) {
          // Already none, so opt view T => none is none
        } else {
          // Dangerous: points to stack. Handled by compiler only for safe cases.
          *target = value_ref(target, REF_VIEW);
        }
        break;
      }
      case OP_REF_MOD: {
        Value* target = vm_peek(vm, 0);
        if (target->type == VAL_NONE) {
          // Already none
        } else {
          *target = value_ref(target, REF_MOD);
        }
        break;
      }
      case OP_VIEW_LOCAL:
      case OP_MOD_LOCAL: {
        uint32_t slot = read_uint32(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) return VM_RUNTIME_ERROR;
        if (slot >= 256) return VM_RUNTIME_ERROR;
        Value* slot_val = &frame->locals[slot];

        // Hot-path optimisation: view-of-primitive where the local slot
        // directly holds a primitive (NOT a VAL_REF chain ending in
        // one) pushes a by-value copy instead of allocating a VAL_REF
        // wrapper. The callee can't tell the difference (view is
        // read-only and the bytes are identical), and we avoid the
        // per-call ref-then-deref churn that was dominating Live-mode
        // CPU in the mobile UI hot loop.
        //
        // Constraints:
        //  - Only for OP_VIEW_LOCAL — OP_MOD_LOCAL must keep VAL_REF so
        //    writes flow back to the caller slot.
        //  - Only when the slot directly contains a pod. If the slot
        //    holds a VAL_REF that happens to point at a pod, that's a
        //    deliberate aliasing path (e.g. the let-bind backing-slot
        //    trick in vm_emit_stmt.c that supports
        //    `let r: view T => fn()`) and collapsing the chain would
        //    silently degrade view-bind to value-bind. Test
        //    336_return_view_ref_alias guards this.
        if (instruction == OP_VIEW_LOCAL && value_is_pod(slot_val)) {
          vm_push(vm, *slot_val);
          break;
        }

        Value* target = slot_val;
        // Resolve pointer chain: if slot already holds a reference, point to its target.
        while (target->type == VAL_REF) {
          target = target->as.ref_value.target;
        }

        if (target->type == VAL_NONE) {
          vm_push(vm, value_none());
        } else {
          vm_push(vm, value_ref(target, instruction == OP_MOD_LOCAL ? REF_MOD : REF_VIEW));
        }
        break;
      }
      case OP_VIEW_FIELD:
      case OP_MOD_FIELD: {
        uint32_t index = read_uint32(vm);
        Value obj_val = vm_pop(vm);

        if (obj_val.type != VAL_REF) {
            // Cannot take reference to a temporary (literal or result of expr)
            value_free(&obj_val);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "cannot take reference to a temporary value (chunk %s, op %s, bytecode offset %zu)",
                vm_chunk_debug_name(chunk, instruction_offset),
                instruction == OP_VIEW_FIELD ? "OP_VIEW_FIELD" : "OP_MOD_FIELD",
                instruction_offset);
            diag_error(NULL, instruction_line, 0, buf);
            return VM_RUNTIME_ERROR;
        }

        Value* target_obj = obj_val.as.ref_value.target;
        while (target_obj->type == VAL_REF) {
            target_obj = target_obj->as.ref_value.target;
        }

        if (target_obj->type != VAL_OBJECT || index >= target_obj->as.object_value.field_count) {
            value_free(&obj_val);
            return VM_RUNTIME_ERROR;
        }

        Value* field_target = &target_obj->as.object_value.fields[index];
        vm_push(vm, value_ref(field_target, instruction == OP_MOD_FIELD ? REF_MOD : REF_VIEW));

        if (obj_val.type != VAL_REF) {
            value_free(&obj_val);
        }
        break;
      }
      case OP_DUP: {
        vm_push(vm, value_copy(vm_peek(vm, 0)));
        break;
      }
      case OP_LOAD_REF: {
        Value ref = vm_pop(vm);
        if (ref.type != VAL_REF) {
          diag_error(NULL, 0, 0, "OP_LOAD_REF on non-reference");
          value_free(&ref);
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, value_copy(ref.as.ref_value.target));
        value_free(&ref);
        break;
      }
      case OP_STORE_REF: {
        Value val = vm_pop(vm);
        Value ref = vm_pop(vm);
        if (ref.type != VAL_REF) {
          diag_error(NULL, 0, 0, "OP_STORE_REF on non-reference");
          value_free(&val);
          value_free(&ref);
          return VM_RUNTIME_ERROR;
        }
        if (ref.as.ref_value.kind == REF_VIEW) {
          diag_error(NULL, 0, 0, "cannot store through a read-only 'view' reference");
          value_free(&val);
          value_free(&ref);
          return VM_RUNTIME_ERROR;
        }
        value_free(ref.as.ref_value.target);
        *ref.as.ref_value.target = value_copy(&val);
        vm_push(vm, value_copy(&val)); // Return result of assignment
        value_free(&val);
        value_free(&ref);
        break;
      }
      case OP_CONSTRUCT: {
        uint32_t field_count = read_uint32(vm);
        uint32_t type_name_index = read_uint32(vm);
        const char* type_name = NULL;
        if (type_name_index != 0xFFFFFFFF) {
            type_name = vm->chunk->constants[type_name_index].as.string_value.chars;
        }
        Value obj = value_object(field_count, type_name);
        for (int i = (int)field_count - 1; i >= 0; --i) {
          Value field = vm_pop(vm);
          // A struct literal stores VALUES in its fields, never references —
          // otherwise refs to the constructor's caller-local arguments
          // outlive the caller's frame and dangle. Deref any VAL_REF here
          // so e.g. `Shape { fill: fill }` where `fill` is a view param
          // stores a self-contained value, not a ref into freed storage.
          while (field.type == VAL_REF) {
            Value deref = value_copy(field.as.ref_value.target);
            value_free(&field);
            field = deref;
          }
          obj.as.object_value.fields[i] = field;
        }
        vm_push(vm, obj);
        break;
      }
      case OP_BUF_ALLOC: {
        Value size_val = vm_pop(vm);
        if (size_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_ALLOC expects integer size");
          return VM_RUNTIME_ERROR;
        }
        int64_t size = size_val.as.int_value;
        if (size < 0) {
          diag_error(NULL, 0, 0, "OP_BUF_ALLOC size must be positive");
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, value_buffer((size_t)size));
        break;
      }
      case OP_BUF_FREE: {
        Value buf = vm_pop(vm);
        if (buf.type != VAL_BUFFER) {
          diag_error(NULL, 0, 0, "OP_BUF_FREE expects buffer");
          return VM_RUNTIME_ERROR;
        }
        value_free(&buf);
        break;
      }
      case OP_BUF_GET: {
        Value idx_val = vm_pop(vm);
        Value buf_val = vm_pop(vm);

        Value* resolved_buf = &buf_val;
        while (resolved_buf->type == VAL_REF) {
            resolved_buf = resolved_buf->as.ref_value.target;
        }

        if (resolved_buf->type != VAL_BUFFER || idx_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_GET invalid arguments");
          value_free(&buf_val); value_free(&idx_val);
          return VM_RUNTIME_ERROR;
        }
        ValueBuffer* vb = resolved_buf->as.buffer_value;
        int64_t idx = idx_val.as.int_value;
        if (idx < 0 || (size_t)idx >= vb->count) {
          diag_error(NULL, 0, 0, "OP_BUF_GET out of bounds");
          value_free(&buf_val); value_free(&idx_val);
          return VM_RUNTIME_ERROR;
        }
        Value res = value_copy(&vb->items[idx]);
        vm_push(vm, res);
        value_free(&buf_val); value_free(&idx_val);
        break;
      }
      case OP_BUF_REF: {
        // Aliasing buf_get: push a VAL_REF directly into the buffer's
        // backing storage so writes through the borrow are visible.
        // Mirrors the C backend's inlined `*((T*)((char*)buf + i*sz))`
        // lvalue. The next byte is a 1-byte kind (REF_VIEW or REF_MOD).
        uint8_t kind_byte = *vm->ip++;
        ReferenceKind kind = (kind_byte == 0) ? REF_VIEW : REF_MOD;
        Value idx_val = vm_pop(vm);
        Value buf_val = vm_pop(vm);
        Value* resolved_buf = &buf_val;
        while (resolved_buf->type == VAL_REF) {
            resolved_buf = resolved_buf->as.ref_value.target;
        }
        if (resolved_buf->type != VAL_BUFFER || idx_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_REF invalid arguments");
          value_free(&buf_val); value_free(&idx_val);
          return VM_RUNTIME_ERROR;
        }
        ValueBuffer* vb = resolved_buf->as.buffer_value;
        int64_t idx = idx_val.as.int_value;
        if (idx < 0 || (size_t)idx >= vb->count) {
          diag_error(NULL, 0, 0, "OP_BUF_REF out of bounds");
          value_free(&buf_val); value_free(&idx_val);
          return VM_RUNTIME_ERROR;
        }
        // The buffer's items array is heap-stable for the lifetime of
        // the buffer (capacity grows by realloc, but no shrink during
        // a borrow's lifetime under typical componentMod use). The
        // VAL_REF points directly at the slot.
        vm_push(vm, value_ref(&vb->items[idx], kind));
        value_free(&buf_val); value_free(&idx_val);
        break;
      }
      case OP_BUF_SET: {
        Value val = vm_pop(vm);
        Value idx_val = vm_pop(vm);
        Value buf_val = vm_pop(vm);
        
        Value* resolved_buf = &buf_val;
        while (resolved_buf->type == VAL_REF) {
            resolved_buf = resolved_buf->as.ref_value.target;
        }
        
        if (resolved_buf->type != VAL_BUFFER || idx_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_SET invalid arguments");
          value_free(&val); value_free(&idx_val); value_free(&buf_val);
          return VM_RUNTIME_ERROR;
        }
        ValueBuffer* vb = resolved_buf->as.buffer_value;
        int64_t idx = idx_val.as.int_value;
        if (idx < 0 || (size_t)idx >= vb->count) {
          diag_error(NULL, 0, 0, "OP_BUF_SET out of bounds");
          value_free(&val); value_free(&idx_val); value_free(&buf_val);
          return VM_RUNTIME_ERROR;
        }
        value_free(&vb->items[idx]);
        vb->items[idx] = value_copy(&val);
        value_free(&val); value_free(&idx_val); value_free(&buf_val);
        break;
      }
      case OP_BUF_LEN: {
        Value buf_val = vm_pop(vm);
        Value* resolved_buf = &buf_val;
        while (resolved_buf->type == VAL_REF) {
            resolved_buf = resolved_buf->as.ref_value.target;
        }
        if (resolved_buf->type != VAL_BUFFER) {
          diag_error(NULL, 0, 0, "OP_BUF_LEN expects buffer");
          value_free(&buf_val);
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, value_int((int64_t)resolved_buf->as.buffer_value->count));
        value_free(&buf_val);
        break;
      }
      case OP_BUF_RESIZE: {
        Value size_val = vm_pop(vm);
        Value* buf_stack_ptr = vm_peek(vm, 0); // Modifies in place on stack
        
        Value* resolved_buf = buf_stack_ptr;
        while (resolved_buf->type == VAL_REF) {
            resolved_buf = resolved_buf->as.ref_value.target;
        }
        
        if (resolved_buf->type != VAL_BUFFER || size_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_RESIZE invalid arguments");
          value_free(&size_val);
          return VM_RUNTIME_ERROR;
        }
        if (!value_buffer_resize(resolved_buf, (size_t)size_val.as.int_value)) {
          diag_error(NULL, 0, 0, "OP_BUF_RESIZE failed (out of memory)");
          value_free(&size_val);
          return VM_RUNTIME_ERROR;
        }
        value_free(&size_val);
        break;
      }
      case OP_BUF_COPY: {
        Value count_val = vm_pop(vm);
        Value dst_off_val = vm_pop(vm);
        Value dst_buf_val = vm_pop(vm);
        Value src_off_val = vm_pop(vm);
        Value src_buf_val = vm_pop(vm);
        
        Value* resolved_src = &src_buf_val;
        while (resolved_src->type == VAL_REF) resolved_src = resolved_src->as.ref_value.target;
        Value* resolved_dst = &dst_buf_val;
        while (resolved_dst->type == VAL_REF) resolved_dst = resolved_dst->as.ref_value.target;
        
        if (resolved_src->type != VAL_BUFFER || resolved_dst->type != VAL_BUFFER ||
            src_off_val.type != VAL_INT || dst_off_val.type != VAL_INT ||
            count_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_COPY invalid arguments");
          value_free(&count_val); value_free(&dst_off_val); value_free(&dst_buf_val);
          value_free(&src_off_val); value_free(&src_buf_val);
          return VM_RUNTIME_ERROR;
        }
        
        ValueBuffer* src = resolved_src->as.buffer_value;
        ValueBuffer* dst = resolved_dst->as.buffer_value;
        int64_t so = src_off_val.as.int_value;
        int64_t doff = dst_off_val.as.int_value;
        int64_t count = count_val.as.int_value;
        
        if (so < 0 || (size_t)so + count > src->count || 
            doff < 0 || (size_t)doff + count > dst->count || count < 0) {
          diag_error(NULL, 0, 0, "OP_BUF_COPY out of bounds");
          value_free(&count_val); value_free(&dst_off_val); value_free(&dst_buf_val);
          value_free(&src_off_val); value_free(&src_buf_val);
          return VM_RUNTIME_ERROR;
        }
        
        if (src == dst) {
            if (doff < so) {
                for (int64_t i = 0; i < count; i++) {
                    value_free(&dst->items[doff + i]);
                    dst->items[doff + i] = value_copy(&src->items[so + i]);
                }
            } else if (doff > so) {
                for (int64_t i = count - 1; i >= 0; i--) {
                    value_free(&dst->items[doff + i]);
                    dst->items[doff + i] = value_copy(&src->items[so + i]);
                }
            }
        } else {
            for (int64_t i = 0; i < count; i++) {
                value_free(&dst->items[doff + i]);
                dst->items[doff + i] = value_copy(&src->items[so + i]);
            }
        }
        value_free(&count_val); value_free(&dst_off_val); value_free(&dst_buf_val);
        value_free(&src_off_val); value_free(&src_buf_val);
        break;
      }
      case OP_RETURN: {
        uint8_t has_value = *vm->ip++;
        Value result;
        bool push_result = false;
        if (has_value) {
          result = vm_pop(vm);
          push_result = true;
        }
        
        CallFrame* current_frame = &vm->call_stack[vm->call_stack_top - 1];
        for (int i = 0; i < 256; i++) {
          value_free(&current_frame->locals[i]);
        }

        if (vm->call_stack_top <= 1) {
          vm->call_stack_top = 0;
          if (push_result) value_free(&result);
          return VM_RUNTIME_OK;
        }
        CallFrame* frame = &vm->call_stack[--vm->call_stack_top];
        vm->ip = frame->return_ip;
        vm->stack_top = frame->slots;
        if (push_result) {
          vm_push(vm, result);
        } else {
          vm_push(vm, value_none());
        }
        break;
      }
      default: {
        char buf[128];
        snprintf(buf, sizeof(buf), "unknown opcode 0x%02X encountered in VM at offset %zu", instruction, (size_t)(vm->ip - vm->chunk->code - 1));
        diag_error(NULL, 0, 0, buf);
        return VM_RUNTIME_ERROR;
      }
    }
  }
}
