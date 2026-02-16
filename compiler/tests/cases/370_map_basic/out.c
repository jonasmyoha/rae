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
typedef struct rae_StringMap_int64_t rae_StringMap_int64_t;
typedef struct rae_StringMapEntry_int64_t rae_StringMapEntry_int64_t;
typedef struct rae_IntMap_const_char_p rae_IntMap_const_char_p;
typedef struct rae_IntMapEntry_const_char_p rae_IntMapEntry_const_char_p;
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

typedef struct rae_StringMapEntry_int64_t rae_StringMapEntry_int64_t;
struct rae_StringMapEntry_int64_t {
  const char* k;
  int64_t value;
  bool occupied;
};

typedef struct rae_StringMap_int64_t rae_StringMap_int64_t;
struct rae_StringMap_int64_t {
  rae_StringMapEntry_int64_t* data;
  int64_t length;
  int64_t capacity;
};

typedef struct rae_IntMap_const_char_p rae_IntMap_const_char_p;
struct rae_IntMap_const_char_p {
  rae_IntMapEntry_const_char_p* data;
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
RAE_UNUSED static const char* rae_toJson_rae_IntMap_const_char_p_(rae_IntMap_const_char_p* this);
RAE_UNUSED static rae_IntMap_const_char_p rae_fromJson_rae_IntMap_const_char_p_(const char* json);
RAE_UNUSED static void rae_log_rae_IntMap_const_char_p_(rae_IntMap_const_char_p val);
RAE_UNUSED static void rae_log_stream_rae_IntMap_const_char_p_(rae_IntMap_const_char_p val);
RAE_UNUSED static const char* rae_str_rae_IntMap_const_char_p_(rae_IntMap_const_char_p val);
RAE_UNUSED static const char* rae_toJson_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p* this);
RAE_UNUSED static rae_IntMapEntry_const_char_p rae_fromJson_rae_IntMapEntry_const_char_p_(const char* json);
RAE_UNUSED static void rae_log_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p val);
RAE_UNUSED static void rae_log_stream_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p val);
RAE_UNUSED static const char* rae_str_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p val);

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

