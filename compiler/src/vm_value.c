#include "vm_value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Value value_int(int64_t v) {
  Value value = {.type = VAL_INT};
  value.as.int_value = v;
  return value;
}

Value value_float(double v) {
  Value value = {.type = VAL_FLOAT};
  value.as.float_value = v;
  return value;
}

Value value_bool(bool v) {
  Value value = {.type = VAL_BOOL};
  value.as.bool_value = v;
  return value;
}

Value value_string_copy(const char* data, size_t length) {
  Value value = {.type = VAL_STRING};
  value.as.string_value.length = length;
  value.as.string_value.chars = malloc(length + 1);
  if (value.as.string_value.chars) {
    memcpy(value.as.string_value.chars, data, length);
    value.as.string_value.chars[length] = '\0';
  }
  return value;
}

Value value_string_take(char* data, size_t length) {
  Value value = {.type = VAL_STRING};
  value.as.string_value.chars = data;
  value.as.string_value.length = length;
  if (value.as.string_value.chars) {
    value.as.string_value.chars[length] = '\0';
  }
  return value;
}

Value value_none(void) {
  Value value = {.type = VAL_NONE};
  return value;
}

Value value_object(size_t field_count) {
  Value value = {.type = VAL_OBJECT};
  value.as.object_value.field_count = field_count;
  value.as.object_value.fields = calloc(field_count, sizeof(Value));
  return value;
}

void value_free(Value* value) {
  if (!value) return;
  if (value->type == VAL_STRING && value->as.string_value.chars) {
    free(value->as.string_value.chars);
    value->as.string_value.chars = NULL;
    value->as.string_value.length = 0;
  } else if (value->type == VAL_OBJECT && value->as.object_value.fields) {
    for (size_t i = 0; i < value->as.object_value.field_count; ++i) {
      value_free(&value->as.object_value.fields[i]);
    }
    free(value->as.object_value.fields);
    value->as.object_value.fields = NULL;
    value->as.object_value.field_count = 0;
  }
  value->type = VAL_INT;
  value->as.int_value = 0;
}

void value_print(const Value* value) {
  if (!value) return;
  switch (value->type) {
    case VAL_INT:
      printf("%lld", (long long)value->as.int_value);
      break;
    case VAL_FLOAT:
      printf("%g", value->as.float_value);
      break;
    case VAL_BOOL:
      printf("%s", value->as.bool_value ? "true" : "false");
      break;
    case VAL_STRING:
      if (value->as.string_value.chars) {
        fwrite(value->as.string_value.chars, 1, value->as.string_value.length, stdout);
      }
      break;
    case VAL_NONE:
      printf("none");
      break;
    case VAL_OBJECT:
      printf("{ ");
      for (size_t i = 0; i < value->as.object_value.field_count; i++) {
        if (i > 0) printf(", ");
        value_print(&value->as.object_value.fields[i]);
      }
      printf(" }");
      break;
  }
}
