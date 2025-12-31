#include "vm.h"

#include <stdio.h>

#include "diag.h"

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

void vm_init(VM* vm) {
  if (!vm) return;
  vm->chunk = NULL;
  vm_reset_stack(vm);
  vm->call_stack_top = 0;
}

void vm_reset_stack(VM* vm) {
  if (!vm) return;
  vm->stack_top = vm->stack;
  vm->call_stack_top = 0;
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
        frame->slot_count = arg_count;
        if (target >= vm->chunk->code_count) {
          diag_error(NULL, 0, 0, "invalid function address");
          return VM_RUNTIME_ERROR;
        }
        vm->ip = vm->chunk->code + target;
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
        if ((frame->slots - vm->stack) + frame->slot_count + count >
            (int)(sizeof(vm->stack) / sizeof(vm->stack[0]))) {
          diag_error(NULL, 0, 0, "VM local storage overflow");
          return VM_RUNTIME_ERROR;
        }
        for (uint16_t i = 0; i < count; ++i) {
          frame->slots[frame->slot_count + i] = value_int(0);
        }
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
