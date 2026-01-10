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
  VAL_LIST,
  VAL_ARRAY,
  VAL_BORROW,
  VAL_ID,
  VAL_KEY
} ValueType;

typedef struct {
  char* chars;
  size_t length;
} OwnedString;

struct Value;

typedef enum {
  BORROW_VIEW,
  BORROW_MOD
} BorrowKind;

typedef struct {
  struct Value* target;
  BorrowKind kind;
} Borrow;

typedef struct {
  struct Value* items;
  size_t count;
  size_t capacity;
} ValueList;

typedef struct {
  struct Value* items;
  size_t count;
} ValueArray;

typedef struct {
  struct Value* fields;
  size_t field_count;
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
    ValueList* list_value;
    ValueArray* array_value;
    Borrow borrow_value;
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
Value value_object(size_t field_count);
Value value_list(void);
void value_list_add(Value* list, Value item);
Value value_array(size_t count);
Value value_borrow(Value* target, BorrowKind kind);
Value value_id(int64_t v);
Value value_key_copy(const char* data, size_t length);
Value value_copy(const Value* value);
void value_free(Value* value);
void value_print(const Value* value);

#endif /* VM_VALUE_H */
