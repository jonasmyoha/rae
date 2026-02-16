#include "rae_runtime.h"

typedef int8_t rae_Bool;
typedef const char* const_char_p;
#define LIFT_TO_TEMP(T, ...) (&(T){__VA_ARGS__})
#define LIFT_VALUE(T, ...) ((T){__VA_ARGS__})

// Built-in buffer functions
RAE_UNUSED void* rae_ext_rae_buf_alloc(int64_t size, int64_t elemSize);
RAE_UNUSED void rae_ext_rae_buf_free(void* ptr);
RAE_UNUSED void* rae_ext_rae_buf_resize(void* ptr, int64_t new_size, int64_t elemSize);
RAE_UNUSED void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elemSize);

// Forward declarations for specialized generics
typedef struct rae_List_const_char_p rae_List_const_char_p;
typedef struct rae_List_int64_t rae_List_int64_t;
typedef struct rae_StringMapEntry_RaeAny rae_StringMapEntry_RaeAny;
typedef struct rae_IntMapEntry_RaeAny rae_IntMapEntry_RaeAny;
typedef struct rae_MapEntry_RaeAny rae_MapEntry_RaeAny;

typedef struct rae_List_const_char_p rae_List_const_char_p;
struct rae_List_const_char_p {
  const char** data;
  int64_t length;
  int64_t capacity;
};

typedef struct rae_List_int64_t rae_List_int64_t;
struct rae_List_int64_t {
  int64_t* data;
  int64_t length;
  int64_t capacity;
};

RAE_UNUSED static const char* rae_toJson_rae_List_const_char_p_(rae_List_const_char_p* this);
RAE_UNUSED static rae_List_const_char_p rae_fromJson_rae_List_const_char_p_(const char* json);
RAE_UNUSED static void rae_log_rae_List_const_char_p_(rae_List_const_char_p val);
RAE_UNUSED static void rae_log_stream_rae_List_const_char_p_(rae_List_const_char_p val);
RAE_UNUSED static const char* rae_str_rae_List_const_char_p_(rae_List_const_char_p val);
RAE_UNUSED static const char* rae_toJson_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static rae_List_int64_t rae_fromJson_rae_List_int64_t_(const char* json);
RAE_UNUSED static void rae_log_rae_List_int64_t_(rae_List_int64_t val);
RAE_UNUSED static void rae_log_stream_rae_List_int64_t_(rae_List_int64_t val);
RAE_UNUSED static const char* rae_str_rae_List_int64_t_(rae_List_int64_t val);

RAE_UNUSED static const char* rae_toJson_rae_List_const_char_p_(rae_List_const_char_p* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_List_const_char_p rae_fromJson_rae_List_const_char_p_(const char* json) { (void)json; rae_List_const_char_p res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_List_const_char_p_(rae_List_const_char_p val) { rae_log_stream_rae_List_const_char_p_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_List_const_char_p_(rae_List_const_char_p val) {
  printf("{ #(");
  for (int64_t i = 0; i < val.length; i++) {
    if (i > 0) printf(", ");
    rae_ext_rae_log_stream_any(rae_any(val.data[i]));
  }
  printf("), %lld, %lld }", (long long)val.length, (long long)val.capacity);
}
RAE_UNUSED static const char* rae_str_rae_List_const_char_p_(rae_List_const_char_p val) { (void)val; return "List(...)"; }

RAE_UNUSED static const char* rae_toJson_rae_List_int64_t_(rae_List_int64_t* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_List_int64_t rae_fromJson_rae_List_int64_t_(const char* json) { (void)json; rae_List_int64_t res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_List_int64_t_(rae_List_int64_t val) { rae_log_stream_rae_List_int64_t_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_List_int64_t_(rae_List_int64_t val) {
  printf("{ #(");
  for (int64_t i = 0; i < val.length; i++) {
    if (i > 0) printf(", ");
    rae_ext_rae_log_stream_any(rae_any(val.data[i]));
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
RAE_UNUSED  void* rae_ext_rae_ext_rae_buf_alloc(int64_t size, int64_t elemSize);
RAE_UNUSED  void rae_ext_rae_ext_rae_buf_free(RaeAny* buf);
RAE_UNUSED  void rae_ext_rae_ext_rae_buf_copy(RaeAny* src, int64_t src_off, RaeAny* dst, int64_t dst_off, int64_t len, int64_t elemSize);
RAE_UNUSED  void rae_ext_rae_ext_rae_buf_set(RaeAny* buf, int64_t index, RaeAny value);
RAE_UNUSED  RaeAny rae_ext_rae_ext_rae_buf_get(RaeAny* buf, int64_t index);
RAE_UNUSED  int64_t rae_ext_rae_str_hash(const char* s);
RAE_UNUSED  rae_Bool rae_ext_rae_str_eq(const char* a, const char* b);
RAE_UNUSED  const char* rae_ext_rae_io_read_line(void);
RAE_UNUSED  rae_Char rae_ext_rae_io_read_char(void);
RAE_UNUSED static const char* rae_readLine_(void);
RAE_UNUSED static rae_Char rae_readChar_(void);
RAE_UNUSED static void rae_add_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value);
RAE_UNUSED static void rae_grow_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static int64_t rae_sizeof_(void);

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

RAE_UNUSED static const char* rae_readLine_(void) {
  {
  return rae_ext_rae_io_read_line();
  }
}

RAE_UNUSED static rae_Char rae_readChar_(void) {
  {
  return rae_ext_rae_io_read_char();
  }
}

int main(void) {
  {
  rae_List_int64_t x = (rae_List_int64_t){ .data = (int64_t*)LIFT_TO_TEMP(int64_t[], ((int64_t)10LL), ((int64_t)20LL)), .length = ((int64_t)2LL), .capacity = ((int64_t)2LL) };
  rae_add_rae_List_int64_t_int64_t_(&(x), ((int64_t)30LL));
  rae_log_rae_List_int64_t_(x);
  }
  return 0;
}

RAE_UNUSED static void rae_add_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value) {
  {
  if ((bool)(this->length == this->capacity)) {
  {
  rae_grow_rae_List_int64_t_(this);
  }
  }
  this->data[this->length] = value;
  this->length = this->length + ((int64_t)1LL);
  }
}

RAE_UNUSED static void rae_grow_rae_List_int64_t_(rae_List_int64_t* this) {
  {
  int64_t newCap = this->capacity * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  {
  newCap = ((int64_t)4LL);
  }
  }
  int64_t* newData = (int64_t*)(void*)rae_ext_rae_buf_alloc(newCap, sizeof(int64_t));
  rae_ext_rae_buf_copy((void*)(this->data), ((int64_t)0LL), (void*)(newData), ((int64_t)0LL), this->length, sizeof(int64_t));
  rae_ext_rae_buf_free((void*)(this->data));
  this->data = newData;
  this->capacity = newCap;
  }
}

RAE_UNUSED static int64_t rae_sizeof_(void) {
}

