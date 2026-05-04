/* str.c - String slice implementation */

#include "str.h"
#include "arena.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

Str str_from_cstr(const char* cstr) {
  return (Str){.data = cstr, .len = strlen(cstr)};
}

Str str_from_buf(const char* data, size_t len) {
  return (Str){.data = data, .len = len};
}

bool str_eq(Str a, Str b) {
  if (a.len != b.len) return false;
  return memcmp(a.data, b.data, a.len) == 0;
}

bool str_eq_cstr(Str a, const char* cstr) {
  return str_eq(a, str_from_cstr(cstr));
}

bool str_starts_with_cstr(Str s, const char* prefix) {
  size_t prefix_len = strlen(prefix);
  if (s.len < prefix_len) return false;
  return memcmp(s.data, prefix, prefix_len) == 0;
}

bool str_ends_with_cstr(Str s, const char* suffix) {
  size_t suffix_len = strlen(suffix);
  if (s.len < suffix_len) return false;
  return memcmp(s.data + s.len - suffix_len, suffix, suffix_len) == 0;
}

char* str_to_cstr(Str s) {
  char* result = malloc(s.len + 1);
  if (!result) return NULL;
  
  memcpy(result, s.data, s.len);
  result[s.len] = '\0';
  return result;
}

Str str_dup(Str s) {
    if (s.len == 0) return (Str){0};
    char* copy = malloc(s.len + 1);
    memcpy(copy, s.data, s.len);
    copy[s.len] = '\0';
    return str_from_buf(copy, s.len);
}

void str_free(Str s) {
    if (s.data) free((void*)s.data);
}

bool str_is_empty(Str s) {
  return s.len == 0;
}

Str str_dup_arena(Arena* arena, Str s) {
    if (s.len == 0) return (Str){0};
    char* copy = arena_alloc(arena, s.len + 1);
    memcpy(copy, s.data, s.len);
    copy[s.len] = '\0';
    return str_from_buf(copy, s.len);
}

void interner_init(StringInterner* interner, Arena* arena) {
  interner->arena = arena;
  interner->count = 0;
  interner->capacity = 1024;
  interner->strings = arena_alloc(arena, sizeof(Str) * interner->capacity);
}

Str interner_intern(StringInterner* interner, Str s) {
  for (size_t i = 0; i < interner->count; i++) {
    if (str_eq(interner->strings[i], s)) return interner->strings[i];
  }
  
  if (interner->count >= interner->capacity) {
    size_t new_cap = interner->capacity * 2;
    Str* new_strings = arena_alloc(interner->arena, sizeof(Str) * new_cap);
    memcpy(new_strings, interner->strings, sizeof(Str) * interner->count);
    interner->strings = new_strings;
    interner->capacity = new_cap;
  }
  
  Str interned = str_dup_arena(interner->arena, s);
  interner->strings[interner->count++] = interned;
  return interned;
}

Str interner_intern_cstr(StringInterner* interner, const char* s) {
  return interner_intern(interner, str_from_cstr(s));
}

char* read_file(const char* path, size_t* out_size) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_SET);
  
  char* buffer = malloc(size + 1);
  if (!buffer) {
    fclose(f);
    return NULL;
  }
  
  size_t read = fread(buffer, 1, size, f);
  fclose(f);
  
  if (read != (size_t)size) {
    free(buffer);
    return NULL;
  }
  
  buffer[size] = '\0';
  if (out_size) *out_size = size;
  return buffer;
}
