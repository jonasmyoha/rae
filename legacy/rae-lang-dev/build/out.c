#include "rae_runtime.h"

typedef struct {
  int64_t startTime;
  int64_t accumulated;
  int8_t running;
} Timer;

typedef struct {
  RaeAny* data;
  int64_t length;
  int64_t capacity;
} List2;

typedef struct {
  int64_t* data;
  int64_t length;
  int64_t capacity;
} List2Int;

typedef struct {
  RaeAny* data;
  int64_t length;
  int64_t capacity;
} List_String_;

typedef struct {
  RaeAny* data;
  int64_t length;
  int64_t capacity;
} List_V_;

typedef struct {
  RaeAny* data;
  int64_t length;
  int64_t capacity;
} List_Int_;

typedef struct {
  RaeAny* data;
  int64_t length;
  int64_t capacity;
} List_String_;

RAE_UNUSED static const char* rae_toJson_List_(List* this);
RAE_UNUSED static List rae_fromJson_List_(const char* json);
RAE_UNUSED static void* rae_toBinary_List_(List* this, int64_t* out_size);
RAE_UNUSED static List rae_fromBinary_List_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_StringMapEntry_(StringMapEntry* this);
RAE_UNUSED static StringMapEntry rae_fromJson_StringMapEntry_(const char* json);
RAE_UNUSED static void* rae_toBinary_StringMapEntry_(StringMapEntry* this, int64_t* out_size);
RAE_UNUSED static StringMapEntry rae_fromBinary_StringMapEntry_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_StringMap_(StringMap* this);
RAE_UNUSED static StringMap rae_fromJson_StringMap_(const char* json);
RAE_UNUSED static void* rae_toBinary_StringMap_(StringMap* this, int64_t* out_size);
RAE_UNUSED static StringMap rae_fromBinary_StringMap_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_IntMapEntry_(IntMapEntry* this);
RAE_UNUSED static IntMapEntry rae_fromJson_IntMapEntry_(const char* json);
RAE_UNUSED static void* rae_toBinary_IntMapEntry_(IntMapEntry* this, int64_t* out_size);
RAE_UNUSED static IntMapEntry rae_fromBinary_IntMapEntry_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_IntMap_(IntMap* this);
RAE_UNUSED static IntMap rae_fromJson_IntMap_(const char* json);
RAE_UNUSED static void* rae_toBinary_IntMap_(IntMap* this, int64_t* out_size);
RAE_UNUSED static IntMap rae_fromBinary_IntMap_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Timer_(Timer* this);
RAE_UNUSED static Timer rae_fromJson_Timer_(const char* json);
RAE_UNUSED static void* rae_toBinary_Timer_(Timer* this, int64_t* out_size);
RAE_UNUSED static Timer rae_fromBinary_Timer_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_List2_(List2* this);
RAE_UNUSED static List2 rae_fromJson_List2_(const char* json);
RAE_UNUSED static void* rae_toBinary_List2_(List2* this, int64_t* out_size);
RAE_UNUSED static List2 rae_fromBinary_List2_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_List2Int_(List2Int* this);
RAE_UNUSED static List2Int rae_fromJson_List2Int_(const char* json);
RAE_UNUSED static void* rae_toBinary_List2Int_(List2Int* this, int64_t* out_size);
RAE_UNUSED static List2Int rae_fromBinary_List2Int_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_List2Generic_(List2Generic* this);
RAE_UNUSED static List2Generic rae_fromJson_List2Generic_(const char* json);
RAE_UNUSED static void* rae_toBinary_List2Generic_(List2Generic* this, int64_t* out_size);
RAE_UNUSED static List2Generic rae_fromBinary_List2Generic_(void* data, int64_t size);

