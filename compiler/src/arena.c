/* arena.c - Bump allocator implementation */

#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define ARENA_ALIGN 8

struct Arena {
  char* buffer;
  size_t capacity;
  size_t used;
};

static size_t align_up(size_t n, size_t align) {
  return (n + align - 1) & ~(align - 1);
}

Arena* arena_create(size_t capacity) {
  Arena* a = malloc(sizeof(Arena));
  if (!a) return NULL;
  
  a->buffer = malloc(capacity);
  if (!a->buffer) {
    free(a);
    return NULL;
  }
  
  a->capacity = capacity;
  a->used = 0;
  return a;
}

void* arena_alloc(Arena* a, size_t size) {
  assert(a != NULL);

  size_t aligned_size = align_up(size, ARENA_ALIGN);

  if (a->used + aligned_size > a->capacity) {
    // Abort with a clear message rather than return NULL. Callers
    // (mangler, sema, codegen) historically didn't null-check, and
    // a silent NULL leads to a memcpy crash deep in the mangler with
    // no useful trace. If you hit this, either: (a) the program
    // genuinely needs a bigger arena (bump the size at the call
    // site in main.c), or (b) there's an O(N^2) allocation pattern
    // somewhere that should be cached/avoided.
    fprintf(stderr, "arena_alloc: out of space (request=%zu, used=%zu, cap=%zu)\n",
            size, a->used, a->capacity);
    abort();
  }

  void* ptr = a->buffer + a->used;
  a->used += aligned_size;

  memset(ptr, 0, size);
  return ptr;
}

void arena_reset(Arena* a) {
  assert(a != NULL);
  a->used = 0;
}

void arena_destroy(Arena* a) {
  if (!a) return;
  free(a->buffer);
  free(a);
}

size_t arena_used(Arena* a) {
  assert(a != NULL);
  return a->used;
}

size_t arena_capacity(Arena* a) {
  assert(a != NULL);
  return a->capacity;
}
