#ifndef VM_VALUE_H
#define VM_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  VAL_INT,
  VAL_BOOL,
  VAL_STRING,
  VAL_NONE
} ValueType;

typedef struct {
  char* chars;
  size_t length;
} OwnedString;

typedef struct {
  ValueType type;
  union {
    int64_t int_value;
    bool bool_value;
    OwnedString string_value;
  } as;
} Value;

Value value_int(int64_t v);
Value value_bool(bool v);
Value value_string_copy(const char* data, size_t length);
Value value_string_take(char* data, size_t length);
Value value_none(void);
void value_free(Value* value);
void value_print(const Value* value);

#endif /* VM_VALUE_H */
