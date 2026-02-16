#include "rae_runtime.h"

typedef const char* const_char_p;
#define LIFT_TO_TEMP(T, ...) (&(T){__VA_ARGS__})
#define LIFT_TO_TEMP_STRUCT(T, ...) ((T){__VA_ARGS__})
#define LIFT_VALUE(T, ...) ((T){__VA_ARGS__})

// Built-in buffer functions
RAE_UNUSED void* rae_ext_rae_buf_alloc(int64_t size, int64_t elemSize);
RAE_UNUSED void rae_ext_rae_buf_free(void* ptr);
RAE_UNUSED void* rae_ext_rae_buf_resize(void* ptr, int64_t new_size, int64_t elemSize);
RAE_UNUSED void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elemSize);

// Forward declarations for specialized generics
typedef struct rae_List_rae_String rae_List_rae_String;
typedef struct rae_List_int64_t rae_List_int64_t;
typedef struct rae_List_rae_String rae_List_rae_String;
typedef struct rae_StringMapEntry_RaeAny rae_StringMapEntry_RaeAny;
typedef struct rae_IntMapEntry_RaeAny rae_IntMapEntry_RaeAny;
typedef struct rae_MapEntry_RaeAny rae_MapEntry_RaeAny;

typedef struct rae_List_rae_String rae_List_rae_String;
struct rae_List_rae_String {
  rae_String* data;
  int64_t length;
  int64_t capacity;
};

typedef struct rae_List_int64_t rae_List_int64_t;
struct rae_List_int64_t {
  int64_t* data;
  int64_t length;
  int64_t capacity;
};

RAE_UNUSED static const char* rae_toJson_rae_List_rae_String_(rae_List_rae_String* this);
RAE_UNUSED static rae_List_rae_String rae_fromJson_rae_List_rae_String_(const char* json);
RAE_UNUSED static void rae_log_rae_List_rae_String_(rae_List_rae_String val);
RAE_UNUSED static void rae_log_stream_rae_List_rae_String_(rae_List_rae_String val);
RAE_UNUSED static const char* rae_str_rae_List_rae_String_(rae_List_rae_String val);
RAE_UNUSED static const char* rae_toJson_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static rae_List_int64_t rae_fromJson_rae_List_int64_t_(const char* json);
RAE_UNUSED static void rae_log_rae_List_int64_t_(rae_List_int64_t val);
RAE_UNUSED static void rae_log_stream_rae_List_int64_t_(rae_List_int64_t val);
RAE_UNUSED static const char* rae_str_rae_List_int64_t_(rae_List_int64_t val);