RAE_UNUSED static const char* rae_toJson_rae_IntMap_const_char_p_(rae_IntMap_const_char_p* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_IntMap_const_char_p rae_fromJson_rae_IntMap_const_char_p_(const char* json) { (void)json; rae_IntMap_const_char_p res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_IntMap_const_char_p_(rae_IntMap_const_char_p val) { rae_log_stream_rae_IntMap_const_char_p_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_IntMap_const_char_p_(rae_IntMap_const_char_p val) { (void)val; printf("IntMap(...)"); }
RAE_UNUSED static const char* rae_str_rae_IntMap_const_char_p_(rae_IntMap_const_char_p val) { (void)val; return "IntMap(...)"; }

RAE_UNUSED static const char* rae_toJson_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_IntMapEntry_const_char_p rae_fromJson_rae_IntMapEntry_const_char_p_(const char* json) { (void)json; rae_IntMapEntry_const_char_p res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p val) { rae_log_stream_rae_IntMapEntry_const_char_p_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p val) { (void)val; printf("IntMapEntry(...)"); }
RAE_UNUSED static const char* rae_str_rae_IntMapEntry_const_char_p_(rae_IntMapEntry_const_char_p val) { (void)val; return "IntMapEntry(...)"; }

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
RAE_UNUSED static rae_StringMap_int64_t rae_createStringMap_int64_t_(int64_t initialCap);
RAE_UNUSED static rae_IntMap_const_char_p rae_createIntMap_int64_t_(int64_t initialCap);
RAE_UNUSED static int64_t rae_sizeof_(void);
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
  rae_ext_rae_log_any(rae_any("StringMap test"));
  rae_StringMap_int64_t m = rae_createStringMap_int64_t_(((int64_t)4LL));
  rae_set_rae_StringMap_int64_t_const_char_p_int64_t_(&(m), "apple", ((int64_t)10LL));
  rae_set_rae_StringMap_int64_t_const_char_p_int64_t_(&(m), "banana", ((int64_t)20LL));
  rae_set_rae_StringMap_int64_t_const_char_p_int64_t_(&(m), "cherry", ((int64_t)30LL));
  const char* k1 = "apple";
  const char* k2 = "banana";
  const char* k3 = "cherry";
  const char* k4 = "durian";
  rae_ext_rae_log_stream_any(rae_any("apple: ")), rae_ext_rae_log_stream_any(rae_any(LIFT_TO_TEMP(int64_t, rae_get_rae_StringMap_int64_t_const_char_p_(&(m), k1)))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("banana: ")), rae_ext_rae_log_stream_any(rae_any(LIFT_TO_TEMP(int64_t, rae_get_rae_StringMap_int64_t_const_char_p_(&(m), k2)))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("cherry: ")), rae_ext_rae_log_stream_any(rae_any(LIFT_TO_TEMP(int64_t, rae_get_rae_StringMap_int64_t_const_char_p_(&(m), k3)))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("missing: ")), rae_ext_rae_log_stream_any(rae_any(LIFT_TO_TEMP(int64_t, rae_get_rae_StringMap_int64_t_const_char_p_(&(m), k4)))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("has banana: ")), rae_ext_rae_log_stream_any(rae_any_bool(rae_has_rae_StringMap_int64_t_const_char_p_(&(m), k2))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("has durian: ")), rae_ext_rae_log_stream_any(rae_any_bool(rae_has_rae_StringMap_int64_t_const_char_p_(&(m), k4))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("length: ")), rae_ext_rae_log_stream_any(rae_any(&(m.length))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_any(rae_any("IntMap test"));
  rae_IntMap_const_char_p m2 = rae_createIntMap_int64_t_(((int64_t)4LL));
  rae_set_rae_IntMap_const_char_p_int64_t_const_char_p_(&(m2), ((int64_t)1LL), "one");
  rae_set_rae_IntMap_const_char_p_int64_t_const_char_p_(&(m2), ((int64_t)2LL), "two");
  rae_set_rae_IntMap_const_char_p_int64_t_const_char_p_(&(m2), ((int64_t)10LL), "ten");
  rae_ext_rae_log_stream_any(rae_any("1: ")), rae_ext_rae_log_stream_any(rae_any(LIFT_TO_TEMP(const char*, rae_get_rae_IntMap_const_char_p_int64_t_(&(m2), ((int64_t)1LL))))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("2: ")), rae_ext_rae_log_stream_any(rae_any(LIFT_TO_TEMP(const char*, rae_get_rae_IntMap_const_char_p_int64_t_(&(m2), ((int64_t)2LL))))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("10: ")), rae_ext_rae_log_stream_any(rae_any(LIFT_TO_TEMP(const char*, rae_get_rae_IntMap_const_char_p_int64_t_(&(m2), ((int64_t)10LL))))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  rae_ext_rae_log_stream_any(rae_any("length: ")), rae_ext_rae_log_stream_any(rae_any(&(m2.length))), rae_ext_rae_log_stream_any(rae_any("")), rae_ext_rae_log_cstr("");
  }
  return 0;
}

RAE_UNUSED static rae_StringMap_int64_t rae_createStringMap_int64_t_(int64_t initialCap) {
  {
  return (rae_StringMap_int64_t){ .data = (int64_t*)(void*)rae_ext_rae_buf_alloc(initialCap, sizeof(int64_t)), .length = ((int64_t)0LL), .capacity = initialCap };
  }
}

RAE_UNUSED static rae_IntMap_const_char_p rae_createIntMap_int64_t_(int64_t initialCap) {
  {
  return (rae_IntMap_const_char_p){ .data = (int64_t*)(void*)rae_ext_rae_buf_alloc(initialCap, sizeof(int64_t)), .length = ((int64_t)0LL), .capacity = initialCap };
  }
}

RAE_UNUSED static void rae_set_rae_StringMap_int64_t_const_char_p_int64_t_(rae_StringMap_int64_t* this, const char* k, int64_t value) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  this->data = (int64_t*)(void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  }
  }
  if ((bool)(this->length * ((int64_t)2LL) > this->capacity)) {
  {
  rae_growStringMap_rae_StringMap_int64_t_(this);
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
  this->data[idx] = { .k = k, .value = value, .occupied = (bool)true };
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

RAE_UNUSED static RaeAny rae_get_rae_StringMap_int64_t_const_char_p_(rae_StringMap_int64_t* this, const char* k) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  return 0;
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
  return 0;
  }
  }
  if (rae_ext_rae_str_eq(entry.k, k)) {
  {
  return entry.value;
  }
  }
  idx = (idx + ((int64_t)1LL)) % this->capacity;
  if ((bool)(idx == startIdx)) {
  {
  return 0;
  }
  }
  }
  }
  }
  }
}

