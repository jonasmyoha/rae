/* arena.h - Bump allocator for compiler memory management */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct Arena Arena;

Arena* arena_create(size_t capacity);
void* arena_alloc(Arena* a, size_t size);
void arena_reset(Arena* a);
void arena_destroy(Arena* a);
size_t arena_used(Arena* a);
size_t arena_capacity(Arena* a);

#endif /* ARENA_H */
