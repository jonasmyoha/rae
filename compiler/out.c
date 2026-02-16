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
typedef struct rae_StringMap_int64_t rae_StringMap_int64_t;
typedef struct rae_StringMapEntry_int64_t rae_StringMapEntry_int64_t;
typedef struct rae_IntMap_rae_String rae_IntMap_rae_String;
typedef struct rae_IntMapEntry_rae_String rae_IntMapEntry_rae_String;
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

typedef struct rae_StringMap_int64_t rae_StringMap_int64_t;
struct rae_StringMap_int64_t {
  rae_StringMapEntry_int64_t* data;
  int64_t length;
  int64_t capacity;
};

typedef struct rae_StringMapEntry_int64_t rae_StringMapEntry_int64_t;
struct rae_StringMapEntry_int64_t {
  rae_String k;
  int64_t value;
  rae_Bool occupied;
};

typedef struct rae_IntMap_rae_String rae_IntMap_rae_String;
struct rae_IntMap_rae_String {
  rae_IntMapEntry_rae_String* data;
  int64_t length;
  int64_t capacity;
};

typedef struct rae_IntMapEntry_rae_String rae_IntMapEntry_rae_String;
struct rae_IntMapEntry_rae_String {
  int64_t k;
  rae_String value;
  rae_Bool occupied;
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
RAE_UNUSED static const char* rae_toJson_rae_StringMap_int64_t_(rae_StringMap_int64_t* this);
RAE_UNUSED static rae_StringMap_int64_t rae_fromJson_rae_StringMap_int64_t_(const char* json);
RAE_UNUSED static void rae_log_rae_StringMap_int64_t_(rae_StringMap_int64_t val);
RAE_UNUSED static void rae_log_stream_rae_StringMap_int64_t_(rae_StringMap_int64_t val);
RAE_UNUSED static const char* rae_str_rae_StringMap_int64_t_(rae_StringMap_int64_t val);
RAE_UNUSED static const char* rae_toJson_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t* this);
RAE_UNUSED static rae_StringMapEntry_int64_t rae_fromJson_rae_StringMapEntry_int64_t_(const char* json);
RAE_UNUSED static void rae_log_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t val);
RAE_UNUSED static void rae_log_stream_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t val);
RAE_UNUSED static const char* rae_str_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t val);
RAE_UNUSED static const char* rae_toJson_rae_IntMap_rae_String_(rae_IntMap_rae_String* this);
RAE_UNUSED static rae_IntMap_rae_String rae_fromJson_rae_IntMap_rae_String_(const char* json);
RAE_UNUSED static void rae_log_rae_IntMap_rae_String_(rae_IntMap_rae_String val);
RAE_UNUSED static void rae_log_stream_rae_IntMap_rae_String_(rae_IntMap_rae_String val);
RAE_UNUSED static const char* rae_str_rae_IntMap_rae_String_(rae_IntMap_rae_String val);
RAE_UNUSED static const char* rae_toJson_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String* this);
RAE_UNUSED static rae_IntMapEntry_rae_String rae_fromJson_rae_IntMapEntry_rae_String_(const char* json);
RAE_UNUSED static void rae_log_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String val);
RAE_UNUSED static void rae_log_stream_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String val);
RAE_UNUSED static const char* rae_str_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String val);

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

