/* str.c - String slice implementation */

#include "str.h"
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

char* str_to_cstr(Str s) {
  char* result = malloc(s.len + 1);
  if (!result) return NULL;
  
  memcpy(result, s.data, s.len);
  result[s.len] = '\0';
  return result;
}

bool str_is_empty(Str s) {
  return s.len == 0;
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