RAE_UNUSED static const char* rae_toJson_List_(List* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"data\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"length\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->length));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"capacity\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->capacity));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static List rae_fromJson_List_(const char* json) {
  List res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "data");
  val = rae_ext_json_get(json, "length");
  if (val.type == RAE_TYPE_INT) res.length = val.as.i;
  val = rae_ext_json_get(json, "capacity");
  if (val.type == RAE_TYPE_INT) res.capacity = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_List_(List* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static List rae_fromBinary_List_(void* data, int64_t size) {
  (void)data; (void)size;
  List res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_StringMapEntry_(StringMapEntry* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"k\": ");
  res = rae_ext_rae_str_concat(res, "\"");
  res = rae_ext_rae_str_concat(res, this->k ? this->k : "");
  res = rae_ext_rae_str_concat(res, "\"");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"value\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"occupied\": ");
  res = rae_ext_rae_str_concat(res, this->occupied ? "true" : "false");
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static StringMapEntry rae_fromJson_StringMapEntry_(const char* json) {
  StringMapEntry res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "k");
  if (val.type == RAE_TYPE_STRING) res.k = val.as.s;
  val = rae_ext_json_get(json, "value");
  val = rae_ext_json_get(json, "occupied");
  if (val.type == RAE_TYPE_BOOL) res.occupied = val.as.b;
  return res;
}

RAE_UNUSED static void* rae_toBinary_StringMapEntry_(StringMapEntry* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static StringMapEntry rae_fromBinary_StringMapEntry_(void* data, int64_t size) {
  (void)data; (void)size;
  StringMapEntry res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_StringMap_(StringMap* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"data\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"length\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->length));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"capacity\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->capacity));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static StringMap rae_fromJson_StringMap_(const char* json) {
  StringMap res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "data");
  val = rae_ext_json_get(json, "length");
  if (val.type == RAE_TYPE_INT) res.length = val.as.i;
  val = rae_ext_json_get(json, "capacity");
  if (val.type == RAE_TYPE_INT) res.capacity = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_StringMap_(StringMap* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static StringMap rae_fromBinary_StringMap_(void* data, int64_t size) {
  (void)data; (void)size;
  StringMap res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_IntMapEntry_(IntMapEntry* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"k\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->k));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"value\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"occupied\": ");
  res = rae_ext_rae_str_concat(res, this->occupied ? "true" : "false");
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static IntMapEntry rae_fromJson_IntMapEntry_(const char* json) {
  IntMapEntry res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "k");
  if (val.type == RAE_TYPE_INT) res.k = val.as.i;
  val = rae_ext_json_get(json, "value");
  val = rae_ext_json_get(json, "occupied");
  if (val.type == RAE_TYPE_BOOL) res.occupied = val.as.b;
  return res;
}

RAE_UNUSED static void* rae_toBinary_IntMapEntry_(IntMapEntry* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static IntMapEntry rae_fromBinary_IntMapEntry_(void* data, int64_t size) {
  (void)data; (void)size;
  IntMapEntry res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_IntMap_(IntMap* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"data\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"length\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->length));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"capacity\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->capacity));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static IntMap rae_fromJson_IntMap_(const char* json) {
  IntMap res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "data");
  val = rae_ext_json_get(json, "length");
  if (val.type == RAE_TYPE_INT) res.length = val.as.i;
  val = rae_ext_json_get(json, "capacity");
  if (val.type == RAE_TYPE_INT) res.capacity = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_IntMap_(IntMap* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static IntMap rae_fromBinary_IntMap_(void* data, int64_t size) {
  (void)data; (void)size;
  IntMap res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_Timer_(Timer* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"startTime\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->startTime));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"accumulated\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->accumulated));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"running\": ");
  res = rae_ext_rae_str_concat(res, this->running ? "true" : "false");
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static Timer rae_fromJson_Timer_(const char* json) {
  Timer res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "startTime");
  if (val.type == RAE_TYPE_INT) res.startTime = val.as.i;
  val = rae_ext_json_get(json, "accumulated");
  if (val.type == RAE_TYPE_INT) res.accumulated = val.as.i;
  val = rae_ext_json_get(json, "running");
  if (val.type == RAE_TYPE_BOOL) res.running = val.as.b;
  return res;
}

