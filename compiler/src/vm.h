#ifndef VM_H
#define VM_H

#include "vm_chunk.h"

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
  OP_ADD = 0x10,
  OP_SUB = 0x11,
  OP_MUL = 0x12,
  OP_DIV = 0x13,
  OP_NEG = 0x14
} OpCode;

typedef enum {
  VM_RUNTIME_OK = 0,
  VM_RUNTIME_ERROR
} VMResult;

typedef struct {
  uint8_t* return_ip;
  Value* slots;
  uint16_t slot_count;
} CallFrame;

typedef struct {
  Chunk* chunk;
  uint8_t* ip;
  Value stack[256];
  Value* stack_top;
  CallFrame call_stack[256];
  size_t call_stack_top;
} VM;

void vm_init(VM* vm);
VMResult vm_run(VM* vm, Chunk* chunk);
void vm_reset_stack(VM* vm);

#endif /* VM_H */