RAE_UNUSED static const char* rae_toJson_rae_List_rae_String_(rae_List_rae_String* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_List_rae_String rae_fromJson_rae_List_rae_String_(const char* json) { (void)json; rae_List_rae_String res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_List_rae_String_(rae_List_rae_String val) { rae_log_stream_rae_List_rae_String_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_List_rae_String_(rae_List_rae_String val) {
  printf("{ #(");
  for (int64_t i = 0; i < val.capacity; i++) {
    if (i > 0) printf(", ");
    if (i < val.length) {
      rae_ext_rae_log_stream_any(rae_any(val.data[i]));
    } else {
      printf("none");
    }
  }
  printf("), %lld, %lld }", (long long)val.length, (long long)val.capacity);
}
RAE_UNUSED static const char* rae_str_rae_List_rae_String_(rae_List_rae_String val) { (void)val; return "List(...)"; }

RAE_UNUSED static const char* rae_toJson_rae_List_int64_t_(rae_List_int64_t* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_List_int64_t rae_fromJson_rae_List_int64_t_(const char* json) { (void)json; rae_List_int64_t res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_List_int64_t_(rae_List_int64_t val) { rae_log_stream_rae_List_int64_t_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_List_int64_t_(rae_List_int64_t val) {
  printf("{ #(");
  for (int64_t i = 0; i < val.capacity; i++) {
    if (i > 0) printf(", ");
    if (i < val.length) {
      rae_ext_rae_log_stream_any(rae_any(val.data[i]));
    } else {
      printf("none");
    }
  }
  printf("), %lld, %lld }", (long long)val.length, (long long)val.capacity);
}
RAE_UNUSED static const char* rae_str_rae_List_int64_t_(rae_List_int64_t val) { (void)val; return "List(...)"; }

RAE_UNUSED  int64_t rae_ext_nextTick(void);
RAE_UNUSED  int64_t rae_ext_nowMs(void);
RAE_UNUSED  int64_t rae_ext_nowNs(void);
RAE_UNUSED  void rae_ext_rae_sleep(int64_t ms);
RAE_UNUSED  double rae_ext_rae_int_to_float(int64_t i);
RAE_UNUSED  int64_t rae_ext_rae_float_to_int(double f);
RAE_UNUSED static double rae_toFloat_int64_t_(int64_t this);
RAE_UNUSED static int64_t rae_toInt_double_(double this);
RAE_UNUSED  void* rae_ext_rae_buf_alloc(int64_t size, int64_t elemSize);
RAE_UNUSED  void rae_ext_rae_buf_free(void* buf);
RAE_UNUSED  void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elemSize);
RAE_UNUSED  void rae_ext_rae_buf_set(void* buf, int64_t index, RaeAny value);
RAE_UNUSED  RaeAny rae_ext_rae_buf_get(void* buf, int64_t index);
RAE_UNUSED  int64_t rae_ext_rae_str_hash(rae_String s);
RAE_UNUSED  rae_Bool rae_ext_rae_str_eq(rae_String a, rae_String b);
RAE_UNUSED  rae_String rae_ext_rae_io_read_line(void);
RAE_UNUSED  uint32_t rae_ext_rae_io_read_char(void);
RAE_UNUSED static rae_String rae_readLine_(void);
RAE_UNUSED static uint32_t rae_readChar_(void);
RAE_UNUSED  int64_t rae_ext_rae_str_len(rae_String s);
RAE_UNUSED  int64_t rae_ext_rae_str_compare(rae_String a, rae_String b);
RAE_UNUSED  rae_String rae_ext_rae_str_concat(rae_String a, rae_String b);
RAE_UNUSED  rae_String rae_ext_rae_str_sub(rae_String s, int64_t start, int64_t len);
RAE_UNUSED  rae_Bool rae_ext_rae_str_contains(rae_String s, rae_String sub);
RAE_UNUSED  rae_Bool rae_ext_rae_str_starts_with(rae_String s, rae_String prefix);
RAE_UNUSED  rae_Bool rae_ext_rae_str_ends_with(rae_String s, rae_String suffix);
RAE_UNUSED  int64_t rae_ext_rae_str_index_of(rae_String s, rae_String sub);
RAE_UNUSED  rae_String rae_ext_rae_str_trim(rae_String s);
RAE_UNUSED  double rae_ext_rae_str_to_f64(rae_String s);
RAE_UNUSED  int64_t rae_ext_rae_str_to_i64(rae_String s);
RAE_UNUSED  rae_String rae_ext_rae_str_from_cstr(void* s);
RAE_UNUSED  void* rae_ext_rae_str_to_cstr(rae_String s);
RAE_UNUSED static rae_String rae_fromCStr_void_p_(void* s);
RAE_UNUSED static void* rae_toCStr_rae_String_(rae_String this);
RAE_UNUSED  uint32_t rae_ext_rae_str_at(rae_String s, int64_t index);
RAE_UNUSED static uint32_t rae_at_rae_String_int64_t_(rae_String this, int64_t index);
RAE_UNUSED static int64_t rae_length_rae_String_(rae_String this);
RAE_UNUSED static int64_t rae_compare_rae_String_rae_String_(rae_String this, rae_String other);
RAE_UNUSED static rae_Bool rae_equals_rae_String_rae_String_(rae_String this, rae_String other);
RAE_UNUSED static int64_t rae_hash_rae_String_(rae_String this);
RAE_UNUSED static rae_String rae_concat_rae_String_rae_String_(rae_String this, rae_String other);
RAE_UNUSED static rae_String rae_sub_rae_String_int64_t_int64_t_(rae_String this, int64_t start, int64_t len);
RAE_UNUSED static rae_Bool rae_contains_rae_String_rae_String_(rae_String this, rae_String sub);
RAE_UNUSED static rae_Bool rae_startsWith_rae_String_rae_String_(rae_String this, rae_String prefix);
RAE_UNUSED static rae_Bool rae_endsWith_rae_String_rae_String_(rae_String this, rae_String suffix);
RAE_UNUSED static int64_t rae_indexOf_rae_String_rae_String_(rae_String this, rae_String sub);
RAE_UNUSED static rae_String rae_trim_rae_String_(rae_String this);
RAE_UNUSED static rae_List_rae_String rae_split_rae_String_rae_String_(rae_String this, rae_String sep);
RAE_UNUSED static rae_String rae_replace_rae_String_rae_String_rae_String_(rae_String this, rae_String old, rae_String new);
RAE_UNUSED static rae_String rae_join_rae_List_rae_String_rae_String_(rae_List_rae_String* this, rae_String sep);
RAE_UNUSED static double rae_toFloat_rae_String_(rae_String this);
RAE_UNUSED static int64_t rae_toInt_rae_String_(rae_String this);
int main(void);
RAE_UNUSED static rae_List_rae_String rae_createList_rae_String_int64_t_(int64_t initialCap);
RAE_UNUSED static void rae_add_rae_String_rae_List_rae_String_rae_String_(rae_List_rae_String* this, rae_String value);
RAE_UNUSED static rae_String rae_get_rae_String_rae_List_rae_String_int64_t_(rae_List_rae_String* this, int64_t index);
RAE_UNUSED static int64_t rae_sizeof_rae_String_(void);
RAE_UNUSED static void rae_grow_rae_String_rae_List_rae_String_(rae_List_rae_String* this);

RAE_UNUSED static double rae_toFloat_int64_t_(int64_t this) {
  {
  return rae_ext_rae_int_to_float(this);
  }
}

RAE_UNUSED static int64_t rae_toInt_double_(double this) {
  {
  return rae_ext_rae_float_to_int(this);
  }
}

RAE_UNUSED static rae_String rae_readLine_(void) {
  {
  return rae_ext_rae_io_read_line();
  }
}

RAE_UNUSED static uint32_t rae_readChar_(void) {
  {
  return rae_ext_rae_io_read_char();
  }
}

RAE_UNUSED static rae_String rae_fromCStr_void_p_(void* s) {
  {
  return rae_ext_rae_str_from_cstr((*s));
  }
}

RAE_UNUSED static void* rae_toCStr_rae_String_(rae_String this) {
  {
  return rae_ext_rae_str_to_cstr(this);
  }
}

RAE_UNUSED static uint32_t rae_at_rae_String_int64_t_(rae_String this, int64_t index) {
  {
  return rae_ext_rae_str_at(this, index);
  }
}

RAE_UNUSED static int64_t rae_length_rae_String_(rae_String this) {
  {
  return rae_ext_rae_str_len(this);
  }
}

RAE_UNUSED static int64_t rae_compare_rae_String_rae_String_(rae_String this, rae_String other) {
  {
  return rae_ext_rae_str_compare(this, other);
  }
}

RAE_UNUSED static rae_Bool rae_equals_rae_String_rae_String_(rae_String this, rae_String other) {
  {
  return rae_ext_rae_str_eq(this, other);
  }
}

RAE_UNUSED static int64_t rae_hash_rae_String_(rae_String this) {
  {
  return rae_ext_rae_str_hash(this);
  }
}

RAE_UNUSED static rae_String rae_concat_rae_String_rae_String_(rae_String this, rae_String other) {
  {
  return rae_ext_rae_str_concat(this, other);
  }
}

RAE_UNUSED static rae_String rae_sub_rae_String_int64_t_int64_t_(rae_String this, int64_t start, int64_t len) {
  {
  return rae_ext_rae_str_sub(this, start, len);
  }
}

RAE_UNUSED static rae_Bool rae_contains_rae_String_rae_String_(rae_String this, rae_String sub) {
  {
  return rae_ext_rae_str_contains(this, sub);
  }
}

RAE_UNUSED static rae_Bool rae_startsWith_rae_String_rae_String_(rae_String this, rae_String prefix) {
  {
  return rae_ext_rae_str_starts_with(this, prefix);
  }
}

RAE_UNUSED static rae_Bool rae_endsWith_rae_String_rae_String_(rae_String this, rae_String suffix) {
  {
  return rae_ext_rae_str_ends_with(this, suffix);
  }
}

RAE_UNUSED static int64_t rae_indexOf_rae_String_rae_String_(rae_String this, rae_String sub) {
  {
  return rae_ext_rae_str_index_of(this, sub);
  }
}

RAE_UNUSED static rae_String rae_trim_rae_String_(rae_String this) {
  {
  return rae_ext_rae_str_trim(this);
  }
}

RAE_UNUSED static rae_List_rae_String rae_split_rae_String_rae_String_(rae_String this, rae_String sep) {
  {
  rae_List_rae_String result = rae_createList_rae_String_int64_t_(((int64_t)4LL));
  if ((bool)(rae_length_rae_String_(sep) == ((int64_t)0LL))) {
  {
  rae_add_rae_String_rae_List_rae_String_rae_String_(&(result), this);
  return result;
  }
  }
  rae_String remaining = this;
  {
  while ((bool)true) {
  {
  int64_t idx = rae_indexOf_rae_String_rae_String_(remaining, sep);
  if ((bool)(idx == -(((int64_t)1LL)))) {
  {
  rae_add_rae_String_rae_List_rae_String_rae_String_(&(result), remaining);
  return result;
  }
  }
  rae_String part = rae_sub_rae_String_int64_t_int64_t_(remaining, ((int64_t)0LL), idx);
  rae_add_rae_String_rae_List_rae_String_rae_String_(&(result), part);
  remaining = rae_sub_rae_String_int64_t_int64_t_(remaining, idx + rae_length_rae_String_(sep), rae_length_rae_String_(remaining) - idx - rae_length_rae_String_(sep));
  }
  }
  }
  return result;
  }
}

RAE_UNUSED static rae_String rae_replace_rae_String_rae_String_rae_String_(rae_String this, rae_String old, rae_String new) {
  {
  if ((bool)(rae_length_rae_String_(old) == ((int64_t)0LL))) {
  {
  return this;
  }
  }
  rae_List_rae_String parts = rae_split_rae_String_rae_String_(this, old);
  return rae_join_rae_List_rae_String_rae_String_(&(parts), new);
  }
}

RAE_UNUSED static rae_String rae_join_rae_List_rae_String_rae_String_(rae_List_rae_String* this, rae_String sep) {
  {
  if ((bool)(this->length == ((int64_t)0LL))) {
  {
  return (rae_String){(uint8_t*)"", 0};
  }
  }
  rae_String result = rae_get_rae_String_rae_List_rae_String_int64_t_(this, ((int64_t)0LL));
  int64_t i = ((int64_t)1LL);
  {
  while ((bool)(i < this->length)) {
  {
  result = rae_concat_rae_String_rae_String_(rae_concat_rae_String_rae_String_(result, sep), rae_get_rae_String_rae_List_rae_String_int64_t_(this, i));
  i = i + ((int64_t)1LL);
  }
  }
  }
  return result;
  }
}

RAE_UNUSED static double rae_toFloat_rae_String_(rae_String this) {
  {
  return rae_ext_rae_str_to_f64(this);
  }
}

RAE_UNUSED static int64_t rae_toInt_rae_String_(rae_String this) {
  {
  return rae_ext_rae_str_to_i64(this);
  }
}

int main(void) {
  {
  rae_String s = (rae_String){(uint8_t*)"Hello \xf0\x9f\x92\x9c", 10};
  rae_ext_rae_log_any(rae_any((rae_length_rae_String_(s))));
  rae_ext_rae_log_any(rae_any((rae_at_rae_String_int64_t_(s, ((int64_t)0LL)))));
  rae_ext_rae_log_any(rae_any((rae_at_rae_String_int64_t_(s, ((int64_t)6LL)))));
  uint32_t c = rae_at_rae_String_int64_t_(s, ((int64_t)6LL));
  rae_ext_rae_log_any(rae_any((c)));
  rae_String s2 = rae_fromCStr_void_p_(rae_toCStr_rae_String_(s));
  rae_ext_rae_log_any(rae_any((s2)));
  rae_ext_rae_log_any(rae_any((rae_equals_rae_String_rae_String_(s, s2))));
  rae_List_rae_String l = rae_createList_rae_String_int64_t_(((int64_t)1LL));
  rae_add_rae_String_rae_List_rae_String_rae_String_(&(l), (rae_String){(uint8_t*)"test", 4});
  rae_ext_rae_log_any(rae_any((l.length)));
  }
  return 0;
}

RAE_UNUSED static rae_List_rae_String rae_createList_rae_String_int64_t_(int64_t initialCap) {
  {
  return (rae_List_rae_String){ .data = (void*)rae_ext_rae_buf_alloc(initialCap, sizeof(rae_String)), .length = ((int64_t)0LL), .capacity = initialCap };
  }
}

RAE_UNUSED static void rae_add_rae_String_rae_List_rae_String_rae_String_(rae_List_rae_String* this, rae_String value) {
  {
  if ((bool)(this->length == this->capacity)) {
  {
  rae_grow_rae_String_rae_List_rae_String_(this);
  }
  }
  this->data[this->length] = value;
  this->length = this->length + ((int64_t)1LL);
  }
}

RAE_UNUSED static rae_String rae_get_rae_String_rae_List_rae_String_int64_t_(rae_List_rae_String* this, int64_t index) {
  {
  if ((bool)((bool)(index < ((int64_t)0LL)) || (bool)(index >= this->length))) {
  {
  return (rae_String){0};
  }
  }
  return this->data[index];
  }
}

RAE_UNUSED static void rae_grow_rae_String_rae_List_rae_String_(rae_List_rae_String* this) {
  {
  int64_t newCap = this->capacity * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  {
  newCap = ((int64_t)4LL);
  }
  }
  rae_String* newData = (void*)rae_ext_rae_buf_alloc(newCap, sizeof(rae_String));
  rae_ext_rae_buf_copy((void*)(this->data), ((int64_t)0LL), (void*)(newData), ((int64_t)0LL), this->length, sizeof(rae_String));
  rae_ext_rae_buf_free((void*)(this->data));
  this->data = newData;
  this->capacity = newCap;
  }
}