RAE_UNUSED static const char* rae_toJson_rae_StringMap_int64_t_(rae_StringMap_int64_t* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_StringMap_int64_t rae_fromJson_rae_StringMap_int64_t_(const char* json) { (void)json; rae_StringMap_int64_t res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_StringMap_int64_t_(rae_StringMap_int64_t val) { rae_log_stream_rae_StringMap_int64_t_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_StringMap_int64_t_(rae_StringMap_int64_t val) { (void)val; printf("StringMap(...)"); }
RAE_UNUSED static const char* rae_str_rae_StringMap_int64_t_(rae_StringMap_int64_t val) { (void)val; return "StringMap(...)"; }

RAE_UNUSED static const char* rae_toJson_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_StringMapEntry_int64_t rae_fromJson_rae_StringMapEntry_int64_t_(const char* json) { (void)json; rae_StringMapEntry_int64_t res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t val) { rae_log_stream_rae_StringMapEntry_int64_t_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t val) { (void)val; printf("StringMapEntry(...)"); }
RAE_UNUSED static const char* rae_str_rae_StringMapEntry_int64_t_(rae_StringMapEntry_int64_t val) { (void)val; return "StringMapEntry(...)"; }

RAE_UNUSED static const char* rae_toJson_rae_IntMap_rae_String_(rae_IntMap_rae_String* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_IntMap_rae_String rae_fromJson_rae_IntMap_rae_String_(const char* json) { (void)json; rae_IntMap_rae_String res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_IntMap_rae_String_(rae_IntMap_rae_String val) { rae_log_stream_rae_IntMap_rae_String_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_IntMap_rae_String_(rae_IntMap_rae_String val) { (void)val; printf("IntMap(...)"); }
RAE_UNUSED static const char* rae_str_rae_IntMap_rae_String_(rae_IntMap_rae_String val) { (void)val; return "IntMap(...)"; }

RAE_UNUSED static const char* rae_toJson_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_IntMapEntry_rae_String rae_fromJson_rae_IntMapEntry_rae_String_(const char* json) { (void)json; rae_IntMapEntry_rae_String res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String val) { rae_log_stream_rae_IntMapEntry_rae_String_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String val) { (void)val; printf("IntMapEntry(...)"); }
RAE_UNUSED static const char* rae_str_rae_IntMapEntry_rae_String_(rae_IntMapEntry_rae_String val) { (void)val; return "IntMapEntry(...)"; }

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
int main(void);
RAE_UNUSED static rae_StringMap_int64_t rae_createStringMap_int64_t_int64_t_(int64_t initialCap);
RAE_UNUSED static void rae_set_int64_t_rae_StringMap_int64_t_rae_String_int64_t_(rae_StringMap_int64_t* this, rae_String k, int64_t value);
RAE_UNUSED static RaeAny rae_get_int64_t_rae_StringMap_int64_t_rae_String_(rae_StringMap_int64_t* this, rae_String k);
RAE_UNUSED static rae_Bool rae_has_int64_t_rae_StringMap_int64_t_rae_String_(rae_StringMap_int64_t* this, rae_String k);
RAE_UNUSED static rae_IntMap_rae_String rae_createIntMap_rae_String_int64_t_(int64_t initialCap);
RAE_UNUSED static void rae_set_rae_String_rae_IntMap_rae_String_int64_t_rae_String_(rae_IntMap_rae_String* this, int64_t k, rae_String value);
RAE_UNUSED static RaeAny rae_get_rae_String_rae_IntMap_rae_String_int64_t_(rae_IntMap_rae_String* this, int64_t k);
RAE_UNUSED static int64_t rae_sizeof_int64_t_(void);
RAE_UNUSED static void rae_growStringMap_int64_t_rae_StringMap_int64_t_(rae_StringMap_int64_t* this);
RAE_UNUSED static rae_IntMap_rae_String rae_createInt64Map_rae_String_int64_t_(int64_t initialCap);
RAE_UNUSED static int64_t rae_sizeof_rae_String_(void);
RAE_UNUSED static void rae_growInt64Map_rae_String_rae_IntMap_rae_String_(rae_IntMap_rae_String* this);
RAE_UNUSED static void rae_set_int64_t_rae_List_int64_t_int64_t_int64_t_(rae_List_int64_t* this, int64_t index, int64_t value);
RAE_UNUSED static void rae_set_rae_String_rae_List_rae_String_int64_t_rae_String_(rae_List_rae_String* this, int64_t index, rae_String value);

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

int main(void) {
  {
  rae_ext_rae_log_any(rae_any(((rae_String){(uint8_t*)"StringMap test", 14})));
  rae_StringMap_int64_t m = rae_createStringMap_int64_t_(((int64_t)4LL));
  rae_set_int64_t_rae_StringMap_int64_t_rae_String_int64_t_(&(m), (rae_String){(uint8_t*)"apple", 5}, ((int64_t)10LL));
  rae_set_int64_t_rae_StringMap_int64_t_rae_String_int64_t_(&(m), (rae_String){(uint8_t*)"banana", 6}, ((int64_t)20LL));
  rae_set_int64_t_rae_StringMap_int64_t_rae_String_int64_t_(&(m), (rae_String){(uint8_t*)"cherry", 6}, ((int64_t)30LL));
  rae_String k1 = (rae_String){(uint8_t*)"apple", 5};
  rae_String k2 = (rae_String){(uint8_t*)"banana", 6};
  rae_String k3 = (rae_String){(uint8_t*)"cherry", 6};
  rae_String k4 = (rae_String){(uint8_t*)"durian", 6};
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"apple: ", 7}))), rae_ext_rae_log_stream_any(rae_get_int64_t_rae_StringMap_int64_t_rae_String_(&(m), k1)), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"banana: ", 8}))), rae_ext_rae_log_stream_any(rae_get_int64_t_rae_StringMap_int64_t_rae_String_(&(m), k2)), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"cherry: ", 8}))), rae_ext_rae_log_stream_any(rae_get_int64_t_rae_StringMap_int64_t_rae_String_(&(m), k3)), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"missing: ", 9}))), rae_ext_rae_log_stream_any(rae_get_int64_t_rae_StringMap_int64_t_rae_String_(&(m), k4)), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"has banana: ", 12}))), rae_ext_rae_log_stream_any(rae_any((rae_has_int64_t_rae_StringMap_int64_t_rae_String_(&(m), k2)))), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"has durian: ", 12}))), rae_ext_rae_log_stream_any(rae_any((rae_has_int64_t_rae_StringMap_int64_t_rae_String_(&(m), k4)))), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"length: ", 8}))), rae_ext_rae_log_stream_any(rae_any((m.length))), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_any(rae_any(((rae_String){(uint8_t*)"IntMap test", 11})));
  rae_IntMap_rae_String m2 = rae_createIntMap_int64_t_(((int64_t)4LL));
  rae_set_rae_String_rae_IntMap_rae_String_int64_t_rae_String_(&(m2), ((int64_t)1LL), (rae_String){(uint8_t*)"one", 3});
  rae_set_rae_String_rae_IntMap_rae_String_int64_t_rae_String_(&(m2), ((int64_t)2LL), (rae_String){(uint8_t*)"two", 3});
  rae_set_rae_String_rae_IntMap_rae_String_int64_t_rae_String_(&(m2), ((int64_t)10LL), (rae_String){(uint8_t*)"ten", 3});
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"1: ", 3}))), rae_ext_rae_log_stream_any(rae_get_rae_String_rae_IntMap_rae_String_int64_t_(&(m2), ((int64_t)1LL))), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"2: ", 3}))), rae_ext_rae_log_stream_any(rae_get_rae_String_rae_IntMap_rae_String_int64_t_(&(m2), ((int64_t)2LL))), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"10: ", 4}))), rae_ext_rae_log_stream_any(rae_get_rae_String_rae_IntMap_rae_String_int64_t_(&(m2), ((int64_t)10LL))), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"length: ", 8}))), rae_ext_rae_log_stream_any(rae_any((m2.length))), rae_ext_rae_log_stream_any(rae_any(((rae_String){(uint8_t*)"", 0}))), rae_ext_rae_log_cstr("");
  }
  return 0;
}

