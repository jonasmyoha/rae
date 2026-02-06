#ifndef VM_VALUE_H
#define VM_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  VAL_INT,
  VAL_FLOAT,
  VAL_BOOL,
  VAL_STRING,
  VAL_CHAR,
  VAL_NONE,
  VAL_OBJECT,
  VAL_ARRAY,
  VAL_BUFFER,
  VAL_REF,
  VAL_ID,
  VAL_KEY
} ValueType;

typedef struct {
  char* chars;
  size_t length;
} OwnedString;

struct Value;

typedef enum {
  REF_VIEW,
  REF_MOD
} ReferenceKind;

typedef struct {
  struct Value* target;
  ReferenceKind kind;
} Reference;

typedef struct {
  struct Value* items;
  size_t count;
  size_t capacity;
  size_t ref_count;
} ValueBuffer;

typedef struct {
  struct Value* items;
  size_t count;
} ValueArray;

typedef struct {
  struct Value* fields;
  size_t field_count;
  char* type_name;
} Object;

typedef struct Value {
  ValueType type;
  union {
    int64_t int_value;
    double float_value;
    bool bool_value;
    int64_t char_value;
    OwnedString string_value;
    Object object_value;
    ValueBuffer* buffer_value;
    ValueArray* array_value;
    Reference ref_value;
    int64_t id_value;
    OwnedString key_value;
  } as;
} Value;

Value value_int(int64_t v);
Value value_float(double v);
Value value_bool(bool v);
Value value_char(int64_t v);
Value value_string_copy(const char* data, size_t length);
Value value_string_take(char* data, size_t length);
Value value_none(void);
Value value_object(size_t field_count, const char* type_name);
Value value_array(size_t count);
Value value_buffer(size_t capacity);
bool value_buffer_resize(Value* buffer, size_t new_capacity);
Value value_ref(Value* target, ReferenceKind kind);
Value value_id(int64_t v);
Value value_key_copy(const char* data, size_t length);
Value value_copy(const Value* value);
void value_free(Value* value);
void value_print(const Value* value);

typedef const char** (*VmFieldNamesResolver)(void* user_data, const char* type_name, size_t* out_count);
Value value_to_json(const Value* value, VmFieldNamesResolver resolver, void* user_data);

Value value_to_binary(const Value* value);
Value value_from_binary(const Value* buffer, const char* type_name, VmFieldNamesResolver resolver, void* user_data);

#endif /* VM_VALUE_H */