RAE_UNUSED static rae_Bool rae_has_rae_StringMap_int64_t_const_char_p_(rae_StringMap_int64_t* this, const char* k) {
  {
  int64_t __m0 = rae_get_rae_StringMap_int64_t_const_char_p_(this, k);
  if (__m0 == 0) {
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

RAE_UNUSED static void rae_set_rae_IntMap_const_char_p_int64_t_const_char_p_(rae_IntMap_const_char_p* this, int64_t k, const char* value) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  this->data = (int64_t*)(void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  }
  }
  if ((bool)(this->length * ((int64_t)2LL) > this->capacity)) {
  {
  rae_growIntMap_rae_IntMap_const_char_p_(this);
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
  rae_IntMapEntry_const_char_p entry = this->data[idx];
  if (((bool)!(entry.occupied))) {
  {
  this->data[idx] = { .k = k, .value = value, .occupied = (bool)true };
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

RAE_UNUSED static RaeAny rae_get_rae_IntMap_const_char_p_int64_t_(rae_IntMap_const_char_p* this, int64_t k) {
  {
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  return 0;
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
  rae_IntMapEntry_const_char_p entry = this->data[idx];
  if (((bool)!(entry.occupied))) {
  {
  return 0;
  }
  }
  if ((bool)(entry.k == k)) {
  {
  return entry.value;
  }
  }
  idx = (idx + ((int64_t)1LL)) % this->capacity;
  if ((bool)(idx == startIdx)) {
  {
  return 0;
  }
  }
  }
  }
  }
  }
}

RAE_UNUSED static void rae_growStringMap_rae_StringMap_int64_t_(rae_StringMap_int64_t* this) {
  {
  int64_t oldCap = this->capacity;
  rae_StringMapEntry_int64_t* oldData = this->data;
  this->capacity = oldCap * ((int64_t)2LL);
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  }
  }
  this->data = (int64_t*)(void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  this->length = ((int64_t)0LL);
  int64_t i = ((int64_t)0LL);
  {
  while ((bool)(i < oldCap)) {
  {
  rae_StringMapEntry_int64_t entry = oldData[i];
  if (entry.occupied) {
  {
  rae_set_rae_StringMap_int64_t_const_char_p_int64_t_(this, entry.k, entry.value);
  }
  }
  i = i + ((int64_t)1LL);
  }
  }
  }
  rae_ext_rae_buf_free((void*)(oldData));
  }
}

RAE_UNUSED static void rae_growIntMap_rae_IntMap_const_char_p_(rae_IntMap_const_char_p* this) {
  {
  int64_t oldCap = this->capacity;
  rae_IntMapEntry_const_char_p* oldData = this->data;
  this->capacity = oldCap * ((int64_t)2LL);
  if ((bool)(this->capacity == ((int64_t)0LL))) {
  {
  this->capacity = ((int64_t)8LL);
  }
  }
  this->data = (int64_t*)(void*)rae_ext_rae_buf_alloc(this->capacity, sizeof(int64_t));
  this->length = ((int64_t)0LL);
  int64_t i = ((int64_t)0LL);
  {
  while ((bool)(i < oldCap)) {
  {
  rae_IntMapEntry_const_char_p entry = oldData[i];
  if (entry.occupied) {
  {
  rae_set_rae_IntMap_const_char_p_int64_t_const_char_p_(this, entry.k, entry.value);
  }
  }
  i = i + ((int64_t)1LL);
  }
  }
  }
  rae_ext_rae_buf_free((void*)(oldData));
  }
}