RAE_UNUSED static rae_StringMap_int64_t rae_createStringMap_int64_t_int64_t_(int64_t initialCap) {
  {
  return (rae_StringMap_int64_t){ .data = (void*)rae_ext_rae_buf_alloc(initialCap, sizeof(int64_t)), .length = ((int64_t)0LL), .capacity = initialCap };
  }
}

RAE_UNUSED static void rae_set_int64_t_rae_StringMap_int64_t_rae_String_int64_t_(rae_StringMap_int64_t* this, rae_String k, int64_t value) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  this->data = (void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  }
  }
  if ((bool)(this->length * ((int64_t)2LL) > this->capacity)) {
  {
  rae_growStringMap_int64_t_rae_StringMap_int64_t_(this);
  }
  }
  int64_t h = rae_ext_rae_str_hash(k);
  int64_t idx = h % this->capacity;
  if ((bool)(idx < ((int64_t)0LL))) {
  {
  idx = -(idx);
  }
  }
  {
  while ((bool)true) {
  {
  rae_StringMapEntry_int64_t entry = this->data[idx];
  if (((bool)!(entry.occupied))) {
  {
  this->data[idx] = (rae_StringMapEntry_int64_t){ .k = k, .value = value, .occupied = (bool)true };
  this->length = this->length + ((int64_t)1LL);
  return;
  }
  }
  if (rae_ext_rae_str_eq(entry.k, k)) {
  {
  entry.value = value;
  this->data[idx] = entry;
  return;
  }
  }
  idx = (idx + ((int64_t)1LL)) % this->capacity;
  }
  }
  }
  }
}

