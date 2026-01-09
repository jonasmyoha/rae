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

Value value_char(int64_t v) {
  Value value = {.type = VAL_CHAR};
  value.as.char_value = v;
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

Value value_list(void) {
  Value value = {.type = VAL_LIST};
  value.as.list_value = malloc(sizeof(ValueList));
  if (value.as.list_value) {
    value.as.list_value->items = NULL;
    value.as.list_value->count = 0;
    value.as.list_value->capacity = 0;
  }
  return value;
}

void value_list_add(Value* list, Value item) {
  if (!list || list->type != VAL_LIST || !list->as.list_value) return;
  ValueList* vl = list->as.list_value;
  if (vl->count + 1 > vl->capacity) {
    size_t new_cap = vl->capacity < 8 ? 8 : vl->capacity * 2;
    Value* new_items = realloc(vl->items, new_cap * sizeof(Value));
    if (new_items) {
      vl->items = new_items;
      vl->capacity = new_cap;
    }
  }
  vl->items[vl->count++] = item;
}

Value value_array(size_t count) {
  Value value = {.type = VAL_ARRAY};
  value.as.array_value = malloc(sizeof(ValueArray));
  if (value.as.array_value) {
    value.as.array_value->count = count;
    value.as.array_value->items = calloc(count, sizeof(Value));
  }
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
  } else if (value->type == VAL_LIST && value->as.list_value) {
    ValueList* vl = value->as.list_value;
    for (size_t i = 0; i < vl->count; i++) {
      value_free(&vl->items[i]);
    }
    free(vl->items);
    free(vl);
    value->as.list_value = NULL;
  } else if (value->type == VAL_ARRAY && value->as.array_value) {
    ValueArray* va = value->as.array_value;
    for (size_t i = 0; i < va->count; i++) {
      value_free(&va->items[i]);
    }
    free(va->items);
    free(va);
    value->as.array_value = NULL;
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
    case VAL_CHAR: {
      int64_t c = value->as.char_value;
      if (c < 0x80) {
        printf("%c", (char)c);
      } else if (c < 0x800) {
        printf("%c%c", (char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F)));
      } else if (c < 0x10000) {
        printf("%c%c%c", (char)(0xE0 | (c >> 12)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F)));
      } else {
        printf("%c%c%c%c", (char)(0xF0 | (c >> 18)), (char)(0x80 | ((c >> 12) & 0x3F)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F)));
      }
      break;
    }
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
    case VAL_LIST:
      printf("[");
      if (value->as.list_value) {
        for (size_t i = 0; i < value->as.list_value->count; i++) {
          if (i > 0) printf(", ");
          value_print(&value->as.list_value->items[i]);
        }
      }
      printf("]");
      break;
    case VAL_ARRAY:
      printf("@(");
      if (value->as.array_value) {
        for (size_t i = 0; i < value->as.array_value->count; i++) {
          if (i > 0) printf(", ");
          value_print(&value->as.array_value->items[i]);
        }
      }
      printf(")");
      break;
  }
}
