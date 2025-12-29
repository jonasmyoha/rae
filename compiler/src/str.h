/* str.h - String slice and utility functions */

#ifndef STR_H
#define STR_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
  const char* data;
  size_t len;
} Str;

Str str_from_cstr(const char* cstr);
Str str_from_buf(const char* data, size_t len);
bool str_eq(Str a, Str b);
bool str_eq_cstr(Str a, const char* cstr);
char* str_to_cstr(Str s);
bool str_is_empty(Str s);
char* read_file(const char* path, size_t* out_size);

#endif /* STR_H */
