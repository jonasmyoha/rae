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
  value.as.object_value.fields = malloc(field_count * sizeof(Value));
  if (value.as.object_value.fields) {
    for (size_t i = 0; i < field_count; i++) {
      value.as.object_value.fields[i].type = VAL_NONE;
    }
  }
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
    size_t old_cap = vl->capacity;
    size_t new_cap = old_cap < 8 ? 8 : old_cap * 2;
    Value* new_items = realloc(vl->items, new_cap * sizeof(Value));
    if (new_items) {
      // Initialize new slots to VAL_NONE
      for (size_t i = old_cap; i < new_cap; i++) {
        new_items[i].type = VAL_NONE;
      }
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
    value.as.array_value->items = malloc(count * sizeof(Value));
    if (value.as.array_value->items) {
      for (size_t i = 0; i < count; i++) {
        value.as.array_value->items[i].type = VAL_NONE;
      }
    }
  }
  return value;
}

Value value_borrow(Value* target, BorrowKind kind) {
  Value value = {.type = VAL_BORROW};
  value.as.borrow_value.target = target;
  value.as.borrow_value.kind = kind;
  return value;
}

Value value_id(int64_t v) {
  Value value = {.type = VAL_ID};
  value.as.id_value = v;
  return value;
}

Value value_key_copy(const char* data, size_t length) {
  Value value = {.type = VAL_KEY};
  value.as.key_value.length = length;
  value.as.key_value.chars = malloc(length + 1);
  if (value.as.key_value.chars) {
    memcpy(value.as.key_value.chars, data, length);
    value.as.key_value.chars[length] = '\0';
  }
  return value;
}

Value value_copy(const Value* value) {
  if (!value) return value_none();
  
  Value copy = *value;
  switch (value->type) {
    case VAL_STRING:
      if (value->as.string_value.chars) {
        copy.as.string_value.chars = malloc(value->as.string_value.length + 1);
        memcpy(copy.as.string_value.chars, value->as.string_value.chars, value->as.string_value.length);
        copy.as.string_value.chars[value->as.string_value.length] = '\0';
      }
      break;
    case VAL_KEY:
      if (value->as.key_value.chars) {
        copy.as.key_value.chars = malloc(value->as.key_value.length + 1);
        memcpy(copy.as.key_value.chars, value->as.key_value.chars, value->as.key_value.length);
        copy.as.key_value.chars[value->as.key_value.length] = '\0';
      }
      break;
    case VAL_OBJECT:
      if (value->as.object_value.fields) {
        copy.as.object_value.fields = calloc(value->as.object_value.field_count, sizeof(Value));
        for (size_t i = 0; i < value->as.object_value.field_count; i++) {
          copy.as.object_value.fields[i] = value_copy(&value->as.object_value.fields[i]);
        }
      }
      break;
    case VAL_LIST:
      if (value->as.list_value) {
        copy.as.list_value = malloc(sizeof(ValueList));
        copy.as.list_value->count = value->as.list_value->count;
        copy.as.list_value->capacity = value->as.list_value->count;
        copy.as.list_value->items = malloc(copy.as.list_value->capacity * sizeof(Value));
        for (size_t i = 0; i < value->as.list_value->count; i++) {
          copy.as.list_value->items[i] = value_copy(&value->as.list_value->items[i]);
        }
      }
      break;
    case VAL_ARRAY:
      if (value->as.array_value) {
        copy.as.array_value = malloc(sizeof(ValueArray));
        copy.as.array_value->count = value->as.array_value->count;
        copy.as.array_value->items = malloc(copy.as.array_value->count * sizeof(Value));
        for (size_t i = 0; i < value->as.array_value->count; i++) {
          copy.as.array_value->items[i] = value_copy(&value->as.array_value->items[i]);
        }
      }
      break;
    case VAL_BORROW:
      // Borrows themselves are copied (pointing to the same target)
      break;
    default:
      break;
  }
  return copy;
}

void value_free(Value* value) {
  if (!value) return;
  if (value->type == VAL_STRING && value->as.string_value.chars) {
    free(value->as.string_value.chars);
    value->as.string_value.chars = NULL;
    value->as.string_value.length = 0;
  } else if (value->type == VAL_KEY && value->as.key_value.chars) {
    free(value->as.key_value.chars);
    value->as.key_value.chars = NULL;
    value->as.key_value.length = 0;
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
  } else if (value->type == VAL_BORROW) {
    // Nothing to free for the borrow itself, it's a weak reference
    value->as.borrow_value.target = NULL;
  }
  value->type = VAL_NONE;
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
    case VAL_BORROW:
      printf(value->as.borrow_value.kind == BORROW_VIEW ? "view " : "mod ");
      value_print(value->as.borrow_value.target);
      break;
    case VAL_ID:
      printf("Id(%lld)", (long long)value->as.id_value);
      break;
    case VAL_KEY:
      printf("Key(\"");
      if (value->as.key_value.chars) {
        fwrite(value->as.key_value.chars, 1, value->as.key_value.length, stdout);
      }
      printf("\")");
      break;
  }
}
