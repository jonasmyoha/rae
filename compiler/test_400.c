#include "rae_runtime.h"

typedef struct rae_Buffer_Any_ rae_Buffer_Any_;
typedef struct rae_List_Any_ rae_List_Any_;
typedef struct rae_Buffer_rae_StringMapEntry_Any_ rae_Buffer_rae_StringMapEntry_Any_;
typedef struct rae_StringMapEntry_Any_ rae_StringMapEntry_Any_;
typedef struct rae_StringMap_Any_ rae_StringMap_Any_;
typedef struct rae_List_String_ rae_List_String_;
typedef struct rae_Buffer_rae_IntMapEntry_Any_ rae_Buffer_rae_IntMapEntry_Any_;
typedef struct rae_IntMapEntry_Any_ rae_IntMapEntry_Any_;
typedef struct rae_IntMap_Any_ rae_IntMap_Any_;
typedef struct rae_List_Int_ rae_List_Int_;

struct rae_List_Any_ {
  RaeAny* data;
  int64_t length;
  int64_t capacity;
};

struct rae_StringMapEntry_Any_ {
  const char* k;
  RaeAny value;
  int8_t occupied;
};

struct rae_StringMap_Any_ {
  rae_StringMapEntry_Any_* data;
  int64_t length;
  int64_t capacity;
};

struct rae_List_String_ {
  const char** data;
  int64_t length;
  int64_t capacity;
};

struct rae_IntMapEntry_Any_ {
  int64_t k;
  RaeAny value;
  int8_t occupied;
};

struct rae_IntMap_Any_ {
  rae_IntMapEntry_Any_* data;
  int64_t length;
  int64_t capacity;
};

struct rae_List_Int_ {
  int64_t* data;
  int64_t length;
  int64_t capacity;
};


RAE_UNUSED static const char* rae_toJson_rae_Buffer_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_List_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_Buffer_rae_StringMapEntry_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_StringMapEntry_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_StringMap_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_List_String__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_Buffer_rae_IntMapEntry_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_IntMapEntry_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_IntMap_Any__(void* this) {
  (void)this;
  return "{}";
}

RAE_UNUSED static const char* rae_toJson_rae_List_Int__(void* this) {
  (void)this;
  return "{}";
}

extern int64_t rae_ext_nextTick(void);
extern int64_t rae_ext_nowMs(void);
extern int64_t rae_ext_nowNs(void);
extern void rae_ext_rae_sleep(int64_t ms);
extern double rae_ext_rae_int_to_float(int64_t i);
RAE_UNUSED static double rae_toFloat_rae_Int_(int64_t this);
RAE_UNUSED static rae_List_Any_ rae_createList_rae_Int_(int64_t initialCap);
RAE_UNUSED static void rae_grow_rae_List_Any__(rae_List_Any_* this);
RAE_UNUSED static void rae_add_rae_List_Any__rae_T_(rae_List_Any_* this, RaeAny value);
RAE_UNUSED static RaeAny rae_get_rae_List_Any__rae_Int_(const rae_List_Any_* this, int64_t index);
RAE_UNUSED static void rae_set_rae_List_Any__rae_Int_rae_T_(rae_List_Any_* this, int64_t index, RaeAny value);
RAE_UNUSED static void rae_insert_rae_List_Any__rae_Int_rae_T_(rae_List_Any_* this, int64_t index, RaeAny value);
RAE_UNUSED static RaeAny rae_pop_rae_List_Any__(rae_List_Any_* this);
RAE_UNUSED static void rae_remove_rae_List_Any__rae_Int_(rae_List_Any_* this, int64_t index);
RAE_UNUSED static void rae_clear_rae_List_Any__(rae_List_Any_* this);
RAE_UNUSED static int64_t rae_length_rae_List_Any__(const rae_List_Any_* this);
RAE_UNUSED static void rae_swap_rae_List_Any__rae_Int_rae_Int_(rae_List_Any_* this, int64_t i, int64_t j);
RAE_UNUSED static void rae_free_rae_List_Any__(rae_List_Any_* this);
extern int64_t rae_ext_rae_str_hash(const char* s);
extern int8_t rae_ext_rae_str_eq(const char* a, const char* b);
RAE_UNUSED static rae_StringMap_Any_ rae_createStringMap_rae_Int_(int64_t initialCap);
RAE_UNUSED static void rae_set_rae_StringMap_Any__rae_String_rae_V_(rae_StringMap_Any_* this, const char* k, RaeAny value);
RAE_UNUSED static RaeAny rae_get_rae_StringMap_Any__rae_String_(const rae_StringMap_Any_* this, const char* k);
RAE_UNUSED static int8_t rae_has_rae_StringMap_Any__rae_String_(const rae_StringMap_Any_* this, const char* k);
RAE_UNUSED static void rae_remove_rae_StringMap_Any__rae_String_(rae_StringMap_Any_* this, const char* k);
RAE_UNUSED static rae_List_String_ rae_keys_rae_StringMap_Any__(const rae_StringMap_Any_* this);
RAE_UNUSED static rae_List_Any_ rae_values_rae_StringMap_Any__(const rae_StringMap_Any_* this);
RAE_UNUSED static void rae_growStringMap_rae_StringMap_Any__(rae_StringMap_Any_* this);
RAE_UNUSED static void rae_free_rae_StringMap_Any__(rae_StringMap_Any_* this);
RAE_UNUSED static rae_IntMap_Any_ rae_createIntMap_rae_Int_(int64_t initialCap);
RAE_UNUSED static void rae_set_rae_IntMap_Any__rae_Int_rae_V_(rae_IntMap_Any_* this, int64_t k, RaeAny value);
RAE_UNUSED static RaeAny rae_get_rae_IntMap_Any__rae_Int_(const rae_IntMap_Any_* this, int64_t k);
RAE_UNUSED static int8_t rae_has_rae_IntMap_Any__rae_Int_(const rae_IntMap_Any_* this, int64_t k);
RAE_UNUSED static void rae_remove_rae_IntMap_Any__rae_Int_(rae_IntMap_Any_* this, int64_t k);
RAE_UNUSED static rae_List_Int_ rae_keys_rae_IntMap_Any__(const rae_IntMap_Any_* this);
RAE_UNUSED static rae_List_Any_ rae_values_rae_IntMap_Any__(const rae_IntMap_Any_* this);
RAE_UNUSED static void rae_growIntMap_rae_IntMap_Any__(rae_IntMap_Any_* this);
RAE_UNUSED static void rae_free_rae_IntMap_Any__(rae_IntMap_Any_* this);
extern const char* rae_ext_rae_io_read_line(void);
extern int64_t rae_ext_rae_io_read_char(void);
RAE_UNUSED static const char* rae_readLine_(void);
RAE_UNUSED static int64_t rae_readChar_(void);
RAE_UNUSED static void rae_helper_(void);

