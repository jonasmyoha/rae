#include "vm.h"

#include <stdio.h>

#include "diag.h"

static Value vm_pop(VM* vm) {
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
        if (vm->call_stack_top >= sizeof(vm->call_stack) / sizeof(vm->call_stack[0])) {
          diag_error(NULL, 0, 0, "call stack overflow");
          return VM_RUNTIME_ERROR;
        }
        vm->call_stack[vm->call_stack_top++] = vm->ip;
        if (target >= vm->chunk->code_count) {
          diag_error(NULL, 0, 0, "invalid function address");
          return VM_RUNTIME_ERROR;
        }
        vm->ip = vm->chunk->code + target;
        break;
      }
      case OP_RETURN:
        if (vm->call_stack_top == 0) {
          return VM_RUNTIME_OK;
        }
        vm->ip = vm->call_stack[--vm->call_stack_top];
        break;
      default:
        diag_error(NULL, 0, 0, "unknown opcode encountered in VM");
        return VM_RUNTIME_ERROR;
    }
  }
}