RAE_UNUSED static void* rae_toBinary_Timer_(Timer* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static Timer rae_fromBinary_Timer_(void* data, int64_t size) {
  (void)data; (void)size;
  Timer res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_List2_(List2* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"data\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"length\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->length));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"capacity\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->capacity));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static List2 rae_fromJson_List2_(const char* json) {
  List2 res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "data");
  val = rae_ext_json_get(json, "length");
  if (val.type == RAE_TYPE_INT) res.length = val.as.i;
  val = rae_ext_json_get(json, "capacity");
  if (val.type == RAE_TYPE_INT) res.capacity = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_List2_(List2* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static List2 rae_fromBinary_List2_(void* data, int64_t size) {
  (void)data; (void)size;
  List2 res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_List2Int_(List2Int* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"data\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"length\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->length));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"capacity\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->capacity));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static List2Int rae_fromJson_List2Int_(const char* json) {
  List2Int res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "data");
  val = rae_ext_json_get(json, "length");
  if (val.type == RAE_TYPE_INT) res.length = val.as.i;
  val = rae_ext_json_get(json, "capacity");
  if (val.type == RAE_TYPE_INT) res.capacity = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_List2Int_(List2Int* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static List2Int rae_fromBinary_List2Int_(void* data, int64_t size) {
  (void)data; (void)size;
  List2Int res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_List2Generic_(List2Generic* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"data\": ");
  res = rae_ext_rae_str_concat(res, "null");
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"length\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->length));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"capacity\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->capacity));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static List2Generic rae_fromJson_List2Generic_(const char* json) {
  List2Generic res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "data");
  val = rae_ext_json_get(json, "length");
  if (val.type == RAE_TYPE_INT) res.length = val.as.i;
  val = rae_ext_json_get(json, "capacity");
  if (val.type == RAE_TYPE_INT) res.capacity = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_List2Generic_(List2Generic* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static List2Generic rae_fromBinary_List2Generic_(void* data, int64_t size) {
  (void)data; (void)size;
  List2Generic res = {0};
  return res;
}

extern int64_t rae_ext_nextTick(void);
extern int64_t rae_ext_nowMs(void);
extern int64_t rae_ext_nowNs(void);
extern void rae_ext_rae_sleep(int64_t ms);
extern double rae_ext_rae_int_to_float(int64_t i);
RAE_UNUSED static double rae_toFloat_Int_(int64_t this);
extern int64_t rae_ext_rae_str_hash(const char* s);
extern int8_t rae_ext_rae_str_eq(const char* a, const char* b);
extern int64_t rae_ext_rae_str_len(const char* s);
extern int64_t rae_ext_rae_str_compare(const char* a, const char* b);
extern const char* rae_ext_rae_str_concat(const char* a, const char* b);
extern const char* rae_ext_rae_str_sub(const char* s, int64_t start, int64_t len);
extern int8_t rae_ext_rae_str_contains(const char* s, const char* sub);
extern int8_t rae_ext_rae_str_starts_with(const char* s, const char* prefix);
extern int8_t rae_ext_rae_str_ends_with(const char* s, const char* suffix);
extern int64_t rae_ext_rae_str_index_of(const char* s, const char* sub);
extern const char* rae_ext_rae_str_trim(const char* s);
extern double rae_ext_rae_str_to_f64(const char* s);
extern int64_t rae_ext_rae_str_to_i64(const char* s);
RAE_UNUSED static int64_t rae_length_String_(const char* this);
RAE_UNUSED static int64_t rae_compare_String_String_(const char* this, const char* other);
RAE_UNUSED static int8_t rae_equals_String_String_(const char* this, const char* other);
RAE_UNUSED static int64_t rae_hash_String_(const char* this);
RAE_UNUSED static const char* rae_concat_String_String_(const char* this, const char* other);
RAE_UNUSED static const char* rae_sub_String_Int_Int_(const char* this, int64_t start, int64_t len);
RAE_UNUSED static int8_t rae_contains_String_String_(const char* this, const char* sub);
RAE_UNUSED static int8_t rae_startsWith_String_String_(const char* this, const char* prefix);
RAE_UNUSED static int8_t rae_endsWith_String_String_(const char* this, const char* suffix);
RAE_UNUSED static int64_t rae_indexOf_String_String_(const char* this, const char* sub);
RAE_UNUSED static const char* rae_trim_String_(const char* this);
RAE_UNUSED static List rae_split_String_String_(const char* this, const char* sep);
RAE_UNUSED static const char* rae_replace_String_String_String_(const char* this, const char* old, const char* new);
RAE_UNUSED static const char* rae_join_List_String_(const List* this, const char* sep);
RAE_UNUSED static double rae_toFloat_String_(const char* this);
RAE_UNUSED static int64_t rae_toInt_String_(const char* this);
RAE_UNUSED static Timer rae_createTimer_(void);
RAE_UNUSED static void rae_start_Timer_(Timer* t);
RAE_UNUSED static void rae_stop_Timer_(Timer* t);
RAE_UNUSED static void rae_reset_Timer_(Timer* t);
RAE_UNUSED static int64_t rae_elapsedNs_Timer_(const Timer* t);
RAE_UNUSED static double rae_elapsedMs_Timer_(const Timer* t);
RAE_UNUSED static double rae_elapsedSeconds_Timer_(const Timer* t);
RAE_UNUSED static List2 rae_createList2_Int_(int64_t initialCap);
RAE_UNUSED static void rae_grow_List2_(List2* this);
RAE_UNUSED static void rae_add_List2_Any_(List2* this, RaeAny value);
RAE_UNUSED static RaeAny rae_get_List2_Int_(const List2* this, int64_t index);
RAE_UNUSED static int64_t rae_length_List2_(const List2* this);
RAE_UNUSED static List2Int rae_createList2Int_Int_(int64_t initialCap);
RAE_UNUSED static void rae_grow_List2Int_(List2Int* this);
RAE_UNUSED static void rae_add_List2Int_Int_(List2Int* this, int64_t value);
RAE_UNUSED static int64_t rae_get_List2Int_Int_(const List2Int* this, int64_t index);
RAE_UNUSED static int64_t rae_length_List2Int_(const List2Int* this);

RAE_UNUSED static double rae_toFloat_Int_(int64_t this) {
  double _ret = rae_ext_rae_int_to_float(this);
  return _ret;
}

typedef struct {
  int64_t this;
} _spawn_args_rae_toFloat_Int_;

static void* _spawn_wrapper_rae_toFloat_Int_(void* data) {
  _spawn_args_rae_toFloat_Int_* args = (_spawn_args_rae_toFloat_Int_*)data;
  rae_toFloat_Int_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_length_String_(const char* this) {
  int64_t _ret = rae_ext_rae_str_len(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_length_String_;

static void* _spawn_wrapper_rae_length_String_(void* data) {
  _spawn_args_rae_length_String_* args = (_spawn_args_rae_length_String_*)data;
  rae_length_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_compare_String_String_(const char* this, const char* other) {
  int64_t _ret = rae_ext_rae_str_compare(this, other);
  return _ret;
}

typedef struct {
  const char* this;
  const char* other;
} _spawn_args_rae_compare_String_String_;

static void* _spawn_wrapper_rae_compare_String_String_(void* data) {
  _spawn_args_rae_compare_String_String_* args = (_spawn_args_rae_compare_String_String_*)data;
  rae_compare_String_String_(args->this, args->other);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_equals_String_String_(const char* this, const char* other) {
  int8_t _ret = rae_ext_rae_str_eq(this, other);
  return _ret;
}

typedef struct {
  const char* this;
  const char* other;
} _spawn_args_rae_equals_String_String_;

static void* _spawn_wrapper_rae_equals_String_String_(void* data) {
  _spawn_args_rae_equals_String_String_* args = (_spawn_args_rae_equals_String_String_*)data;
  rae_equals_String_String_(args->this, args->other);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_hash_String_(const char* this) {
  int64_t _ret = rae_ext_rae_str_hash(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_hash_String_;

static void* _spawn_wrapper_rae_hash_String_(void* data) {
  _spawn_args_rae_hash_String_* args = (_spawn_args_rae_hash_String_*)data;
  rae_hash_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_concat_String_String_(const char* this, const char* other) {
  const char* _ret = rae_ext_rae_str_concat(this, other);
  return _ret;
}

typedef struct {
  const char* this;
  const char* other;
} _spawn_args_rae_concat_String_String_;

static void* _spawn_wrapper_rae_concat_String_String_(void* data) {
  _spawn_args_rae_concat_String_String_* args = (_spawn_args_rae_concat_String_String_*)data;
  rae_concat_String_String_(args->this, args->other);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_sub_String_Int_Int_(const char* this, int64_t start, int64_t len) {
  const char* _ret = rae_ext_rae_str_sub(this, start, len);
  return _ret;
}

typedef struct {
  const char* this;
  int64_t start;
  int64_t len;
} _spawn_args_rae_sub_String_Int_Int_;

static void* _spawn_wrapper_rae_sub_String_Int_Int_(void* data) {
  _spawn_args_rae_sub_String_Int_Int_* args = (_spawn_args_rae_sub_String_Int_Int_*)data;
  rae_sub_String_Int_Int_(args->this, args->start, args->len);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_contains_String_String_(const char* this, const char* sub) {
  int8_t _ret = rae_ext_rae_str_contains(this, sub);
  return _ret;
}

typedef struct {
  const char* this;
  const char* sub;
} _spawn_args_rae_contains_String_String_;

static void* _spawn_wrapper_rae_contains_String_String_(void* data) {
  _spawn_args_rae_contains_String_String_* args = (_spawn_args_rae_contains_String_String_*)data;
  rae_contains_String_String_(args->this, args->sub);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_startsWith_String_String_(const char* this, const char* prefix) {
  int8_t _ret = rae_ext_rae_str_starts_with(this, prefix);
  return _ret;
}

typedef struct {
  const char* this;
  const char* prefix;
} _spawn_args_rae_startsWith_String_String_;

static void* _spawn_wrapper_rae_startsWith_String_String_(void* data) {
  _spawn_args_rae_startsWith_String_String_* args = (_spawn_args_rae_startsWith_String_String_*)data;
  rae_startsWith_String_String_(args->this, args->prefix);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_endsWith_String_String_(const char* this, const char* suffix) {
  int8_t _ret = rae_ext_rae_str_ends_with(this, suffix);
  return _ret;
}

typedef struct {
  const char* this;
  const char* suffix;
} _spawn_args_rae_endsWith_String_String_;

static void* _spawn_wrapper_rae_endsWith_String_String_(void* data) {
  _spawn_args_rae_endsWith_String_String_* args = (_spawn_args_rae_endsWith_String_String_*)data;
  rae_endsWith_String_String_(args->this, args->suffix);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_indexOf_String_String_(const char* this, const char* sub) {
  int64_t _ret = rae_ext_rae_str_index_of(this, sub);
  return _ret;
}

typedef struct {
  const char* this;
  const char* sub;
} _spawn_args_rae_indexOf_String_String_;

static void* _spawn_wrapper_rae_indexOf_String_String_(void* data) {
  _spawn_args_rae_indexOf_String_String_* args = (_spawn_args_rae_indexOf_String_String_*)data;
  rae_indexOf_String_String_(args->this, args->sub);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_trim_String_(const char* this) {
  const char* _ret = rae_ext_rae_str_trim(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_trim_String_;

static void* _spawn_wrapper_rae_trim_String_(void* data) {
  _spawn_args_rae_trim_String_* args = (_spawn_args_rae_trim_String_*)data;
  rae_trim_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static List rae_split_String_String_(const char* this, const char* sep) {
  List_String_ result = rae_createList_Int_(4);
  if (rae_length_String_(sep) == 0) {
  rae_add_List_T_(&(result), rae_any(this));
  List _ret = result;
  return _ret;
  }
  const char* remaining = this;
  {
  while (1) {
  int64_t idx = rae_indexOf_String_String_(remaining, sep);
  if (idx == (-1)) {
  rae_add_List_T_(&(result), rae_any(remaining));
  List _ret = result;
  return _ret;
  }
  const char* part = rae_sub_String_Int_Int_(remaining, 0, idx);
  rae_add_List_T_(&(result), rae_any(part));
  remaining = rae_sub_String_Int_Int_(remaining, idx + rae_length_String_(sep), rae_length_String_(remaining) - idx - rae_length_String_(sep));
  }
  }
  List _ret = result;
  return _ret;
}

typedef struct {
  const char* this;
  const char* sep;
} _spawn_args_rae_split_String_String_;

static void* _spawn_wrapper_rae_split_String_String_(void* data) {
  _spawn_args_rae_split_String_String_* args = (_spawn_args_rae_split_String_String_*)data;
  rae_split_String_String_(args->this, args->sep);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_replace_String_String_String_(const char* this, const char* old, const char* new) {
  if (rae_length_String_(old) == 0) {
  const char* _ret = this;
  return _ret;
  }
  List_String_ parts = rae_split_String_String_(this, old);
  const char* _ret = rae_join_List_String_(&(parts), new);
  return _ret;
}

typedef struct {
  const char* this;
  const char* old;
  const char* new;
} _spawn_args_rae_replace_String_String_String_;

static void* _spawn_wrapper_rae_replace_String_String_String_(void* data) {
  _spawn_args_rae_replace_String_String_String_* args = (_spawn_args_rae_replace_String_String_String_*)data;
  rae_replace_String_String_String_(args->this, args->old, args->new);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_join_List_String_(const List* this, const char* sep) {
  if (rae_length_List_(this) == 0) {
  const char* _ret = "";
  return _ret;
  }
  const char* result = ((const char*)(rae_get_List_Int_(this, 0)).as.s);
  int64_t i = 1;
  {
  while (i < rae_length_List_(this)) {
  result = rae_concat_String_String_(rae_concat_String_String_(result, sep), ((const char*)(rae_get_List_Int_(this, i)).as.s));
  i = i + 1;
  }
  }
  const char* _ret = result;
  return _ret;
}

typedef struct {
  List_String_* this;
  const char* sep;
} _spawn_args_rae_join_List_String_;

static void* _spawn_wrapper_rae_join_List_String_(void* data) {
  _spawn_args_rae_join_List_String_* args = (_spawn_args_rae_join_List_String_*)data;
  rae_join_List_String_(args->this, args->sep);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_toFloat_String_(const char* this) {
  double _ret = rae_ext_rae_str_to_f64(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_toFloat_String_;

static void* _spawn_wrapper_rae_toFloat_String_(void* data) {
  _spawn_args_rae_toFloat_String_* args = (_spawn_args_rae_toFloat_String_*)data;
  rae_toFloat_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_toInt_String_(const char* this) {
  int64_t _ret = rae_ext_rae_str_to_i64(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_toInt_String_;

static void* _spawn_wrapper_rae_toInt_String_(void* data) {
  _spawn_args_rae_toInt_String_* args = (_spawn_args_rae_toInt_String_*)data;
  rae_toInt_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static Timer rae_createTimer_(void) {
  Timer _ret = (Timer){ .startTime = 0, .accumulated = 0, .running = 0 };
  return _ret;
}

typedef struct {
} _spawn_args_rae_createTimer_;

static void* _spawn_wrapper_rae_createTimer_(void* data) {
  _spawn_args_rae_createTimer_* args = (_spawn_args_rae_createTimer_*)data;
  rae_createTimer_();
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_start_Timer_(Timer* t) {
  if (t->running) {
  return;
  }
  t->startTime = rae_ext_nowNs();
  t->running = 1;
}

typedef struct {
  Timer* t;
} _spawn_args_rae_start_Timer_;

static void* _spawn_wrapper_rae_start_Timer_(void* data) {
  _spawn_args_rae_start_Timer_* args = (_spawn_args_rae_start_Timer_*)data;
  rae_start_Timer_(args->t);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_stop_Timer_(Timer* t) {
  if (t->running == 0) {
  return;
  }
  int64_t now = rae_ext_nowNs();
  t->accumulated = t->accumulated + (now - t->startTime);
  t->running = 0;
}

typedef struct {
  Timer* t;
} _spawn_args_rae_stop_Timer_;

static void* _spawn_wrapper_rae_stop_Timer_(void* data) {
  _spawn_args_rae_stop_Timer_* args = (_spawn_args_rae_stop_Timer_*)data;
  rae_stop_Timer_(args->t);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_reset_Timer_(Timer* t) {
  t->startTime = 0;
  t->accumulated = 0;
  t->running = 0;
}

typedef struct {
  Timer* t;
} _spawn_args_rae_reset_Timer_;

static void* _spawn_wrapper_rae_reset_Timer_(void* data) {
  _spawn_args_rae_reset_Timer_* args = (_spawn_args_rae_reset_Timer_*)data;
  rae_reset_Timer_(args->t);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_elapsedNs_Timer_(const Timer* t) {
  int64_t total = t->accumulated;
  if (t->running) {
  int64_t now = rae_ext_nowNs();
  int64_t _ret = total + (now - t->startTime);
  return _ret;
  }
  int64_t _ret = total;
  return _ret;
}

typedef struct {
  Timer* t;
} _spawn_args_rae_elapsedNs_Timer_;

static void* _spawn_wrapper_rae_elapsedNs_Timer_(void* data) {
  _spawn_args_rae_elapsedNs_Timer_* args = (_spawn_args_rae_elapsedNs_Timer_*)data;
  rae_elapsedNs_Timer_(args->t);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_elapsedMs_Timer_(const Timer* t) {
  int64_t ns = rae_elapsedNs_Timer_(t);
  double _ret = rae_toFloat_Int_(ns) / 1000000.0;
  return _ret;
}

typedef struct {
  Timer* t;
} _spawn_args_rae_elapsedMs_Timer_;

static void* _spawn_wrapper_rae_elapsedMs_Timer_(void* data) {
  _spawn_args_rae_elapsedMs_Timer_* args = (_spawn_args_rae_elapsedMs_Timer_*)data;
  rae_elapsedMs_Timer_(args->t);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_elapsedSeconds_Timer_(const Timer* t) {
  int64_t ns = rae_elapsedNs_Timer_(t);
  double _ret = rae_toFloat_Int_(ns) / 1000000000.0;
  return _ret;
}

typedef struct {
  Timer* t;
} _spawn_args_rae_elapsedSeconds_Timer_;

static void* _spawn_wrapper_rae_elapsedSeconds_Timer_(void* data) {
  _spawn_args_rae_elapsedSeconds_Timer_* args = (_spawn_args_rae_elapsedSeconds_Timer_*)data;
  rae_elapsedSeconds_Timer_(args->t);
  free(args);
  return NULL;
}

RAE_UNUSED static List2 rae_createList2_Int_(int64_t initialCap) {
  List2 _ret = (List2){ .data = rae_ext_rae_buf_alloc(initialCap, 8), .length = 0, .capacity = initialCap };
  return _ret;
}

typedef struct {
  int64_t initialCap;
} _spawn_args_rae_createList2_Int_;

static void* _spawn_wrapper_rae_createList2_Int_(void* data) {
  _spawn_args_rae_createList2_Int_* args = (_spawn_args_rae_createList2_Int_*)data;
  rae_createList2_Int_(args->initialCap);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_grow_List2_(List2* this) {
  int64_t newCap = this->capacity * 2;
  if (newCap == 0) {
  newCap = 4;
  }
  RaeAny* newData = rae_ext_rae_buf_alloc(newCap, 8);
  rae_ext_rae_buf_copy(this->data, 0, newData, 0, this->length, 8);
  rae_ext_rae_buf_free(this->data);
  this->data = newData;
  this->capacity = newCap;
}

typedef struct {
  List2* this;
} _spawn_args_rae_grow_List2_;

static void* _spawn_wrapper_rae_grow_List2_(void* data) {
  _spawn_args_rae_grow_List2_* args = (_spawn_args_rae_grow_List2_*)data;
  rae_grow_List2_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_add_List2_Any_(List2* this, RaeAny value) {
  if (this->length == this->capacity) {
  rae_grow_List2_(this);
  }
  ((RaeAny*)(this->data))[this->length] = rae_any(value);
  this->length = this->length + 1;
}

typedef struct {
  List2* this;
  RaeAny value;
} _spawn_args_rae_add_List2_Any_;

static void* _spawn_wrapper_rae_add_List2_Any_(void* data) {
  _spawn_args_rae_add_List2_Any_* args = (_spawn_args_rae_add_List2_Any_*)data;
  rae_add_List2_Any_(args->this, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static RaeAny rae_get_List2_Int_(const List2* this, int64_t index) {
  RaeAny _ret = rae_any(((RaeAny*)(this->data))[index]);
  return _ret;
}

typedef struct {
  List2* this;
  int64_t index;
} _spawn_args_rae_get_List2_Int_;

static void* _spawn_wrapper_rae_get_List2_Int_(void* data) {
  _spawn_args_rae_get_List2_Int_* args = (_spawn_args_rae_get_List2_Int_*)data;
  rae_get_List2_Int_(args->this, args->index);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_length_List2_(const List2* this) {
  int64_t _ret = this->length;
  return _ret;
}

typedef struct {
  List2* this;
} _spawn_args_rae_length_List2_;

static void* _spawn_wrapper_rae_length_List2_(void* data) {
  _spawn_args_rae_length_List2_* args = (_spawn_args_rae_length_List2_*)data;
  rae_length_List2_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static List2Int rae_createList2Int_Int_(int64_t initialCap) {
  List2Int _ret = (List2Int){ .data = rae_ext_rae_buf_alloc(initialCap, 8), .length = 0, .capacity = initialCap };
  return _ret;
}

typedef struct {
  int64_t initialCap;
} _spawn_args_rae_createList2Int_Int_;

static void* _spawn_wrapper_rae_createList2Int_Int_(void* data) {
  _spawn_args_rae_createList2Int_Int_* args = (_spawn_args_rae_createList2Int_Int_*)data;
  rae_createList2Int_Int_(args->initialCap);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_grow_List2Int_(List2Int* this) {
  int64_t newCap = this->capacity * 2;
  if (newCap == 0) {
  newCap = 4;
  }
  int64_t* newData = rae_ext_rae_buf_alloc(newCap, 8);
  rae_ext_rae_buf_copy(this->data, 0, newData, 0, this->length, 8);
  rae_ext_rae_buf_free(this->data);
  this->data = newData;
  this->capacity = newCap;
}

typedef struct {
  List2Int* this;
} _spawn_args_rae_grow_List2Int_;

static void* _spawn_wrapper_rae_grow_List2Int_(void* data) {
  _spawn_args_rae_grow_List2Int_* args = (_spawn_args_rae_grow_List2Int_*)data;
  rae_grow_List2Int_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_add_List2Int_Int_(List2Int* this, int64_t value) {
  if (this->length == this->capacity) {
  rae_grow_List2Int_(this);
  }
  ((int64_t*)(this->data))[this->length] = value;
  this->length = this->length + 1;
}

typedef struct {
  List2Int* this;
  int64_t value;
} _spawn_args_rae_add_List2Int_Int_;

static void* _spawn_wrapper_rae_add_List2Int_Int_(void* data) {
  _spawn_args_rae_add_List2Int_Int_* args = (_spawn_args_rae_add_List2Int_Int_*)data;
  rae_add_List2Int_Int_(args->this, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_get_List2Int_Int_(const List2Int* this, int64_t index) {
  int64_t _ret = ((int64_t*)(this->data))[index];
  return _ret;
}

typedef struct {
  List2Int* this;
  int64_t index;
} _spawn_args_rae_get_List2Int_Int_;

static void* _spawn_wrapper_rae_get_List2Int_Int_(void* data) {
  _spawn_args_rae_get_List2Int_Int_* args = (_spawn_args_rae_get_List2Int_Int_*)data;
  rae_get_List2Int_Int_(args->this, args->index);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_length_List2Int_(const List2Int* this) {
  int64_t _ret = this->length;
  return _ret;
}

typedef struct {
  List2Int* this;
} _spawn_args_rae_length_List2Int_;

static void* _spawn_wrapper_rae_length_List2Int_(void* data) {
  _spawn_args_rae_length_List2Int_* args = (_spawn_args_rae_length_List2Int_*)data;
  rae_length_List2Int_(args->this);
  free(args);
  return NULL;
}

int main(void) {
  List_Int_ xs = rae_createList_Int_(4);
  rae_add_List_T_(&(xs), rae_any(10));
  rae_add_List_T_(&(xs), rae_any(20));
  rae_add_List_T_(&(xs), rae_any(30));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Length: ", rae_ext_rae_str(xs.length)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Item 0: ", rae_ext_rae_str(((int64_t)(rae_get_List_Int_(&(xs), 0)).as.i))), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Item 1: ", rae_ext_rae_str(((int64_t)(rae_get_List_Int_(&(xs), 1)).as.i))), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Item 2: ", rae_ext_rae_str(((int64_t)(rae_get_List_Int_(&(xs), 2)).as.i))), ""));
  return 0;
}