RAE_UNUSED static RaeAny rae_get_int64_t_rae_StringMap_int64_t_rae_String_(rae_StringMap_int64_t* this, rae_String k) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  return rae_any(rae_any_none());
  }
  }
  int64_t h = rae_ext_rae_str_hash(k);
  int64_t idx = h % this->capacity;
  if ((bool)(idx < ((int64_t)0LL))) {
  {
  idx = -(idx);
  }
  }
  int64_t startIdx = idx;
  {
  while ((bool)true) {
  {
  rae_StringMapEntry_int64_t entry = this->data[idx];
  if (((bool)!(entry.occupied))) {
  {
  return rae_any(rae_any_none());
  }
  }
  if (rae_ext_rae_str_eq(entry.k, k)) {
  {
  return rae_any(entry.value);
  }
  }
  idx = (idx + ((int64_t)1LL)) % this->capacity;
  if ((bool)(idx == startIdx)) {
  {
  return rae_any(rae_any_none());
  }
  }
  }
  }
  }
  }
}

RAE_UNUSED static rae_Bool rae_has_int64_t_rae_StringMap_int64_t_rae_String_(rae_StringMap_int64_t* this, rae_String k) {
  {
  RaeAny __m0 = rae_get_int64_t_rae_StringMap_int64_t_rae_String_(this, k);
  if (rae_any_eq(__m0, rae_any_none())) {
  {
  return (bool)false;
  }
  } else {
  {
  return (bool)true;
  }
  }
  }
}

RAE_UNUSED static rae_IntMap_rae_String rae_createIntMap_rae_String_int64_t_(int64_t initialCap) {
  {
  return rae_createInt64Map_rae_String_int64_t_(initialCap);
  }
}

RAE_UNUSED static void rae_set_rae_String_rae_IntMap_rae_String_int64_t_rae_String_(rae_IntMap_rae_String* this, int64_t k, rae_String value) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  this->data = (void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  }
  }
  if ((bool)(this->length * ((int64_t)2LL) > this->capacity)) {
  {
  rae_growInt64Map_rae_String_rae_IntMap_rae_String_(this);
  }
  }
  int64_t h = k;
  int64_t idx = h % this->capacity;
  if ((bool)(idx < ((int64_t)0LL))) {
  {
  idx = -(idx);
  }
  }
  {
  while ((bool)true) {
  {
  rae_IntMapEntry_rae_String entry = this->data[idx];
  if (((bool)!(entry.occupied))) {
  {
  this->data[idx] = (rae_IntMapEntry_rae_String){ .k = k, .value = value, .occupied = (bool)true };
  this->length = this->length + ((int64_t)1LL);
  return;
  }
  }
  if ((bool)(entry.k == k)) {
  {
  entry.value = value;
  this->data[idx] = entry;
  return;
  }
  }
  idx = (idx + ((int64_t)1LL)) % this->capacity;
  }
  }
  }
  }
}

