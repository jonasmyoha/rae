#ifndef VM_VALUE_H
#define VM_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sys_thread.h"

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
  VAL_KEY,
  /* A concurrent task handle (Task(T)). Holds an OS thread plus the
   * captured return value. Move-only at the language level, but the VM
   * copies values freely, so the heap object is ref-counted (like
   * ValueBuffer) and dropping the last reference JOINS the thread. */
  VAL_TASK
} ValueType;

/* Defined after `Value` below, since it stores the result by value. */
typedef struct TaskObj TaskObj;

typedef struct {
  uint8_t* chars;
  int64_t length;
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
    uint32_t char_value;
    OwnedString string_value;
    Object object_value;
    ValueBuffer* buffer_value;
    ValueArray* array_value;
    Reference ref_value;
    int64_t id_value;
    OwnedString key_value;
    TaskObj* task_value;
  } as;
} Value;

/* Heap object behind a VAL_TASK. `ref_count` mirrors ValueBuffer's
 * shallow-share-on-copy scheme. `status`: 0 running, 1 completed, 2
 * failed. The thread is joined exactly once (guarded by `joined`),
 * either explicitly at `task.get()` or implicitly when the last
 * reference is dropped (join-on-drop — the design's safe default). */
struct TaskObj {
  sys_thread_t thread;
  Value result;
  int status;
  bool joined;
  size_t ref_count;
};

Value value_int(int64_t v);
Value value_float(double v);
Value value_bool(bool v);
Value value_char(uint32_t v);
Value value_string_copy(const char* data, size_t length);
Value value_string_take(uint8_t* data, size_t length);
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
