#ifndef VM_CHUNK_H
#define VM_CHUNK_H

#include <stddef.h>
#include <stdint.h>

#include "vm_value.h"

typedef struct {
  char* name;
  size_t offset;
} FunctionDebugInfo;

typedef struct {
  uint8_t* code;
  int* lines;
  size_t code_count;
  size_t code_capacity;

  Value* constants;
  size_t constants_count;
  size_t constants_capacity;

  FunctionDebugInfo* functions;
  size_t functions_count;
  size_t functions_capacity;
} Chunk;

void chunk_init(Chunk* chunk);
void chunk_free(Chunk* chunk);
void chunk_write(Chunk* chunk, uint8_t byte, int line);
uint32_t chunk_add_constant(Chunk* chunk, Value value);
void chunk_add_function_info(Chunk* chunk, const char* name, size_t offset);

#endif /* VM_CHUNK_H */
