#ifndef VM_H
#define VM_H

#include "vm_chunk.h"

typedef enum {
  OP_CONSTANT = 0x01,
  OP_LOG = 0x02,
  OP_LOG_S = 0x03,
  OP_CALL = 0x04,
  OP_RETURN = 0xFF
} OpCode;

typedef enum {
  VM_RUNTIME_OK = 0,
  VM_RUNTIME_ERROR
} VMResult;

typedef struct {
  Chunk* chunk;
  uint8_t* ip;
  Value stack[256];
  Value* stack_top;
  uint8_t* call_stack[256];
  size_t call_stack_top;
} VM;

void vm_init(VM* vm);
VMResult vm_run(VM* vm, Chunk* chunk);
void vm_reset_stack(VM* vm);

#endif /* VM_H */