RAE_UNUSED static RaeAny rae_get_rae_String_rae_IntMap_rae_String_int64_t_(rae_IntMap_rae_String* this, int64_t k) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  return rae_any((rae_String){0});
  }
  }
  int64_t h = k;
  int64_t idx = h % this->capacity;
  if ((bool)(idx < ((int64_t)0LL))) {
  {
  idx = -(idx);
  }
  }
  int64_t startIdx = idx;
  {
  while ((bool)true) {
  {
  rae_IntMapEntry_rae_String entry = this->data[idx];
  if (((bool)!(entry.occupied))) {
  {
  return rae_any((rae_String){0});
  }
  }
  if ((bool)(entry.k == k)) {
  {
  return rae_any(entry.value);
  }
  }
  idx = (idx + ((int64_t)1LL)) % this->capacity;
  if ((bool)(idx == startIdx)) {
  {
  return rae_any((rae_String){0});
  }
  }
  }
  }
  }
  }
}

RAE_UNUSED static void rae_growStringMap_int64_t_rae_StringMap_int64_t_(rae_StringMap_int64_t* this) {
  {
  int64_t oldCap = this->capacity;
  rae_StringMapEntry_int64_t* oldData = this->data;
  this->capacity = oldCap * ((int64_t)2LL);
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  }
  }
  this->data = (void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  this->length = ((int64_t)0LL);
  int64_t i = ((int64_t)0LL);
  {
  while ((bool)(i < oldCap)) {
  {
  rae_StringMapEntry_int64_t entry = oldData[i];
  if (entry.occupied) {
  {
  rae_set_int64_t_rae_StringMap_int64_t_rae_String_int64_t_(this, entry.k, entry.value);
  }
  }
  i = i + ((int64_t)1LL);
  }
  }
  }
  rae_ext_rae_buf_free((void*)(oldData));
  }
}

RAE_UNUSED static rae_IntMap_rae_String rae_createInt64Map_rae_String_int64_t_(int64_t initialCap) {
  {
  return (rae_IntMap_rae_String){ .data = (void*)rae_ext_rae_buf_alloc(initialCap, sizeof(int64_t)), .length = ((int64_t)0LL), .capacity = initialCap };
  }
}

RAE_UNUSED static void rae_growInt64Map_rae_String_rae_IntMap_rae_String_(rae_IntMap_rae_String* this) {
  {
  int64_t oldCap = this->capacity;
  rae_IntMapEntry_rae_String* oldData = this->data;
  this->capacity = oldCap * ((int64_t)2LL);
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  }
  }
  this->data = (void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  this->length = ((int64_t)0LL);
  int64_t i = ((int64_t)0LL);
  {
  while ((bool)(i < oldCap)) {
  {
  rae_IntMapEntry_rae_String entry = oldData[i];
  if (entry.occupied) {
  {
  rae_set_rae_String_rae_IntMap_rae_String_int64_t_rae_String_(this, entry.k, entry.value);
  }
  }
  i = i + ((int64_t)1LL);
  }
  }
  }
  rae_ext_rae_buf_free((void*)(oldData));
  }
}

RAE_UNUSED static void rae_set_int64_t_rae_List_int64_t_int64_t_int64_t_(rae_List_int64_t* this, int64_t index, int64_t value) {
  {
  if ((bool)((bool)(index < ((int64_t)0LL)) || (bool)(index >= this->length))) {
  {
  return;
  }
  }
  this->data[index] = value;
  }
}

RAE_UNUSED static void rae_set_rae_String_rae_List_rae_String_int64_t_rae_String_(rae_List_rae_String* this, int64_t index, rae_String value) {
  {
  if ((bool)((bool)(index < ((int64_t)0LL)) || (bool)(index >= this->length))) {
  {
  return;
  }
  }
  this->data[index] = value;
  }
}

