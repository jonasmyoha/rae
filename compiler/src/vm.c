#include "vm.h"

#include <stdio.h>
#include <stdlib.h> // For malloc and free
#include <string.h>
#include <math.h>

#include "diag.h"
#include "vm_registry.h"
#include "vm_value.h" // For Value, value_list, value_list_add

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
  if (vm->stack_top >= vm->stack + STACK_MAX) {
    diag_error(NULL, 0, 0, "VM stack overflow");
    return;
  }
  *vm->stack_top = value;
  vm->stack_top += 1;
}

static Value* vm_peek(VM* vm, size_t distance) {
  return vm->stack_top - 1 - distance;
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
  vm_reset_stack(vm);
  vm->call_stack_top = 0;
  vm->registry = NULL;
  vm->timeout_seconds = 0;
  vm->start_time = 0;
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

static uint16_t read_short(VM* vm) {
  uint16_t value = (uint16_t)(vm->ip[0] << 8 | vm->ip[1]);
  vm->ip += 2;
  return value;
}

static CallFrame* vm_current_frame(VM* vm) {
  if (vm->call_stack_top == 0) {
    return NULL;
  }
  return &vm->call_stack[vm->call_stack_top - 1];
}

VMResult vm_run(VM* vm, Chunk* chunk) {
  if (!vm || !chunk) return VM_RUNTIME_ERROR;
  
  bool is_resume = (vm->chunk == chunk && vm->ip >= chunk->code && vm->ip < chunk->code + chunk->code_count);
  vm->chunk = chunk;
  
  if (!is_resume) {
      vm->ip = chunk->code;
      vm->start_time = time(NULL);
  }

  for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    // Debug output
#endif
    uint8_t instruction = *vm->ip++;
    switch (instruction) {
      case OP_CONSTANT: {
        uint16_t index = read_short(vm);
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
        uint16_t target = read_short(vm);
        vm->ip = vm->chunk->code + target;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t target = read_short(vm);
        Value* condition = vm_peek(vm, 0);
        if (!value_is_truthy(condition)) {
          vm->ip = vm->chunk->code + target;
        }
        break;
      }
      case OP_CALL: {
        uint16_t target = read_short(vm);
        uint8_t arg_count = *vm->ip++;
        if (vm->stack_top - vm->stack < arg_count) {
          diag_error(NULL, 0, 0, "not enough arguments on stack for call");
          return VM_RUNTIME_ERROR;
        }
        if (vm->call_stack_top >= sizeof(vm->call_stack) / sizeof(vm->call_stack[0])) {
          diag_error(NULL, 0, 0, "call stack overflow");
          return VM_RUNTIME_ERROR;
        }
        CallFrame* frame = &vm->call_stack[vm->call_stack_top++];
        frame->return_ip = vm->ip;
        frame->slots = vm->stack_top - arg_count;
        frame->locals_base = frame->slots;
        frame->slot_count = arg_count;
        
        // Zero-initialize locals to prevent garbage values
        for (int i = 0; i < 256; i++) {
          frame->locals[i] = value_none();
        }

        // Copy arguments to stable locals storage
        for (int i = 0; i < arg_count; i++) {
          // DON'T value_copy here, just assign.
          // Arguments are already prepared by the caller (either a new copy or a reference).
          frame->locals[i] = frame->slots[i];
        }
        if (target >= vm->chunk->code_count) {
          diag_error(NULL, 0, 0, "invalid function address");
          return VM_RUNTIME_ERROR;
        }
        vm->ip = vm->chunk->code + target;
        break;
      }
      case OP_NATIVE_CALL: {
        uint16_t const_index = read_short(vm);
        uint8_t arg_count = *vm->ip++;
        if (!vm->registry) {
          diag_error(NULL, 0, 0, "native call attempted without registry");
          return VM_RUNTIME_ERROR;
        }
        if (const_index >= vm->chunk->constants_count) {
          diag_error(NULL, 0, 0, "native symbol index OOB");
          return VM_RUNTIME_ERROR;
        }
        Value symbol = vm->chunk->constants[const_index];
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
          diag_error(NULL, 0, 0, "native function not registered");
          return VM_RUNTIME_ERROR;
        }
        const Value* args = vm->stack_top - arg_count;
        
        VmNativeResult result = {.has_value = false};
        if (!entry->callback(vm, &result, args, arg_count, entry->user_data)) {
          diag_error(NULL, 0, 0, "native function reported failure");
          return VM_RUNTIME_ERROR;
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
        uint16_t slot = read_short(vm);
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
        uint16_t slot = read_short(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local access outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (slot >= 256) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        Value value = value_copy(vm_peek(vm, 0));
        if (frame->locals[slot].type == VAL_REF) {
          if (frame->locals[slot].as.ref_value.kind == REF_VIEW) {
            diag_error(NULL, 0, 0, "cannot assign to a read-only 'view' reference");
            value_free(&value);
            return VM_RUNTIME_ERROR;
          }
          value_free(frame->locals[slot].as.ref_value.target);
          *frame->locals[slot].as.ref_value.target = value_copy(&value);
        } else {
          value_free(&frame->locals[slot]);
          frame->locals[slot] = value_copy(&value);
        }
        value_free(&value);
        break;
      }
      case OP_BIND_LOCAL: {
        uint16_t slot = read_short(vm);
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
      case OP_ALLOC_LOCAL: {
        uint16_t required = read_short(vm);
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
      case OP_POP:
        vm_pop(vm);
        break;
      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV:
      case OP_MOD: {
        Value rhs = vm_pop(vm);
        Value lhs = vm_pop(vm);
        
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
          diag_error(NULL, 0, 0, "arithmetic operands must be numbers");
          return VM_RUNTIME_ERROR;
        }
        break;
      }
      case OP_NEG: {
        Value operand = vm_pop(vm);
        if (operand.type == VAL_INT) {
          vm_push(vm, value_int(-operand.as.int_value));
        } else if (operand.type == VAL_FLOAT) {
          vm_push(vm, value_float(-operand.as.float_value));
        } else {
          diag_error(NULL, 0, 0, "negation expects numeric operand");
          return VM_RUNTIME_ERROR;
        }
        break;
      }
      case OP_LT:
      case OP_LE:
      case OP_GT:
      case OP_GE: {
        Value rhs = vm_pop(vm);
        Value lhs = vm_pop(vm);
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
        bool equal = values_equal(&lhs, &rhs);
        if (instruction == OP_NE) {
          equal = !equal;
        }
        vm_push(vm, value_bool(equal));
        break;
      }
      case OP_NOT: {
        Value operand = vm_pop(vm);
        vm_push(vm, value_bool(!value_is_truthy(&operand)));
        break;
      }
      case OP_GET_FIELD: {
        uint16_t index = read_short(vm);
        Value obj_val = vm_pop(vm);
        Value* target = &obj_val;
        if (obj_val.type == VAL_REF) {
          target = obj_val.as.ref_value.target;
        }
        
        if (target->type != VAL_OBJECT) {
          diag_error(NULL, 0, 0, "GET_FIELD on non-object");
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        }
        if (index >= target->as.object_value.field_count) {
          diag_error(NULL, 0, 0, "GET_FIELD index out of range");
          value_free(&obj_val);
          return VM_RUNTIME_ERROR;
        }
        Value res = value_copy(&target->as.object_value.fields[index]);
        vm_push(vm, res);
        value_free(&obj_val);
        break;
      }
      case OP_SET_FIELD: {
        uint16_t index = read_short(vm);
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
        uint16_t slot = read_short(vm);
        uint16_t index = read_short(vm);
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
        uint16_t index = read_short(vm);
        Value value = vm_pop(vm); // The reference to bind
        Value* obj_ptr = vm_peek(vm, 0); // Pointer to target object on stack
        
        Value obj = *obj_ptr;
        if (obj.type == VAL_REF) {
          obj_ptr = obj.as.ref_value.target;
          obj = *obj_ptr;
        }
        
        if (obj.type != VAL_OBJECT) {
          diag_error(NULL, 0, 0, "BIND_FIELD on non-object");
          return VM_RUNTIME_ERROR;
        }
        if (index >= obj.as.object_value.field_count) {
          diag_error(NULL, 0, 0, "BIND_FIELD index out of range");
          return VM_RUNTIME_ERROR;
        }
        
        // Actually we need to set the field in the HEAP object
        obj.as.object_value.fields[index] = value;
        // value remains on stack as result of bind? typically yes for assign
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
        uint16_t slot = read_short(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) return VM_RUNTIME_ERROR;
        if (slot >= 256) return VM_RUNTIME_ERROR;
        Value* target = &frame->locals[slot];
        if (target->type == VAL_NONE) {
          vm_push(vm, value_none());
        } else {
          // Resolve pointer chain: if slot already holds a reference, point to its target.
          while (target->type == VAL_REF) {
            target = target->as.ref_value.target;
          }
          vm_push(vm, value_ref(target, instruction == OP_MOD_LOCAL ? REF_MOD : REF_VIEW));
        }
        break;
      }
      case OP_VIEW_FIELD:
      case OP_MOD_FIELD: {
        uint16_t index = read_short(vm);
        Value obj = vm_pop(vm);
        if (obj.type == VAL_REF) {
          obj = *obj.as.ref_value.target;
        }
        if (obj.type != VAL_OBJECT) return VM_RUNTIME_ERROR;
        Value* target = &obj.as.object_value.fields[index];
        if (target->type == VAL_NONE) {
          vm_push(vm, value_none());
        } else {
          vm_push(vm, value_ref(target, instruction == OP_MOD_FIELD ? REF_MOD : REF_VIEW));
        }
        break;
      }
      case OP_DUP: {
        vm_push(vm, value_copy(vm_peek(vm, 0)));
        break;
      }
      case OP_CONSTRUCT: {
        uint16_t field_count = read_short(vm);
        Value obj = value_object(field_count);
        for (int i = (int)field_count - 1; i >= 0; --i) {
          obj.as.object_value.fields[i] = vm_pop(vm);
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
        if (buf_val.type != VAL_BUFFER || idx_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_GET invalid arguments");
          return VM_RUNTIME_ERROR;
        }
        ValueBuffer* vb = buf_val.as.buffer_value;
        int64_t idx = idx_val.as.int_value;
        if (idx < 0 || (size_t)idx >= vb->count) {
          diag_error(NULL, 0, 0, "OP_BUF_GET out of bounds");
          return VM_RUNTIME_ERROR;
        }
        Value res = value_copy(&vb->items[idx]);
        // printf("[VM] BUF_GET: idx=%lld, type=%d\n", idx, res.type);
        vm_push(vm, res);
        break;
      }
      case OP_BUF_SET: {
        Value val = vm_pop(vm);
        Value idx_val = vm_pop(vm);
        Value buf_val = vm_pop(vm);
        if (buf_val.type != VAL_BUFFER || idx_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_SET invalid arguments");
          return VM_RUNTIME_ERROR;
        }
        ValueBuffer* vb = buf_val.as.buffer_value;
        int64_t idx = idx_val.as.int_value;
        if (idx < 0 || (size_t)idx >= vb->count) {
          diag_error(NULL, 0, 0, "OP_BUF_SET out of bounds");
          return VM_RUNTIME_ERROR;
        }
        // printf("[VM] BUF_SET: idx=%lld, type=%d\n", idx, val.type);
        value_free(&vb->items[idx]);
        vb->items[idx] = value_copy(&val);
        break;
      }
      case OP_BUF_LEN: {
        Value buf_val = vm_pop(vm);
        if (buf_val.type != VAL_BUFFER) {
          diag_error(NULL, 0, 0, "OP_BUF_LEN expects buffer");
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, value_int((int64_t)buf_val.as.buffer_value->count));
        break;
      }
      case OP_BUF_RESIZE: {
        Value size_val = vm_pop(vm);
        Value* buf_val = vm_peek(vm, 0); // Modifies in place on stack
        if (buf_val->type != VAL_BUFFER || size_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_RESIZE invalid arguments");
          return VM_RUNTIME_ERROR;
        }
        if (!value_buffer_resize(buf_val, (size_t)size_val.as.int_value)) {
          diag_error(NULL, 0, 0, "OP_BUF_RESIZE failed (out of memory)");
          return VM_RUNTIME_ERROR;
        }
        break;
      }
      case OP_BUF_COPY: {
        Value count_val = vm_pop(vm);
        Value dst_off_val = vm_pop(vm);
        Value dst_buf_val = vm_pop(vm);
        Value src_off_val = vm_pop(vm);
        Value src_buf_val = vm_pop(vm);
        
        if (src_buf_val.type != VAL_BUFFER || dst_buf_val.type != VAL_BUFFER ||
            src_off_val.type != VAL_INT || dst_off_val.type != VAL_INT ||
            count_val.type != VAL_INT) {
          diag_error(NULL, 0, 0, "OP_BUF_COPY invalid arguments");
          return VM_RUNTIME_ERROR;
        }
        
        ValueBuffer* src = src_buf_val.as.buffer_value;
        ValueBuffer* dst = dst_buf_val.as.buffer_value;
        int64_t so = src_off_val.as.int_value;
        int64_t doff = dst_off_val.as.int_value;
        int64_t count = count_val.as.int_value;
        
        if (so < 0 || (size_t)so + count > src->count || 
            doff < 0 || (size_t)doff + count > dst->count || count < 0) {
          diag_error(NULL, 0, 0, "OP_BUF_COPY out of bounds");
          return VM_RUNTIME_ERROR;
        }
        
        memmove(dst->items + doff, src->items + so, count * sizeof(Value));
        if (src != dst) {
            for (int64_t i = 0; i < count; i++) {
                dst->items[doff + i] = value_copy(&src->items[so + i]);
            }
        }
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
        if (vm->call_stack_top == 0) {
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
