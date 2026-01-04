#include "vm.h"

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "vm_registry.h"

static Value vm_pop(VM* vm) {
  if (vm->stack_top == vm->stack) {
    diag_error(NULL, 0, 0, "VM stack underflow");
    Value zero = value_int(0);
    return zero;
  }
  vm->stack_top -= 1;
  return *vm->stack_top;
}

static void vm_push(VM* vm, Value value) {
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
    case VAL_STRING:
      return value->as.string_value.length > 0;
  }
  return false;
}

static bool values_equal(const Value* a, const Value* b) {
  if (a->type != b->type) {
    return false;
  }
  switch (a->type) {
    case VAL_BOOL:
      return a->as.bool_value == b->as.bool_value;
    case VAL_INT:
      return a->as.int_value == b->as.int_value;
    case VAL_STRING:
      if (!a->as.string_value.chars || !b->as.string_value.chars) return false;
      if (a->as.string_value.length != b->as.string_value.length) return false;
      return memcmp(a->as.string_value.chars, b->as.string_value.chars,
                    a->as.string_value.length) == 0;
  }
  return false;
}

void vm_init(VM* vm) {
  if (!vm) return;
  vm->chunk = NULL;
  vm_reset_stack(vm);
  vm->call_stack_top = 0;
  vm->registry = NULL;
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
  vm->chunk = chunk;
  vm->ip = chunk->code;

  for (;;) {
    uint8_t instruction = *vm->ip++;
    switch (instruction) {
      case OP_CONSTANT: {
        uint16_t index = read_short(vm);
        if (index >= chunk->constants_count) {
          diag_fatal("bytecode constant index OOB");
        }
        vm_push(vm, chunk->constants[index]);
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
        if (slot >= frame->slot_count) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL: {
        uint16_t slot = read_short(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local access outside of function");
          return VM_RUNTIME_ERROR;
        }
        if (slot >= frame->slot_count) {
          diag_error(NULL, 0, 0, "VM local slot out of range");
          return VM_RUNTIME_ERROR;
        }
        Value value = vm_pop(vm);
        frame->slots[slot] = value;
        break;
      }
      case OP_ALLOC_LOCAL: {
        uint16_t count = read_short(vm);
        CallFrame* frame = vm_current_frame(vm);
        if (!frame) {
          diag_error(NULL, 0, 0, "VM local allocation outside of function");
          return VM_RUNTIME_ERROR;
        }
        if ((frame->locals_base - vm->stack) + frame->slot_count + count >
            (int)(sizeof(vm->stack) / sizeof(vm->stack[0]))) {
          diag_error(NULL, 0, 0, "VM local storage overflow");
          return VM_RUNTIME_ERROR;
        }
        frame->slots = frame->locals_base;
        frame->slot_count += count;
        vm->stack_top = frame->slots + frame->slot_count;
        break;
      }
      case OP_POP:
        vm_pop(vm);
        break;
      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV: {
        Value rhs = vm_pop(vm);
        Value lhs = vm_pop(vm);
        if (lhs.type != VAL_INT || rhs.type != VAL_INT) {
          diag_error(NULL, 0, 0, "arithmetic operands must be integers");
          return VM_RUNTIME_ERROR;
        }
        int64_t result = 0;
        switch (instruction) {
          case OP_ADD:
            result = lhs.as.int_value + rhs.as.int_value;
            break;
          case OP_SUB:
            result = lhs.as.int_value - rhs.as.int_value;
            break;
          case OP_MUL:
            result = lhs.as.int_value * rhs.as.int_value;
            break;
          case OP_DIV:
            if (rhs.as.int_value == 0) {
              diag_error(NULL, 0, 0, "division by zero");
              return VM_RUNTIME_ERROR;
            }
            result = lhs.as.int_value / rhs.as.int_value;
            break;
          default:
            break;
        }
        vm_push(vm, value_int(result));
        break;
      }
      case OP_NEG: {
        Value operand = vm_pop(vm);
        if (operand.type != VAL_INT) {
          diag_error(NULL, 0, 0, "negation expects integer operand");
          return VM_RUNTIME_ERROR;
        }
        vm_push(vm, value_int(-operand.as.int_value));
        break;
      }
      case OP_LT:
      case OP_LE:
      case OP_GT:
      case OP_GE: {
        Value rhs = vm_pop(vm);
        Value lhs = vm_pop(vm);
        if (lhs.type != VAL_INT || rhs.type != VAL_INT) {
          diag_error(NULL, 0, 0, "comparison operands must be integers");
          return VM_RUNTIME_ERROR;
        }
        bool result = false;
        switch (instruction) {
          case OP_LT:
            result = lhs.as.int_value < rhs.as.int_value;
            break;
          case OP_LE:
            result = lhs.as.int_value <= rhs.as.int_value;
            break;
          case OP_GT:
            result = lhs.as.int_value > rhs.as.int_value;
            break;
          case OP_GE:
            result = lhs.as.int_value >= rhs.as.int_value;
            break;
          default:
            break;
        }
        vm_push(vm, value_bool(result));
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
        }
        break;
      }
      default:
        diag_error(NULL, 0, 0, "unknown opcode encountered in VM");
        return VM_RUNTIME_ERROR;
    }
  }
}
