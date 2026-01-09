#ifndef VM_H
#define VM_H

#include <time.h>
#include "vm_chunk.h"
#include "vm_registry.h"

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
  OP_CONSTRUCT = 0x20
} OpCode;

typedef enum {
  VM_RUNTIME_OK = 0,
  VM_RUNTIME_ERROR,
  VM_RUNTIME_TIMEOUT
} VMResult;

typedef struct {
  uint8_t* return_ip;
  Value* slots;
  uint16_t slot_count;
  Value* locals_base;
} CallFrame;

typedef struct VM {
  Chunk* chunk;
  uint8_t* ip;
  Value stack[1024];
  Value* stack_top;
  CallFrame call_stack[1024];
  size_t call_stack_top;
  VmRegistry* registry;
  int timeout_seconds;
  time_t start_time;
} VM;

void vm_init(VM* vm);
VMResult vm_run(VM* vm, Chunk* chunk);
void vm_reset_stack(VM* vm);
void vm_set_registry(VM* vm, VmRegistry* registry);

#endif /* VM_H */
