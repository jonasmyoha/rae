#include "vm_chunk.h"

#include <stdlib.h>
#include <string.h>

static void* grow_array(void* ptr, size_t element_size, size_t old_count, size_t new_count) {
  size_t new_size = element_size * new_count;
  void* result = realloc(ptr, new_size);
  if (!result) {
    free(ptr);
  } else if (new_count > old_count) {
    size_t diff = (new_count - old_count) * element_size;
    memset((uint8_t*)result + element_size * old_count, 0, diff);
  }
  return result;
}

void chunk_init(Chunk* chunk) {
  if (!chunk) return;
  chunk->code = NULL;
  chunk->lines = NULL;
  chunk->code_count = 0;
  chunk->code_capacity = 0;
  chunk->constants = NULL;
  chunk->constants_count = 0;
  chunk->constants_capacity = 0;
}

void chunk_free(Chunk* chunk) {
  if (!chunk) return;
  for (size_t i = 0; i < chunk->constants_count; ++i) {
    value_free(&chunk->constants[i]);
  }
  free(chunk->code);
  free(chunk->lines);
  free(chunk->constants);
  chunk_init(chunk);
}

void chunk_write(Chunk* chunk, uint8_t byte, int line) {
  if (!chunk) return;
  if (chunk->code_count + 1 > chunk->code_capacity) {
    size_t old_capacity = chunk->code_capacity;
    size_t new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
    chunk->code = grow_array(chunk->code, sizeof(uint8_t), old_capacity, new_capacity);
    chunk->lines = grow_array(chunk->lines, sizeof(int), old_capacity, new_capacity);
    chunk->code_capacity = new_capacity;
  }
  chunk->code[chunk->code_count] = byte;
  chunk->lines[chunk->code_count] = line;
  chunk->code_count += 1;
}

uint16_t chunk_add_constant(Chunk* chunk, Value value) {
  if (!chunk) return 0;
  if (chunk->constants_count + 1 > chunk->constants_capacity) {
    size_t old_capacity = chunk->constants_capacity;
    size_t new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
    chunk->constants =
        grow_array(chunk->constants, sizeof(Value), old_capacity, new_capacity);
    chunk->constants_capacity = new_capacity;
  }
  chunk->constants[chunk->constants_count] = value;
  chunk->constants_count += 1;
  return (uint16_t)(chunk->constants_count - 1);
}
