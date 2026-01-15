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
    case VAL_LIST:
    case VAL_ARRAY:
    case VAL_REF:
    case VAL_ID:
    case VAL_KEY:
      return true;
  }
  return false;
}

static bool values_equal(const Value* a, const Value* b) {
  const Value* lhs = a;
  const Value* rhs = b;
  if (lhs->type == VAL_REF) lhs = lhs->as.ref_value.target;
  if (rhs->type == VAL_REF) rhs = rhs->as.ref_value.target;
  
  
  if (lhs->type != rhs->type) {
    return false;
  }
  switch (lhs->type) {
    case VAL_BOOL:
      return lhs->as.bool_value == rhs->as.bool_value;
    case VAL_INT:
      return lhs->as.int_value == rhs->as.int_value;
    case VAL_FLOAT:
      return lhs->as.float_value == rhs->as.float_value;
    case VAL_STRING:
      if (!lhs->as.string_value.chars || !rhs->as.string_value.chars) return false;
      if (lhs->as.string_value.length != rhs->as.string_value.length) return false;
      return memcmp(lhs->as.string_value.chars, rhs->as.string_value.chars,
                    lhs->as.string_value.length) == 0;
    case VAL_KEY:
      if (!lhs->as.key_value.chars || !rhs->as.key_value.chars) return false;
      if (lhs->as.key_value.length != rhs->as.key_value.length) return false;
      return memcmp(lhs->as.key_value.chars, rhs->as.key_value.chars,
                    lhs->as.key_value.length) == 0;
    case VAL_CHAR:
      return lhs->as.char_value == rhs->as.char_value;
    case VAL_NONE:
      return true;
    case VAL_OBJECT:
      if (lhs->as.object_value.field_count != rhs->as.object_value.field_count) return false;
      for (size_t i = 0; i < lhs->as.object_value.field_count; i++) {
        if (!values_equal(&lhs->as.object_value.fields[i], &rhs->as.object_value.fields[i])) return false;
      }
      return true;
    case VAL_LIST:
      return lhs->as.list_value == rhs->as.list_value;
    case VAL_ARRAY:
      return lhs->as.array_value == rhs->as.array_value;
    case VAL_ID:
      return lhs->as.id_value == rhs->as.id_value;
    case VAL_REF:
      // References are equal if they point to the same storage
      return lhs->as.ref_value.target == rhs->as.ref_value.target;
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
    if (vm->reload_requested) {
        return VM_RUNTIME_RELOAD;
    }
    
    if (vm->timeout_seconds > 0) {
        if (time(NULL) - vm->start_time >= vm->timeout_seconds) {
            return VM_RUNTIME_TIMEOUT;
        }
    }
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
        
        // DEBUG: Log native call
        // printf("[VM] Calling native %s with %d args\n", symbol.as.string_value.chars, arg_count);
        // printf("[VM] Stack info: base=%p, top=%p, args=%p\n", (void*)vm->stack, (void*)vm->stack_top, (void*)args);
        // for (int i = 0; i < arg_count; i++) {
        //    printf("  arg[%d] type: %d\n", i, args[i].type);
        // }

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
        // printf("[VM] GET_LOCAL slot %d (type %d)\n", slot, frame->locals[slot].type);
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
        // printf("[VM] SET_LOCAL slot %d (new type %d)\n", slot, value.type);
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
        if (required > frame->slot_count) {
          if ((frame->locals_base - vm->stack) + required >
              (int)(sizeof(vm->stack) / sizeof(vm->stack[0]))) {
            diag_error(NULL, 0, 0, "VM local storage overflow");
            return VM_RUNTIME_ERROR;
          }
          frame->slot_count = required;
        }
        vm->stack_top = frame->slots + frame->slot_count;
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
        vm_push(vm, value_copy(&target->as.object_value.fields[index]));
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
        uint16_t count = read_short(vm);
        Value obj = value_object(count);
        for (int i = (int)count - 1; i >= 0; i--) {
          Value val = vm_pop(vm);
          obj.as.object_value.fields[i] = value_copy(&val);
          value_free(&val);
        }
        vm_push(vm, obj);
        break;
      }
      case OP_LIST: {
        uint16_t count = read_short(vm);
        Value list = value_list();
        // Pop elements in reverse order and add them to the list
        Value* temp_elements = malloc(count * sizeof(Value));
        if (!temp_elements) {
            diag_error(NULL, 0, 0, "VM out of memory for list literal");
            return VM_RUNTIME_ERROR;
        }
        for (int i = count - 1; i >= 0; --i) {
            temp_elements[i] = vm_pop(vm);
        }
        for (int i = 0; i < count; ++i) {
            value_list_add(&list, temp_elements[i]);
        }
        free(temp_elements);
        vm_push(vm, list);
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
