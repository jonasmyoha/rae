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
}

void vm_reset_stack(VM* vm) {
  if (!vm) return;
  vm->stack_top = vm->stack;
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
      case OP_PRINT: {
        Value value = vm_pop(vm);
        value_print(&value);
        printf("\n");
        break;
      }
      case OP_RETURN:
        return VM_RUNTIME_OK;
      default:
        diag_error(NULL, 0, 0, "unknown opcode encountered in VM");
        return VM_RUNTIME_ERROR;
    }
  }
}
