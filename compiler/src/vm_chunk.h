#ifndef VM_CHUNK_H
#define VM_CHUNK_H

#include <stddef.h>
#include <stdint.h>

#include "vm_value.h"

typedef struct {
  uint8_t* code;
  int* lines;
  size_t code_count;
  size_t code_capacity;

  Value* constants;
  size_t constants_count;
  size_t constants_capacity;
} Chunk;

void chunk_init(Chunk* chunk);
void chunk_free(Chunk* chunk);
void chunk_write(Chunk* chunk, uint8_t byte, int line);
uint16_t chunk_add_constant(Chunk* chunk, Value value);

#endif /* VM_CHUNK_H */