RAE_UNUSED static double rae_toFloat_rae_Int_(int64_t this) {
  double _ret = rae_ext_rae_int_to_float(this);
  return _ret;
}

typedef struct {
  int64_t this;
} _spawn_args_rae_toFloat_rae_Int_;

static void* _spawn_wrapper_rae_toFloat_rae_Int_(void* data) {
  _spawn_args_rae_toFloat_rae_Int_* args = (_spawn_args_rae_toFloat_rae_Int_*)data;
  rae_toFloat_rae_Int_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_List_Any_ rae_createList_rae_Int_(int64_t initialCap) {
  rae_List_Any_ _ret = (rae_List_Any_){ .data = rae_ext_rae_buf_alloc(initialCap, sizeof(RaeAny)), .length = 0, .capacity = initialCap };
  return _ret;
}

typedef struct {
  int64_t initialCap;
} _spawn_args_rae_createList_rae_Int_;

static void* _spawn_wrapper_rae_createList_rae_Int_(void* data) {
  _spawn_args_rae_createList_rae_Int_* args = (_spawn_args_rae_createList_rae_Int_*)data;
  rae_createList_rae_Int_(args->initialCap);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_grow_rae_List_Any__(rae_List_Any_* this) {
  int64_t newCap = this->capacity * 2;
  if (newCap == 0) {
  newCap = 4;
  }
  RaeAny* newData = rae_ext_rae_buf_alloc(newCap, sizeof(RaeAny));
  rae_ext_rae_buf_copy(this->data, 0, newData, 0, this->length, sizeof(RaeAny));
  rae_ext_rae_buf_free(this->data);
  this->data = newData;
  this->capacity = newCap;
}

typedef struct {
  rae_List_Any_* this;
} _spawn_args_rae_grow_rae_List_Any__;

static void* _spawn_wrapper_rae_grow_rae_List_Any__(void* data) {
  _spawn_args_rae_grow_rae_List_Any__* args = (_spawn_args_rae_grow_rae_List_Any__*)data;
  rae_grow_rae_List_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_add_rae_List_Any__rae_T_(rae_List_Any_* this, RaeAny value) {
  if (this->length == this->capacity) {
  rae_grow_rae_List_Any__(this);
  }
  ((RaeAny*)( this->data))[this->length] = rae_any(value);
  this->length = this->length + 1;
}

typedef struct {
  rae_List_Any_* this;
  RaeAny value;
} _spawn_args_rae_add_rae_List_Any__rae_T_;

static void* _spawn_wrapper_rae_add_rae_List_Any__rae_T_(void* data) {
  _spawn_args_rae_add_rae_List_Any__rae_T_* args = (_spawn_args_rae_add_rae_List_Any__rae_T_*)data;
  rae_add_rae_List_Any__rae_T_(args->this, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static RaeAny rae_get_rae_List_Any__rae_Int_(const rae_List_Any_* this, int64_t index) {
  if (index < 0 || index >= this->length) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  RaeAny _ret = rae_any(((RaeAny*)( this->data))[index]);
  return _ret;
}

typedef struct {
  rae_List_Any_* this;
  int64_t index;
} _spawn_args_rae_get_rae_List_Any__rae_Int_;

static void* _spawn_wrapper_rae_get_rae_List_Any__rae_Int_(void* data) {
  _spawn_args_rae_get_rae_List_Any__rae_Int_* args = (_spawn_args_rae_get_rae_List_Any__rae_Int_*)data;
  rae_get_rae_List_Any__rae_Int_(args->this, args->index);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_set_rae_List_Any__rae_Int_rae_T_(rae_List_Any_* this, int64_t index, RaeAny value) {
  if (index < 0 || index >= this->length) {
  return;
  }
  ((RaeAny*)( this->data))[index] = rae_any(value);
}

typedef struct {
  rae_List_Any_* this;
  int64_t index;
  RaeAny value;
} _spawn_args_rae_set_rae_List_Any__rae_Int_rae_T_;

static void* _spawn_wrapper_rae_set_rae_List_Any__rae_Int_rae_T_(void* data) {
  _spawn_args_rae_set_rae_List_Any__rae_Int_rae_T_* args = (_spawn_args_rae_set_rae_List_Any__rae_Int_rae_T_*)data;
  rae_set_rae_List_Any__rae_Int_rae_T_(args->this, args->index, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_insert_rae_List_Any__rae_Int_rae_T_(rae_List_Any_* this, int64_t index, RaeAny value) {
  if (index < 0 || index > this->length) {
  return;
  }
  if (this->length == this->capacity) {
  rae_grow_rae_List_Any__(this);
  }
  if (index < this->length) {
  rae_ext_rae_buf_copy(this->data, index, this->data, index + 1, this->length - index, sizeof(RaeAny));
  }
  ((RaeAny*)( this->data))[index] = rae_any(value);
  this->length = this->length + 1;
}

typedef struct {
  rae_List_Any_* this;
  int64_t index;
  RaeAny value;
} _spawn_args_rae_insert_rae_List_Any__rae_Int_rae_T_;

static void* _spawn_wrapper_rae_insert_rae_List_Any__rae_Int_rae_T_(void* data) {
  _spawn_args_rae_insert_rae_List_Any__rae_Int_rae_T_* args = (_spawn_args_rae_insert_rae_List_Any__rae_Int_rae_T_*)data;
  rae_insert_rae_List_Any__rae_Int_rae_T_(args->this, args->index, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static RaeAny rae_pop_rae_List_Any__(rae_List_Any_* this) {
  if (this->length == 0) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  RaeAny val = ((RaeAny*)( this->data))[this->length - 1];
  this->length = this->length - 1;
  RaeAny _ret = rae_any(val);
  return _ret;
}

typedef struct {
  rae_List_Any_* this;
} _spawn_args_rae_pop_rae_List_Any__;

static void* _spawn_wrapper_rae_pop_rae_List_Any__(void* data) {
  _spawn_args_rae_pop_rae_List_Any__* args = (_spawn_args_rae_pop_rae_List_Any__*)data;
  rae_pop_rae_List_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_remove_rae_List_Any__rae_Int_(rae_List_Any_* this, int64_t index) {
  if (index < 0 || index >= this->length) {
  return;
  }
  if (index < this->length - 1) {
  rae_ext_rae_buf_copy(this->data, index + 1, this->data, index, this->length - index - 1, sizeof(RaeAny));
  }
  this->length = this->length - 1;
}

typedef struct {
  rae_List_Any_* this;
  int64_t index;
} _spawn_args_rae_remove_rae_List_Any__rae_Int_;

static void* _spawn_wrapper_rae_remove_rae_List_Any__rae_Int_(void* data) {
  _spawn_args_rae_remove_rae_List_Any__rae_Int_* args = (_spawn_args_rae_remove_rae_List_Any__rae_Int_*)data;
  rae_remove_rae_List_Any__rae_Int_(args->this, args->index);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_clear_rae_List_Any__(rae_List_Any_* this) {
  this->length = 0;
}

typedef struct {
  rae_List_Any_* this;
} _spawn_args_rae_clear_rae_List_Any__;

static void* _spawn_wrapper_rae_clear_rae_List_Any__(void* data) {
  _spawn_args_rae_clear_rae_List_Any__* args = (_spawn_args_rae_clear_rae_List_Any__*)data;
  rae_clear_rae_List_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_length_rae_List_Any__(const rae_List_Any_* this) {
  int64_t _ret = this->length;
  return _ret;
}

typedef struct {
  rae_List_Any_* this;
} _spawn_args_rae_length_rae_List_Any__;

static void* _spawn_wrapper_rae_length_rae_List_Any__(void* data) {
  _spawn_args_rae_length_rae_List_Any__* args = (_spawn_args_rae_length_rae_List_Any__*)data;
  rae_length_rae_List_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_swap_rae_List_Any__rae_Int_rae_Int_(rae_List_Any_* this, int64_t i, int64_t j) {
  RaeAny temp = ((RaeAny*)( this->data))[i];
  ((RaeAny*)( this->data))[i] = rae_any(((RaeAny*)( this->data))[j]);
  ((RaeAny*)( this->data))[j] = rae_any(temp);
}

typedef struct {
  rae_List_Any_* this;
  int64_t i;
  int64_t j;
} _spawn_args_rae_swap_rae_List_Any__rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_swap_rae_List_Any__rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_swap_rae_List_Any__rae_Int_rae_Int_* args = (_spawn_args_rae_swap_rae_List_Any__rae_Int_rae_Int_*)data;
  rae_swap_rae_List_Any__rae_Int_rae_Int_(args->this, args->i, args->j);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_free_rae_List_Any__(rae_List_Any_* this) {
  rae_ext_rae_buf_free(this->data);
  this->length = 0;
  this->capacity = 0;
}

typedef struct {
  rae_List_Any_* this;
} _spawn_args_rae_free_rae_List_Any__;

static void* _spawn_wrapper_rae_free_rae_List_Any__(void* data) {
  _spawn_args_rae_free_rae_List_Any__* args = (_spawn_args_rae_free_rae_List_Any__*)data;
  rae_free_rae_List_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_StringMap_Any_ rae_createStringMap_rae_Int_(int64_t initialCap) {
  rae_StringMap_Any_ _ret = (rae_StringMap_Any_){ .data = rae_ext_rae_buf_alloc(initialCap, sizeof(RaeAny)), .length = 0, .capacity = initialCap };
  return _ret;
}

typedef struct {
  int64_t initialCap;
} _spawn_args_rae_createStringMap_rae_Int_;

static void* _spawn_wrapper_rae_createStringMap_rae_Int_(void* data) {
  _spawn_args_rae_createStringMap_rae_Int_* args = (_spawn_args_rae_createStringMap_rae_Int_*)data;
  rae_createStringMap_rae_Int_(args->initialCap);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_set_rae_StringMap_Any__rae_String_rae_V_(rae_StringMap_Any_* this, const char* k, RaeAny value) {
  if (this->capacity == 0) {
  this->capacity = 8;
  this->data = rae_ext_rae_buf_alloc(this->capacity, sizeof(RaeAny));
  }
  if (this->length * 2 > this->capacity) {
  rae_growStringMap_rae_StringMap_Any__(this);
  }
  int64_t h = rae_ext_rae_str_hash(k);
  int64_t idx = h % this->capacity;
  if (idx < 0) {
  idx = (-idx);
  }
  {
  while (1) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( this->data))[idx];
  if ((!(entry.occupied))) {
  ((rae_StringMapEntry_Any_*)( this->data))[idx] = (rae_StringMapEntry_Any_){ .k = k, .value = rae_any(value), .occupied = 1 };
  this->length = this->length + 1;
  return;
  }
  if (rae_ext_rae_str_eq(entry.k, k)) {
  entry.value = value;
  ((rae_StringMapEntry_Any_*)( this->data))[idx] = entry;
  return;
  }
  idx = (idx + 1) % this->capacity;
  }
  }
}

typedef struct {
  rae_StringMap_Any_* this;
  const char* k;
  RaeAny value;
} _spawn_args_rae_set_rae_StringMap_Any__rae_String_rae_V_;

static void* _spawn_wrapper_rae_set_rae_StringMap_Any__rae_String_rae_V_(void* data) {
  _spawn_args_rae_set_rae_StringMap_Any__rae_String_rae_V_* args = (_spawn_args_rae_set_rae_StringMap_Any__rae_String_rae_V_*)data;
  rae_set_rae_StringMap_Any__rae_String_rae_V_(args->this, args->k, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static RaeAny rae_get_rae_StringMap_Any__rae_String_(const rae_StringMap_Any_* this, const char* k) {
  if (this->capacity == 0) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  int64_t h = rae_ext_rae_str_hash(k);
  int64_t idx = h % this->capacity;
  if (idx < 0) {
  idx = (-idx);
  }
  int64_t startIdx = idx;
  {
  while (1) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( this->data))[idx];
  if ((!(entry.occupied))) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  if (rae_ext_rae_str_eq(entry.k, k)) {
  RaeAny _ret = rae_any(entry.value);
  return _ret;
  }
  idx = (idx + 1) % this->capacity;
  if (idx == startIdx) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  }
  }
}

typedef struct {
  rae_StringMap_Any_* this;
  const char* k;
} _spawn_args_rae_get_rae_StringMap_Any__rae_String_;

static void* _spawn_wrapper_rae_get_rae_StringMap_Any__rae_String_(void* data) {
  _spawn_args_rae_get_rae_StringMap_Any__rae_String_* args = (_spawn_args_rae_get_rae_StringMap_Any__rae_String_*)data;
  rae_get_rae_StringMap_Any__rae_String_(args->this, args->k);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_has_rae_StringMap_Any__rae_String_(const rae_StringMap_Any_* this, const char* k) {
  RaeAny __match0 = rae_any(((RaeAny)(rae_get_rae_StringMap_Any__rae_String_(this, k))));
  if (__match0.type == RAE_TYPE_NONE) {
  int8_t _ret = 0;
  return _ret;
  } else {
  int8_t _ret = 1;
  return _ret;
  }
}

typedef struct {
  rae_StringMap_Any_* this;
  const char* k;
} _spawn_args_rae_has_rae_StringMap_Any__rae_String_;

static void* _spawn_wrapper_rae_has_rae_StringMap_Any__rae_String_(void* data) {
  _spawn_args_rae_has_rae_StringMap_Any__rae_String_* args = (_spawn_args_rae_has_rae_StringMap_Any__rae_String_*)data;
  rae_has_rae_StringMap_Any__rae_String_(args->this, args->k);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_remove_rae_StringMap_Any__rae_String_(rae_StringMap_Any_* this, const char* k) {
  if (this->capacity == 0) {
  return;
  }
  int64_t h = rae_ext_rae_str_hash(k);
  int64_t idx = h % this->capacity;
  if (idx < 0) {
  idx = (-idx);
  }
  int64_t startIdx = idx;
  {
  while (1) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( this->data))[idx];
  if ((!(entry.occupied))) {
  return;
  }
  if (rae_ext_rae_str_eq(entry.k, k)) {
  entry.occupied = 0;
  ((rae_StringMapEntry_Any_*)( this->data))[idx] = entry;
  this->length = this->length - 1;
  return;
  }
  idx = (idx + 1) % this->capacity;
  if (idx == startIdx) {
  return;
  }
  }
  }
}

typedef struct {
  rae_StringMap_Any_* this;
  const char* k;
} _spawn_args_rae_remove_rae_StringMap_Any__rae_String_;

static void* _spawn_wrapper_rae_remove_rae_StringMap_Any__rae_String_(void* data) {
  _spawn_args_rae_remove_rae_StringMap_Any__rae_String_* args = (_spawn_args_rae_remove_rae_StringMap_Any__rae_String_*)data;
  rae_remove_rae_StringMap_Any__rae_String_(args->this, args->k);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_List_String_ rae_keys_rae_StringMap_Any__(const rae_StringMap_Any_* this) {
  rae_List_String_ result = rae_createList_rae_Int_(this->length);
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__rae_T_(&(result), rae_any(entry.k));
  }
  i = i + 1;
  }
  }
  rae_List_String_ _ret = result;
  return _ret;
}

typedef struct {
  rae_StringMap_Any_* this;
} _spawn_args_rae_keys_rae_StringMap_Any__;

static void* _spawn_wrapper_rae_keys_rae_StringMap_Any__(void* data) {
  _spawn_args_rae_keys_rae_StringMap_Any__* args = (_spawn_args_rae_keys_rae_StringMap_Any__*)data;
  rae_keys_rae_StringMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_List_Any_ rae_values_rae_StringMap_Any__(const rae_StringMap_Any_* this) {
  rae_List_Any_ result = rae_createList_rae_Int_(this->length);
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__rae_T_(&(result), entry.value);
  }
  i = i + 1;
  }
  }
  rae_List_Any_ _ret = result;
  return _ret;
}

typedef struct {
  rae_StringMap_Any_* this;
} _spawn_args_rae_values_rae_StringMap_Any__;

static void* _spawn_wrapper_rae_values_rae_StringMap_Any__(void* data) {
  _spawn_args_rae_values_rae_StringMap_Any__* args = (_spawn_args_rae_values_rae_StringMap_Any__*)data;
  rae_values_rae_StringMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_growStringMap_rae_StringMap_Any__(rae_StringMap_Any_* this) {
  int64_t oldCap = this->capacity;
  rae_StringMapEntry_Any_* oldData = this->data;
  this->capacity = oldCap * 2;
  if (this->capacity == 0) {
  this->capacity = 8;
  }
  this->data = rae_ext_rae_buf_alloc(this->capacity, sizeof(RaeAny));
  this->length = 0;
  int64_t i = 0;
  {
  while (i < oldCap) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( oldData))[i];
  if (entry.occupied) {
  rae_set_rae_StringMap_Any__rae_String_rae_V_(this, entry.k, entry.value);
  }
  i = i + 1;
  }
  }
  rae_ext_rae_buf_free(oldData);
}

typedef struct {
  rae_StringMap_Any_* this;
} _spawn_args_rae_growStringMap_rae_StringMap_Any__;

static void* _spawn_wrapper_rae_growStringMap_rae_StringMap_Any__(void* data) {
  _spawn_args_rae_growStringMap_rae_StringMap_Any__* args = (_spawn_args_rae_growStringMap_rae_StringMap_Any__*)data;
  rae_growStringMap_rae_StringMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_free_rae_StringMap_Any__(rae_StringMap_Any_* this) {
  rae_ext_rae_buf_free(this->data);
  this->length = 0;
  this->capacity = 0;
}

typedef struct {
  rae_StringMap_Any_* this;
} _spawn_args_rae_free_rae_StringMap_Any__;

static void* _spawn_wrapper_rae_free_rae_StringMap_Any__(void* data) {
  _spawn_args_rae_free_rae_StringMap_Any__* args = (_spawn_args_rae_free_rae_StringMap_Any__*)data;
  rae_free_rae_StringMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_IntMap_Any_ rae_createIntMap_rae_Int_(int64_t initialCap) {
  rae_IntMap_Any_ _ret = (rae_IntMap_Any_){ .data = rae_ext_rae_buf_alloc(initialCap, sizeof(RaeAny)), .length = 0, .capacity = initialCap };
  return _ret;
}

typedef struct {
  int64_t initialCap;
} _spawn_args_rae_createIntMap_rae_Int_;

static void* _spawn_wrapper_rae_createIntMap_rae_Int_(void* data) {
  _spawn_args_rae_createIntMap_rae_Int_* args = (_spawn_args_rae_createIntMap_rae_Int_*)data;
  rae_createIntMap_rae_Int_(args->initialCap);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_set_rae_IntMap_Any__rae_Int_rae_V_(rae_IntMap_Any_* this, int64_t k, RaeAny value) {
  if (this->capacity == 0) {
  this->capacity = 8;
  this->data = rae_ext_rae_buf_alloc(this->capacity, sizeof(RaeAny));
  }
  if (this->length * 2 > this->capacity) {
  rae_growIntMap_rae_IntMap_Any__(this);
  }
  int64_t h = k;
  int64_t idx = h % this->capacity;
  if (idx < 0) {
  idx = (-idx);
  }
  {
  while (1) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( this->data))[idx];
  if ((!(entry.occupied))) {
  ((rae_IntMapEntry_Any_*)( this->data))[idx] = (rae_IntMapEntry_Any_){ .k = k, .value = rae_any(value), .occupied = 1 };
  this->length = this->length + 1;
  return;
  }
  if (entry.k == k) {
  entry.value = value;
  ((rae_IntMapEntry_Any_*)( this->data))[idx] = entry;
  return;
  }
  idx = (idx + 1) % this->capacity;
  }
  }
}

typedef struct {
  rae_IntMap_Any_* this;
  int64_t k;
  RaeAny value;
} _spawn_args_rae_set_rae_IntMap_Any__rae_Int_rae_V_;

static void* _spawn_wrapper_rae_set_rae_IntMap_Any__rae_Int_rae_V_(void* data) {
  _spawn_args_rae_set_rae_IntMap_Any__rae_Int_rae_V_* args = (_spawn_args_rae_set_rae_IntMap_Any__rae_Int_rae_V_*)data;
  rae_set_rae_IntMap_Any__rae_Int_rae_V_(args->this, args->k, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static RaeAny rae_get_rae_IntMap_Any__rae_Int_(const rae_IntMap_Any_* this, int64_t k) {
  if (this->capacity == 0) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  int64_t h = k;
  int64_t idx = h % this->capacity;
  if (idx < 0) {
  idx = (-idx);
  }
  int64_t startIdx = idx;
  {
  while (1) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( this->data))[idx];
  if ((!(entry.occupied))) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  if (entry.k == k) {
  RaeAny _ret = rae_any(entry.value);
  return _ret;
  }
  idx = (idx + 1) % this->capacity;
  if (idx == startIdx) {
  RaeAny _ret = rae_any_none();
  return _ret;
  }
  }
  }
}

typedef struct {
  rae_IntMap_Any_* this;
  int64_t k;
} _spawn_args_rae_get_rae_IntMap_Any__rae_Int_;

static void* _spawn_wrapper_rae_get_rae_IntMap_Any__rae_Int_(void* data) {
  _spawn_args_rae_get_rae_IntMap_Any__rae_Int_* args = (_spawn_args_rae_get_rae_IntMap_Any__rae_Int_*)data;
  rae_get_rae_IntMap_Any__rae_Int_(args->this, args->k);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_has_rae_IntMap_Any__rae_Int_(const rae_IntMap_Any_* this, int64_t k) {
  RaeAny __match0 = rae_any(((RaeAny)(rae_get_rae_IntMap_Any__rae_Int_(this, k))));
  if (__match0.type == RAE_TYPE_NONE) {
  int8_t _ret = 0;
  return _ret;
  } else {
  int8_t _ret = 1;
  return _ret;
  }
}

typedef struct {
  rae_IntMap_Any_* this;
  int64_t k;
} _spawn_args_rae_has_rae_IntMap_Any__rae_Int_;

static void* _spawn_wrapper_rae_has_rae_IntMap_Any__rae_Int_(void* data) {
  _spawn_args_rae_has_rae_IntMap_Any__rae_Int_* args = (_spawn_args_rae_has_rae_IntMap_Any__rae_Int_*)data;
  rae_has_rae_IntMap_Any__rae_Int_(args->this, args->k);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_remove_rae_IntMap_Any__rae_Int_(rae_IntMap_Any_* this, int64_t k) {
  if (this->capacity == 0) {
  return;
  }
  int64_t h = k;
  int64_t idx = h % this->capacity;
  if (idx < 0) {
  idx = (-idx);
  }
  int64_t startIdx = idx;
  {
  while (1) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( this->data))[idx];
  if ((!(entry.occupied))) {
  return;
  }
  if (entry.k == k) {
  entry.occupied = 0;
  ((rae_IntMapEntry_Any_*)( this->data))[idx] = entry;
  this->length = this->length - 1;
  return;
  }
  idx = (idx + 1) % this->capacity;
  if (idx == startIdx) {
  return;
  }
  }
  }
}

typedef struct {
  rae_IntMap_Any_* this;
  int64_t k;
} _spawn_args_rae_remove_rae_IntMap_Any__rae_Int_;

static void* _spawn_wrapper_rae_remove_rae_IntMap_Any__rae_Int_(void* data) {
  _spawn_args_rae_remove_rae_IntMap_Any__rae_Int_* args = (_spawn_args_rae_remove_rae_IntMap_Any__rae_Int_*)data;
  rae_remove_rae_IntMap_Any__rae_Int_(args->this, args->k);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_List_Int_ rae_keys_rae_IntMap_Any__(const rae_IntMap_Any_* this) {
  rae_List_Int_ result = rae_createList_rae_Int_(this->length);
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__rae_T_(&(result), rae_any(entry.k));
  }
  i = i + 1;
  }
  }
  rae_List_Int_ _ret = result;
  return _ret;
}

typedef struct {
  rae_IntMap_Any_* this;
} _spawn_args_rae_keys_rae_IntMap_Any__;

static void* _spawn_wrapper_rae_keys_rae_IntMap_Any__(void* data) {
  _spawn_args_rae_keys_rae_IntMap_Any__* args = (_spawn_args_rae_keys_rae_IntMap_Any__*)data;
  rae_keys_rae_IntMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_List_Any_ rae_values_rae_IntMap_Any__(const rae_IntMap_Any_* this) {
  rae_List_Any_ result = rae_createList_rae_Int_(this->length);
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__rae_T_(&(result), entry.value);
  }
  i = i + 1;
  }
  }
  rae_List_Any_ _ret = result;
  return _ret;
}

typedef struct {
  rae_IntMap_Any_* this;
} _spawn_args_rae_values_rae_IntMap_Any__;

static void* _spawn_wrapper_rae_values_rae_IntMap_Any__(void* data) {
  _spawn_args_rae_values_rae_IntMap_Any__* args = (_spawn_args_rae_values_rae_IntMap_Any__*)data;
  rae_values_rae_IntMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_growIntMap_rae_IntMap_Any__(rae_IntMap_Any_* this) {
  int64_t oldCap = this->capacity;
  rae_IntMapEntry_Any_* oldData = this->data;
  this->capacity = oldCap * 2;
  if (this->capacity == 0) {
  this->capacity = 8;
  }
  this->data = rae_ext_rae_buf_alloc(this->capacity, sizeof(RaeAny));
  this->length = 0;
  int64_t i = 0;
  {
  while (i < oldCap) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( oldData))[i];
  if (entry.occupied) {
  rae_set_rae_IntMap_Any__rae_Int_rae_V_(this, entry.k, entry.value);
  }
  i = i + 1;
  }
  }
  rae_ext_rae_buf_free(oldData);
}

typedef struct {
  rae_IntMap_Any_* this;
} _spawn_args_rae_growIntMap_rae_IntMap_Any__;

static void* _spawn_wrapper_rae_growIntMap_rae_IntMap_Any__(void* data) {
  _spawn_args_rae_growIntMap_rae_IntMap_Any__* args = (_spawn_args_rae_growIntMap_rae_IntMap_Any__*)data;
  rae_growIntMap_rae_IntMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_free_rae_IntMap_Any__(rae_IntMap_Any_* this) {
  rae_ext_rae_buf_free(this->data);
  this->length = 0;
  this->capacity = 0;
}

typedef struct {
  rae_IntMap_Any_* this;
} _spawn_args_rae_free_rae_IntMap_Any__;

static void* _spawn_wrapper_rae_free_rae_IntMap_Any__(void* data) {
  _spawn_args_rae_free_rae_IntMap_Any__* args = (_spawn_args_rae_free_rae_IntMap_Any__*)data;
  rae_free_rae_IntMap_Any__(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_readLine_(void) {
  const char* _ret = rae_ext_rae_io_read_line();
  return _ret;
}

typedef struct {
} _spawn_args_rae_readLine_;

static void* _spawn_wrapper_rae_readLine_(void* data) {
  _spawn_args_rae_readLine_* args = (_spawn_args_rae_readLine_*)data;
  rae_readLine_();
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_readChar_(void) {
  int64_t _ret = rae_ext_rae_io_read_char();
  return _ret;
}

typedef struct {
} _spawn_args_rae_readChar_;

static void* _spawn_wrapper_rae_readChar_(void* data) {
  _spawn_args_rae_readChar_* args = (_spawn_args_rae_readChar_*)data;
  rae_readChar_();
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_helper_(void) {
  rae_ext_rae_log_cstr("Hello from C backend helper");
}

typedef struct {
} _spawn_args_rae_helper_;

static void* _spawn_wrapper_rae_helper_(void* data) {
  _spawn_args_rae_helper_* args = (_spawn_args_rae_helper_*)data;
  rae_helper_();
  free(args);
  return NULL;
}

int main(void) {
  rae_ext_rae_log_cstr("C backend demo start");
  rae_helper_();
  rae_ext_rae_log_cstr("C backend demo end");
  return 0;
}

RAE_UNUSED static void rae_grow_rae_List_Any__(rae_List_Any_* this) {
  (void)this;
}

RAE_UNUSED static void rae_add_rae_List_Any__(rae_List_Any_* this, RaeAny value) {
  (void)this;
}

RAE_UNUSED static void rae_get_rae_List_Any__(rae_List_Any_* this, int64_t index) {
  (void)this;
}

RAE_UNUSED static void rae_set_rae_List_Any__(rae_List_Any_* this, int64_t index, RaeAny value) {
  (void)this;
}

RAE_UNUSED static void rae_insert_rae_List_Any__(rae_List_Any_* this, int64_t index, RaeAny value) {
  (void)this;
}

RAE_UNUSED static void rae_pop_rae_List_Any__(rae_List_Any_* this) {
  (void)this;
}

RAE_UNUSED static void rae_remove_rae_List_Any__(rae_List_Any_* this, int64_t index) {
  (void)this;
}

RAE_UNUSED static void rae_clear_rae_List_Any__(rae_List_Any_* this) {
  (void)this;
}

RAE_UNUSED static void rae_length_rae_List_Any__(rae_List_Any_* this) {
  (void)this;
}

RAE_UNUSED static void rae_swap_rae_List_Any__(rae_List_Any_* this, int64_t i, int64_t j) {
  (void)this;
}

RAE_UNUSED static void rae_free_rae_List_Any__(rae_List_Any_* this) {
  (void)this;
}

RAE_UNUSED static void rae_grow_rae_List_String__(rae_List_String_* this) {
  (void)this;
}

RAE_UNUSED static void rae_add_rae_List_String__(rae_List_String_* this, const char* value) {
  (void)this;
}

RAE_UNUSED static void rae_get_rae_List_String__(rae_List_String_* this, int64_t index) {
  (void)this;
}

RAE_UNUSED static void rae_set_rae_List_String__(rae_List_String_* this, int64_t index, const char* value) {
  (void)this;
}

RAE_UNUSED static void rae_insert_rae_List_String__(rae_List_String_* this, int64_t index, const char* value) {
  (void)this;
}

RAE_UNUSED static void rae_pop_rae_List_String__(rae_List_String_* this) {
  (void)this;
}

RAE_UNUSED static void rae_remove_rae_List_String__(rae_List_String_* this, int64_t index) {
  (void)this;
}

RAE_UNUSED static void rae_clear_rae_List_String__(rae_List_String_* this) {
  (void)this;
}

RAE_UNUSED static void rae_length_rae_List_String__(rae_List_String_* this) {
  (void)this;
}

RAE_UNUSED static void rae_swap_rae_List_String__(rae_List_String_* this, int64_t i, int64_t j) {
  (void)this;
}

RAE_UNUSED static void rae_free_rae_List_String__(rae_List_String_* this) {
  (void)this;
}

RAE_UNUSED static void rae_grow_rae_List_Int__(rae_List_Int_* this) {
  (void)this;
}

RAE_UNUSED static void rae_add_rae_List_Int__(rae_List_Int_* this, int64_t value) {
  (void)this;
}

RAE_UNUSED static void rae_get_rae_List_Int__(rae_List_Int_* this, int64_t index) {
  (void)this;
}

RAE_UNUSED static void rae_set_rae_List_Int__(rae_List_Int_* this, int64_t index, int64_t value) {
  (void)this;
}

RAE_UNUSED static void rae_insert_rae_List_Int__(rae_List_Int_* this, int64_t index, int64_t value) {
  (void)this;
}

RAE_UNUSED static void rae_pop_rae_List_Int__(rae_List_Int_* this) {
  (void)this;
}

RAE_UNUSED static void rae_remove_rae_List_Int__(rae_List_Int_* this, int64_t index) {
  (void)this;
}

RAE_UNUSED static void rae_clear_rae_List_Int__(rae_List_Int_* this) {
  (void)this;
}

RAE_UNUSED static void rae_length_rae_List_Int__(rae_List_Int_* this) {
  (void)this;
}

RAE_UNUSED static void rae_swap_rae_List_Int__(rae_List_Int_* this, int64_t i, int64_t j) {
  (void)this;
}

RAE_UNUSED static void rae_free_rae_List_Int__(rae_List_Int_* this) {
  (void)this;
}

