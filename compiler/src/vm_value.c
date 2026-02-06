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

Value value_object(size_t field_count, const char* type_name) {
  Value value = {.type = VAL_OBJECT};
  value.as.object_value.field_count = field_count;
  value.as.object_value.fields = malloc(field_count * sizeof(Value));
  value.as.object_value.type_name = type_name ? strdup(type_name) : NULL;
  if (value.as.object_value.fields) {
    for (size_t i = 0; i < field_count; i++) {
      value.as.object_value.fields[i] = value_none();
    }
  }
  return value;
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

Value value_buffer(size_t capacity) {
  Value value = {.type = VAL_BUFFER};
  value.as.buffer_value = malloc(sizeof(ValueBuffer));
  if (value.as.buffer_value) {
    value.as.buffer_value->capacity = capacity;
    value.as.buffer_value->count = capacity; // Initially, buffer is fixed size
    value.as.buffer_value->ref_count = 1;
    value.as.buffer_value->items = malloc(capacity * sizeof(Value));
    if (value.as.buffer_value->items) {
      for (size_t i = 0; i < capacity; i++) {
        value.as.buffer_value->items[i].type = VAL_NONE;
      }
    }
  }
  return value;
}

bool value_buffer_resize(Value* buffer, size_t new_capacity) {
  if (!buffer || buffer->type != VAL_BUFFER || !buffer->as.buffer_value) return false;
  ValueBuffer* vb = buffer->as.buffer_value;
  size_t old_cap = vb->capacity;
  Value* new_items = realloc(vb->items, new_capacity * sizeof(Value));
  if (!new_items) return false;
  
  vb->items = new_items;
  vb->capacity = new_capacity;
  vb->count = new_capacity;
  
  if (new_capacity > old_cap) {
    for (size_t i = old_cap; i < new_capacity; i++) {
      vb->items[i].type = VAL_NONE;
    }
  }
  return true;
}

Value value_ref(Value* target, ReferenceKind kind) {
  Value value = {.type = VAL_REF};
  value.as.ref_value.target = target;
  value.as.ref_value.kind = kind;
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

Value value_to_json(const Value* value, VmFieldNamesResolver resolver, void* user_data) {
  if (!value) return value_string_copy("null", 4);
  
  char buffer[1024]; // Simple buffer for now
  switch (value->type) {
    case VAL_NONE: return value_string_copy("null", 4);
    case VAL_BOOL: return value_string_copy(value->as.bool_value ? "true" : "false", value->as.bool_value ? 4 : 5);
    case VAL_INT: {
      int len = snprintf(buffer, sizeof(buffer), "%lld", (long long)value->as.int_value);
      return value_string_copy(buffer, len);
    }
    case VAL_FLOAT: {
      int len = snprintf(buffer, sizeof(buffer), "%g", value->as.float_value);
      return value_string_copy(buffer, len);
    }
    case VAL_STRING: {
      // Very basic escaping for now
      int len = snprintf(buffer, sizeof(buffer), "\"%s\"", value->as.string_value.chars ? value->as.string_value.chars : "");
      return value_string_copy(buffer, len);
    }
    case VAL_OBJECT: {
      const char* type_name = value->as.object_value.type_name;
      size_t field_count = 0;
      const char** field_names = NULL;
      if (type_name && resolver) {
          field_names = resolver(user_data, type_name, &field_count);
      }
      
      if (!field_names || field_count != value->as.object_value.field_count) {
          return value_string_copy("{}", 2);
      }

      // Pre-calculate length
      size_t total_len = 2; // { }
      for (size_t i = 0; i < field_count; i++) {
          total_len += strlen(field_names[i]) + 4; // "name": 
          Value field_json = value_to_json(&value->as.object_value.fields[i], resolver, user_data);
          total_len += field_json.as.string_value.length;
          value_free(&field_json);
          if (i < field_count - 1) total_len += 2; // , 
      }

      char* res = malloc(total_len + 1);
      strcpy(res, "{");
      for (size_t i = 0; i < field_count; i++) {
          strcat(res, "\"");
          strcat(res, field_names[i]);
          strcat(res, "\": ");
          Value field_json = value_to_json(&value->as.object_value.fields[i], resolver, user_data);
          strcat(res, field_json.as.string_value.chars);
          value_free(&field_json);
          if (i < field_count - 1) strcat(res, ", ");
      }
      strcat(res, "}");
      Value result = value_string_take(res, total_len);
      return result;
    }
    default:
      return value_string_copy("null", 4);
  }
}

Value value_to_binary(const Value* value) {
  if (!value) return value_buffer(0);
  
  // Very simple binary format: type byte + data
  // For primitive types it's fixed size. For strings/objects it's variable.
  switch (value->type) {
    case VAL_INT: {
      Value buf = value_buffer(9);
      buf.as.buffer_value->items[0] = value_int(VAL_INT);
      buf.as.buffer_value->items[1] = value_int(value->as.int_value);
      buf.as.buffer_value->count = 2;
      return buf;
    }
    // ... Simplified: just return a dummy for now to verify pipeline
    default: {
      Value buf = value_buffer(1);
      buf.as.buffer_value->items[0] = value_int(value->type);
      buf.as.buffer_value->count = 1;
      return buf;
    }
  }
}

Value value_from_binary(const Value* buffer, const char* type_name, VmFieldNamesResolver resolver, void* user_data) {
    (void)type_name; (void)resolver; (void)user_data;
    if (!buffer || buffer->type != VAL_BUFFER || buffer->as.buffer_value->count < 1) return value_none();
    
    Value type_val = buffer->as.buffer_value->items[0];
    if (type_val.type == VAL_INT && type_val.as.int_value == VAL_INT && buffer->as.buffer_value->count >= 2) {
        return value_copy(&buffer->as.buffer_value->items[1]);
    }
    
    return value_none();
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
      copy.as.object_value.type_name = value->as.object_value.type_name ? strdup(value->as.object_value.type_name) : NULL;
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
    case VAL_BUFFER:
      // Shallow copy for Buffer: share the same heap-allocated ValueBuffer
      if (value->as.buffer_value) {
        value->as.buffer_value->ref_count++;
      }
      break;
    case VAL_REF:
      // References themselves are copied (pointing to the same target)
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
  } else if (value->type == VAL_OBJECT) {
    if (value->as.object_value.fields) {
      for (size_t i = 0; i < value->as.object_value.field_count; ++i) {
        value_free(&value->as.object_value.fields[i]);
      }
      free(value->as.object_value.fields);
    }
    if (value->as.object_value.type_name) {
      free(value->as.object_value.type_name);
    }
    value->as.object_value.fields = NULL;
    value->as.object_value.type_name = NULL;
    value->as.object_value.field_count = 0;
  } else if (value->type == VAL_ARRAY && value->as.array_value) {
    ValueArray* va = value->as.array_value;
    for (size_t i = 0; i < va->count; i++) {
      value_free(&va->items[i]);
    }
    free(va->items);
    free(va);
    value->as.array_value = NULL;
  } else if (value->type == VAL_BUFFER && value->as.buffer_value) {
    ValueBuffer* vb = value->as.buffer_value;
    vb->ref_count--;
    if (vb->ref_count == 0) {
      for (size_t i = 0; i < vb->count; i++) {
        value_free(&vb->items[i]);
      }
      free(vb->items);
      free(vb);
    }
    value->as.buffer_value = NULL;
  } else if (value->type == VAL_REF) {
    // Nothing to free for the reference itself, it's a weak pointer
    value->as.ref_value.target = NULL;
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
        // Note: we don't have field names at runtime easily here without more metadata
        // but we can at least print the values
        value_print(&value->as.object_value.fields[i]);
      }
      printf(" }");
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
    case VAL_BUFFER:
      printf("#(");
      if (value->as.buffer_value) {
        for (size_t i = 0; i < value->as.buffer_value->count; i++) {
          if (i > 0) printf(", ");
          value_print(&value->as.buffer_value->items[i]);
        }
      }
      printf(")");
      break;
    case VAL_REF:
      printf(value->as.ref_value.kind == REF_VIEW ? "view " : "mod ");
      value_print(value->as.ref_value.target);
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
