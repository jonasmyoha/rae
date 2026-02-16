#ifndef RAE_HAS_RAYLIB
#define RAE_HAS_RAYLIB
#endif
#include <raylib.h>
#include "rae_runtime.h"

typedef enum {
  GameState_Menu,
  GameState_Playing,
  GameState_Paused,
  GameState_GameOver
} rae_GameState;

typedef enum {
  TetrominoKind_I,
  TetrominoKind_J,
  TetrominoKind_L,
  TetrominoKind_O,
  TetrominoKind_S,
  TetrominoKind_T,
  TetrominoKind_Z
} rae_TetrominoKind;

typedef struct {
  int64_t x;
  int64_t y;
} rae_Pos;

typedef struct {
  rae_TetrominoKind kind;
  rae_Pos pos;
  int64_t rotation;
} rae_Piece;

typedef struct {
  RaeAny* data;
  int64_t length;
  int64_t capacity;
} rae_List;

typedef struct {
  rae_GameState state;
  rae_List_Any_ grid;
  int64_t width;
  int64_t height;
  rae_Piece currentPiece;
  rae_TetrominoKind nextPieceKind;
  int64_t score;
  int64_t lines;
  int64_t level;
  int64_t moveTimer;
  int64_t moveDelay;
  int64_t hMoveTimer;
  int64_t hMoveDelay;
  int64_t dasDelay;
  int64_t lockTimer;
  int64_t lockDelay;
  int64_t maxLockTimer;
} rae_Game;

typedef struct rae_Buffer_Any_ rae_Buffer_Any_;
typedef struct rae_List_Any_ rae_List_Any_;
typedef struct rae_Buffer_rae_StringMapEntry_Any_ rae_Buffer_rae_StringMapEntry_Any_;
typedef struct rae_StringMapEntry_Any_ rae_StringMapEntry_Any_;
typedef struct rae_StringMap_Any_ rae_StringMap_Any_;
typedef struct rae_Buffer_rae_IntMapEntry_Any_ rae_Buffer_rae_IntMapEntry_Any_;
typedef struct rae_IntMapEntry_Any_ rae_IntMapEntry_Any_;
typedef struct rae_IntMap_Any_ rae_IntMap_Any_;

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

RAE_UNUSED static const char* rae_toJson_Color_(Color* this);
RAE_UNUSED static Color rae_fromJson_Color_(const char* json);
RAE_UNUSED static void* rae_toBinary_Color_(Color* this, int64_t* out_size);
RAE_UNUSED static Color rae_fromBinary_Color_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Vector2_(Vector2* this);
RAE_UNUSED static Vector2 rae_fromJson_Vector2_(const char* json);
RAE_UNUSED static void* rae_toBinary_Vector2_(Vector2* this, int64_t* out_size);
RAE_UNUSED static Vector2 rae_fromBinary_Vector2_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Vector3_(Vector3* this);
RAE_UNUSED static Vector3 rae_fromJson_Vector3_(const char* json);
RAE_UNUSED static void* rae_toBinary_Vector3_(Vector3* this, int64_t* out_size);
RAE_UNUSED static Vector3 rae_fromBinary_Vector3_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Camera3D_(Camera3D* this);
RAE_UNUSED static Camera3D rae_fromJson_Camera3D_(const char* json);
RAE_UNUSED static void* rae_toBinary_Camera3D_(Camera3D* this, int64_t* out_size);
RAE_UNUSED static Camera3D rae_fromBinary_Camera3D_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Texture_(Texture* this);
RAE_UNUSED static Texture rae_fromJson_Texture_(const char* json);
RAE_UNUSED static void* rae_toBinary_Texture_(Texture* this, int64_t* out_size);
RAE_UNUSED static Texture rae_fromBinary_Texture_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Pos_(rae_Pos* this);
RAE_UNUSED static rae_Pos rae_fromJson_Pos_(const char* json);
RAE_UNUSED static void* rae_toBinary_Pos_(rae_Pos* this, int64_t* out_size);
RAE_UNUSED static rae_Pos rae_fromBinary_Pos_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Piece_(rae_Piece* this);
RAE_UNUSED static rae_Piece rae_fromJson_Piece_(const char* json);
RAE_UNUSED static void* rae_toBinary_Piece_(rae_Piece* this, int64_t* out_size);
RAE_UNUSED static rae_Piece rae_fromBinary_Piece_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Game_(rae_Game* this);
RAE_UNUSED static rae_Game rae_fromJson_Game_(const char* json);
RAE_UNUSED static void* rae_toBinary_Game_(rae_Game* this, int64_t* out_size);
RAE_UNUSED static rae_Game rae_fromBinary_Game_(void* data, int64_t size);

RAE_UNUSED static const char* rae_toJson_Pos_(rae_Pos* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"x\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->x));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"y\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->y));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static rae_Pos rae_fromJson_Pos_(const char* json) {
  rae_Pos res = {0};
  RaeAny val;
  RaeAny field_val;
  val = rae_ext_json_get(json, "x");
  if (val.type == RAE_TYPE_INT) res.x = val.as.i;
  val = rae_ext_json_get(json, "y");
  if (val.type == RAE_TYPE_INT) res.y = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_Pos_(rae_Pos* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static rae_Pos rae_fromBinary_Pos_(void* data, int64_t size) {
  (void)data; (void)size;
  rae_Pos res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_Piece_(rae_Piece* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"kind\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->kind));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"pos\": ");
  res = rae_ext_rae_str_concat(res, rae_toJson_Pos_(&this->pos));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"rotation\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->rotation));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static rae_Piece rae_fromJson_Piece_(const char* json) {
  rae_Piece res = {0};
  RaeAny val;
  RaeAny field_val;
  val = rae_ext_json_get(json, "kind");
  if (val.type == RAE_TYPE_INT) res.kind = val.as.i;
  val = rae_ext_json_get(json, "pos");
  if (val.type == RAE_TYPE_STRING) res.pos = rae_fromJson_Pos_(val.as.s);
  val = rae_ext_json_get(json, "rotation");
  if (val.type == RAE_TYPE_INT) res.rotation = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_Piece_(rae_Piece* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static rae_Piece rae_fromBinary_Piece_(void* data, int64_t size) {
  (void)data; (void)size;
  rae_Piece res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_Game_(rae_Game* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"state\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->state));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"grid\": ");
  res = rae_ext_rae_str_concat(res, rae_toJson_List_(&this->grid));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"width\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->width));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"height\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->height));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"currentPiece\": ");
  res = rae_ext_rae_str_concat(res, rae_toJson_Piece_(&this->currentPiece));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"nextPieceKind\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->nextPieceKind));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"score\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->score));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"lines\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->lines));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"level\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->level));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"moveTimer\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->moveTimer));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"moveDelay\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->moveDelay));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"hMoveTimer\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->hMoveTimer));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"hMoveDelay\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->hMoveDelay));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"dasDelay\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->dasDelay));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"lockTimer\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->lockTimer));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"lockDelay\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->lockDelay));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"maxLockTimer\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_i64(this->maxLockTimer));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static rae_Game rae_fromJson_Game_(const char* json) {
  rae_Game res = {0};
  RaeAny val;
  RaeAny field_val;
  val = rae_ext_json_get(json, "state");
  if (val.type == RAE_TYPE_INT) res.state = val.as.i;
  val = rae_ext_json_get(json, "grid");
  if (val.type == RAE_TYPE_STRING) res.grid = rae_fromJson_List_(val.as.s);
  val = rae_ext_json_get(json, "width");
  if (val.type == RAE_TYPE_INT) res.width = val.as.i;
  val = rae_ext_json_get(json, "height");
  if (val.type == RAE_TYPE_INT) res.height = val.as.i;
  val = rae_ext_json_get(json, "currentPiece");
  if (val.type == RAE_TYPE_STRING) res.currentPiece = rae_fromJson_Piece_(val.as.s);
  val = rae_ext_json_get(json, "nextPieceKind");
  if (val.type == RAE_TYPE_INT) res.nextPieceKind = val.as.i;
  val = rae_ext_json_get(json, "score");
  if (val.type == RAE_TYPE_INT) res.score = val.as.i;
  val = rae_ext_json_get(json, "lines");
  if (val.type == RAE_TYPE_INT) res.lines = val.as.i;
  val = rae_ext_json_get(json, "level");
  if (val.type == RAE_TYPE_INT) res.level = val.as.i;
  val = rae_ext_json_get(json, "moveTimer");
  if (val.type == RAE_TYPE_INT) res.moveTimer = val.as.i;
  val = rae_ext_json_get(json, "moveDelay");
  if (val.type == RAE_TYPE_INT) res.moveDelay = val.as.i;
  val = rae_ext_json_get(json, "hMoveTimer");
  if (val.type == RAE_TYPE_INT) res.hMoveTimer = val.as.i;
  val = rae_ext_json_get(json, "hMoveDelay");
  if (val.type == RAE_TYPE_INT) res.hMoveDelay = val.as.i;
  val = rae_ext_json_get(json, "dasDelay");
  if (val.type == RAE_TYPE_INT) res.dasDelay = val.as.i;
  val = rae_ext_json_get(json, "lockTimer");
  if (val.type == RAE_TYPE_INT) res.lockTimer = val.as.i;
  val = rae_ext_json_get(json, "lockDelay");
  if (val.type == RAE_TYPE_INT) res.lockDelay = val.as.i;
  val = rae_ext_json_get(json, "maxLockTimer");
  if (val.type == RAE_TYPE_INT) res.maxLockTimer = val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_Game_(rae_Game* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static rae_Game rae_fromBinary_Game_(void* data, int64_t size) {
  (void)data; (void)size;
  rae_Game res = {0};
  return res;
}

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

extern int64_t rae_ext_nextTick(void);
extern int64_t rae_ext_nowMs(void);
extern int64_t rae_ext_nowNs(void);
extern void rae_ext_rae_sleep(int64_t ms);
extern double rae_ext_rae_int_to_float(int64_t i);
extern int64_t rae_ext_rae_float_to_int(double f);
RAE_UNUSED static double rae_toFloat_rae_Int_(int64_t this);
RAE_UNUSED static int64_t rae_toInt_rae_Float_(double this);
RAE_UNUSED static rae_List_Any_ rae_createList_rae_Int_(int64_t initialCap);
RAE_UNUSED static void rae_grow_rae_List_Any__(rae_List_Any_* this);
RAE_UNUSED static void rae_add_rae_List_Any__RaeAny_(rae_List_Any_* this, RaeAny value);
RAE_UNUSED static RaeAny rae_get_rae_List_Any__rae_Int_(const rae_List_Any_* this, int64_t index);
RAE_UNUSED static void rae_set_rae_List_Any__rae_Int_RaeAny_(rae_List_Any_* this, int64_t index, RaeAny value);
RAE_UNUSED static void rae_insert_rae_List_Any__rae_Int_RaeAny_(rae_List_Any_* this, int64_t index, RaeAny value);
RAE_UNUSED static RaeAny rae_pop_rae_List_Any__(rae_List_Any_* this);
RAE_UNUSED static void rae_remove_rae_List_Any__rae_Int_(rae_List_Any_* this, int64_t index);
RAE_UNUSED static void rae_clear_rae_List_Any__(rae_List_Any_* this);
RAE_UNUSED static int64_t rae_length_rae_List_Any__(const rae_List_Any_* this);
RAE_UNUSED static void rae_swap_rae_List_Any__rae_Int_rae_Int_(rae_List_Any_* this, int64_t i, int64_t j);
RAE_UNUSED static void rae_free_rae_List_Any__(rae_List_Any_* this);
extern int64_t rae_ext_rae_str_hash(const char* s);
extern int8_t rae_ext_rae_str_eq(const char* a, const char* b);
RAE_UNUSED static rae_StringMap_Any_ rae_createStringMap_rae_Int_(int64_t initialCap);
RAE_UNUSED static void rae_set_rae_StringMap_Any__rae_String_RaeAny_(rae_StringMap_Any_* this, const char* k, RaeAny value);
RAE_UNUSED static RaeAny rae_get_rae_StringMap_Any__rae_String_(const rae_StringMap_Any_* this, const char* k);
RAE_UNUSED static int8_t rae_has_rae_StringMap_Any__rae_String_(const rae_StringMap_Any_* this, const char* k);
RAE_UNUSED static void rae_remove_rae_StringMap_Any__rae_String_(rae_StringMap_Any_* this, const char* k);
RAE_UNUSED static rae_List_Any_ rae_keys_rae_StringMap_Any__(const rae_StringMap_Any_* this);
RAE_UNUSED static rae_List_Any_ rae_values_rae_StringMap_Any__(const rae_StringMap_Any_* this);
RAE_UNUSED static void rae_growStringMap_rae_StringMap_Any__(rae_StringMap_Any_* this);
RAE_UNUSED static void rae_free_rae_StringMap_Any__(rae_StringMap_Any_* this);
RAE_UNUSED static rae_IntMap_Any_ rae_createIntMap_rae_Int_(int64_t initialCap);
RAE_UNUSED static void rae_set_rae_IntMap_Any__rae_Int_RaeAny_(rae_IntMap_Any_* this, int64_t k, RaeAny value);
RAE_UNUSED static RaeAny rae_get_rae_IntMap_Any__rae_Int_(const rae_IntMap_Any_* this, int64_t k);
RAE_UNUSED static int8_t rae_has_rae_IntMap_Any__rae_Int_(const rae_IntMap_Any_* this, int64_t k);
RAE_UNUSED static void rae_remove_rae_IntMap_Any__rae_Int_(rae_IntMap_Any_* this, int64_t k);
RAE_UNUSED static rae_List_Any_ rae_keys_rae_IntMap_Any__(const rae_IntMap_Any_* this);
RAE_UNUSED static rae_List_Any_ rae_values_rae_IntMap_Any__(const rae_IntMap_Any_* this);
RAE_UNUSED static void rae_growIntMap_rae_IntMap_Any__(rae_IntMap_Any_* this);
RAE_UNUSED static void rae_free_rae_IntMap_Any__(rae_IntMap_Any_* this);
extern void rae_ext_rae_seed(int64_t n);
extern double rae_ext_rae_random(void);
extern int64_t rae_ext_rae_random_int(int64_t min, int64_t max);
extern double rae_ext_rae_math_sin(double x);
extern double rae_ext_rae_math_cos(double x);
extern double rae_ext_rae_math_tan(double x);
extern double rae_ext_rae_math_asin(double x);
extern double rae_ext_rae_math_acos(double x);
extern double rae_ext_rae_math_atan(double x);
extern double rae_ext_rae_math_atan2(double y, double x);
extern double rae_ext_rae_math_sqrt(double x);
extern double rae_ext_rae_math_pow(double base, double exp);
extern double rae_ext_rae_math_exp(double x);
extern double rae_ext_rae_math_log(double x);
extern double rae_ext_rae_math_floor(double x);
extern double rae_ext_rae_math_ceil(double x);
extern double rae_ext_rae_math_round(double x);
RAE_UNUSED static double rae_PI_(void);
RAE_UNUSED static void rae_seed_rae_Int_(int64_t n);
RAE_UNUSED static double rae_random_(void);
RAE_UNUSED static int64_t rae_random_rae_Int_rae_Int_(int64_t min, int64_t max);
RAE_UNUSED static int64_t rae_abs_rae_Int_(int64_t n);
RAE_UNUSED static double rae_abs_rae_Float_(double n);
RAE_UNUSED static int64_t rae_min_rae_Int_rae_Int_(int64_t a, int64_t b);
RAE_UNUSED static int64_t rae_max_rae_Int_rae_Int_(int64_t a, int64_t b);
RAE_UNUSED static int64_t rae_clamp_rae_Int_rae_Int_rae_Int_(int64_t val, int64_t low, int64_t high);
RAE_UNUSED static double rae_lerp_rae_Float_rae_Float_rae_Float_(double a, double b, double t);
RAE_UNUSED static double rae_randomFloat_rae_Float_rae_Float_(double min, double max);
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
RAE_UNUSED static int64_t rae_length_rae_String_(const char* this);
RAE_UNUSED static int64_t rae_compare_rae_String_rae_String_(const char* this, const char* other);
RAE_UNUSED static int8_t rae_equals_rae_String_rae_String_(const char* this, const char* other);
RAE_UNUSED static int64_t rae_hash_rae_String_(const char* this);
RAE_UNUSED static const char* rae_concat_rae_String_rae_String_(const char* this, const char* other);
RAE_UNUSED static const char* rae_sub_rae_String_rae_Int_rae_Int_(const char* this, int64_t start, int64_t len);
RAE_UNUSED static int8_t rae_contains_rae_String_rae_String_(const char* this, const char* sub);
RAE_UNUSED static int8_t rae_startsWith_rae_String_rae_String_(const char* this, const char* prefix);
RAE_UNUSED static int8_t rae_endsWith_rae_String_rae_String_(const char* this, const char* suffix);
RAE_UNUSED static int64_t rae_indexOf_rae_String_rae_String_(const char* this, const char* sub);
RAE_UNUSED static const char* rae_trim_rae_String_(const char* this);
RAE_UNUSED static rae_List_Any_ rae_split_rae_String_rae_String_(const char* this, const char* sep);
RAE_UNUSED static const char* rae_replace_rae_String_rae_String_rae_String_(const char* this, const char* old, const char* new);
RAE_UNUSED static const char* rae_join_rae_List_Any__rae_String_(const rae_List_Any_* this, const char* sep);
RAE_UNUSED static double rae_toFloat_rae_String_(const char* this);
RAE_UNUSED static int64_t rae_toInt_rae_String_(const char* this);
extern void rae_ext_setConfigFlags(int64_t flags);
RAE_UNUSED int64_t rae_ext_windowShouldClose(void) {
  return WindowShouldClose();
}

RAE_UNUSED void rae_ext_closeWindow(void) {
  CloseWindow();
}

RAE_UNUSED void rae_ext_beginDrawing(void) {
  BeginDrawing();
}

RAE_UNUSED void rae_ext_endDrawing(void) {
  EndDrawing();
}

RAE_UNUSED void rae_ext_clearBackground(Color color) {
  ClearBackground((Color){ (unsigned char)color.r, (unsigned char)color.g, (unsigned char)color.b, (unsigned char)color.a });
}

RAE_UNUSED Texture rae_ext_loadTexture(const char* fileName) {
  return LoadTexture(fileName);
}

RAE_UNUSED void rae_ext_unloadTexture(Texture texture) {
  UnloadTexture((Texture){ .id = (unsigned int)texture.id, .width = (int)texture.width, .height = (int)texture.height, .mipmaps = (int)texture.mipmaps, .format = (int)texture.format });
}

RAE_UNUSED void rae_ext_drawTexture(Texture texture, double x, double y, Color tint) {
  DrawTexture((Texture){ .id = (unsigned int)texture.id, .width = (int)texture.width, .height = (int)texture.height, .mipmaps = (int)texture.mipmaps, .format = (int)texture.format }, (float)x, (float)y, (Color){ (unsigned char)tint.r, (unsigned char)tint.g, (unsigned char)tint.b, (unsigned char)tint.a });
}

RAE_UNUSED void rae_ext_drawTextureEx(Texture texture, Vector2 pos, double rotation, double scale, Color tint) {
  DrawTextureEx((Texture){ .id = (unsigned int)texture.id, .width = (int)texture.width, .height = (int)texture.height, .mipmaps = (int)texture.mipmaps, .format = (int)texture.format }, (Vector2){ (float)pos.x, (float)pos.y }, (float)rotation, (float)scale, (Color){ (unsigned char)tint.r, (unsigned char)tint.g, (unsigned char)tint.b, (unsigned char)tint.a });
}

extern void rae_ext_drawRectangle(double x, double y, double width, double height, Color color);
extern void rae_ext_drawRectangleLines(double x, double y, double width, double height, Color color);
extern void rae_ext_drawCubeWires(Vector3 pos, double width, double height, double length, Color color);
RAE_UNUSED void rae_ext_setTargetFPS(int64_t fps) {
  SetTargetFPS((int)fps);
}

RAE_UNUSED int64_t rae_ext_isKeyDown(int64_t key) {
  return IsKeyDown((int)key);
}

RAE_UNUSED int64_t rae_ext_isKeyPressed(int64_t key) {
  return IsKeyPressed((int)key);
}

RAE_UNUSED int64_t rae_ext_getScreenWidth(void) {
  return GetScreenWidth();
}

RAE_UNUSED int64_t rae_ext_getScreenHeight(void) {
  return GetScreenHeight();
}

extern double rae_ext_getTime(void);
extern Color rae_ext_colorFromHSV(double hue, double saturation, double value);
RAE_UNUSED static Color rae_getModernColor_rae_Int_(int64_t index);
RAE_UNUSED static int64_t rae_getKindId_rae_TetrominoKind_(rae_TetrominoKind kind);
RAE_UNUSED static int64_t rae_isOccupied_rae_Game_rae_Int_rae_Int_(const rae_Game* g, int64_t x, int64_t y);
RAE_UNUSED static int64_t rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(rae_TetrominoKind kind, int64_t rot, int64_t px, int64_t py);
RAE_UNUSED static int64_t rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(const rae_Game* g, int64_t dx, int64_t dy, int64_t dr);
RAE_UNUSED static void rae_lockPiece_rae_Game_(rae_Game* g);
RAE_UNUSED static void rae_checkLines_rae_Game_(rae_Game* g);
RAE_UNUSED static rae_TetrominoKind rae_randomKind_(void);
RAE_UNUSED static void rae_spawnPiece_rae_Game_(rae_Game* g);
RAE_UNUSED static void rae_initGame_rae_Game_(rae_Game* g);
RAE_UNUSED static void rae_draw_rae_Game_(const rae_Game* g);
RAE_UNUSED static void rae_handleInput_rae_Game_(rae_Game* g);

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

RAE_UNUSED static int64_t rae_toInt_rae_Float_(double this) {
  int64_t _ret = rae_ext_rae_float_to_int(this);
  return _ret;
}

typedef struct {
  double this;
} _spawn_args_rae_toInt_rae_Float_;

static void* _spawn_wrapper_rae_toInt_rae_Float_(void* data) {
  _spawn_args_rae_toInt_rae_Float_* args = (_spawn_args_rae_toInt_rae_Float_*)data;
  rae_toInt_rae_Float_(args->this);
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

RAE_UNUSED static void rae_add_rae_List_Any__RaeAny_(rae_List_Any_* this, RaeAny value) {
  if (this->length == this->capacity) {
  rae_grow_rae_List_Any__(this);
  }
  ((RaeAny*)( this->data))[this->length] = rae_any(value);
  this->length = this->length + 1;
}

typedef struct {
  rae_List_Any_* this;
  RaeAny value;
} _spawn_args_rae_add_rae_List_Any__RaeAny_;

static void* _spawn_wrapper_rae_add_rae_List_Any__RaeAny_(void* data) {
  _spawn_args_rae_add_rae_List_Any__RaeAny_* args = (_spawn_args_rae_add_rae_List_Any__RaeAny_*)data;
  rae_add_rae_List_Any__RaeAny_(args->this, args->value);
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

RAE_UNUSED static void rae_set_rae_List_Any__rae_Int_RaeAny_(rae_List_Any_* this, int64_t index, RaeAny value) {
  if (index < 0 || index >= this->length) {
  return;
  }
  ((RaeAny*)( this->data))[index] = rae_any(value);
}

typedef struct {
  rae_List_Any_* this;
  int64_t index;
  RaeAny value;
} _spawn_args_rae_set_rae_List_Any__rae_Int_RaeAny_;

static void* _spawn_wrapper_rae_set_rae_List_Any__rae_Int_RaeAny_(void* data) {
  _spawn_args_rae_set_rae_List_Any__rae_Int_RaeAny_* args = (_spawn_args_rae_set_rae_List_Any__rae_Int_RaeAny_*)data;
  rae_set_rae_List_Any__rae_Int_RaeAny_(args->this, args->index, args->value);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_insert_rae_List_Any__rae_Int_RaeAny_(rae_List_Any_* this, int64_t index, RaeAny value) {
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
} _spawn_args_rae_insert_rae_List_Any__rae_Int_RaeAny_;

static void* _spawn_wrapper_rae_insert_rae_List_Any__rae_Int_RaeAny_(void* data) {
  _spawn_args_rae_insert_rae_List_Any__rae_Int_RaeAny_* args = (_spawn_args_rae_insert_rae_List_Any__rae_Int_RaeAny_*)data;
  rae_insert_rae_List_Any__rae_Int_RaeAny_(args->this, args->index, args->value);
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

RAE_UNUSED static void rae_set_rae_StringMap_Any__rae_String_RaeAny_(rae_StringMap_Any_* this, const char* k, RaeAny value) {
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
} _spawn_args_rae_set_rae_StringMap_Any__rae_String_RaeAny_;

static void* _spawn_wrapper_rae_set_rae_StringMap_Any__rae_String_RaeAny_(void* data) {
  _spawn_args_rae_set_rae_StringMap_Any__rae_String_RaeAny_* args = (_spawn_args_rae_set_rae_StringMap_Any__rae_String_RaeAny_*)data;
  rae_set_rae_StringMap_Any__rae_String_RaeAny_(args->this, args->k, args->value);
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

RAE_UNUSED static rae_List_Any_ rae_keys_rae_StringMap_Any__(const rae_StringMap_Any_* this) {
  rae_List_Any_ result = ((union { rae_List_Any_ src; rae_List_Any_ dst; }){ .src = rae_createList_rae_Int_(this->length) }).dst;
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__RaeAny_(((rae_List_Any_*)&(result)), rae_any(entry.k));
  }
  i = i + 1;
  }
  }
  rae_List_Any_ _ret = result;
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
  rae_List_Any_ result = ((union { rae_List_Any_ src; rae_List_Any_ dst; }){ .src = rae_createList_rae_Int_(this->length) }).dst;
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_StringMapEntry_Any_ entry = ((rae_StringMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__RaeAny_(((rae_List_Any_*)&(result)), entry.value);
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
  rae_set_rae_StringMap_Any__rae_String_RaeAny_(this, entry.k, entry.value);
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

RAE_UNUSED static void rae_set_rae_IntMap_Any__rae_Int_RaeAny_(rae_IntMap_Any_* this, int64_t k, RaeAny value) {
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
} _spawn_args_rae_set_rae_IntMap_Any__rae_Int_RaeAny_;

static void* _spawn_wrapper_rae_set_rae_IntMap_Any__rae_Int_RaeAny_(void* data) {
  _spawn_args_rae_set_rae_IntMap_Any__rae_Int_RaeAny_* args = (_spawn_args_rae_set_rae_IntMap_Any__rae_Int_RaeAny_*)data;
  rae_set_rae_IntMap_Any__rae_Int_RaeAny_(args->this, args->k, args->value);
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

RAE_UNUSED static rae_List_Any_ rae_keys_rae_IntMap_Any__(const rae_IntMap_Any_* this) {
  rae_List_Any_ result = ((union { rae_List_Any_ src; rae_List_Any_ dst; }){ .src = rae_createList_rae_Int_(this->length) }).dst;
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__RaeAny_(((rae_List_Any_*)&(result)), rae_any(entry.k));
  }
  i = i + 1;
  }
  }
  rae_List_Any_ _ret = result;
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
  rae_List_Any_ result = ((union { rae_List_Any_ src; rae_List_Any_ dst; }){ .src = rae_createList_rae_Int_(this->length) }).dst;
  int64_t i = 0;
  {
  while (i < this->capacity) {
  rae_IntMapEntry_Any_ entry = ((rae_IntMapEntry_Any_*)( this->data))[i];
  if (entry.occupied) {
  rae_add_rae_List_Any__RaeAny_(((rae_List_Any_*)&(result)), entry.value);
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
  rae_set_rae_IntMap_Any__rae_Int_RaeAny_(this, entry.k, entry.value);
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

RAE_UNUSED static double rae_PI_(void) {
  double _ret = 3.14159265358979323846;
  return _ret;
}

typedef struct {
} _spawn_args_rae_PI_;

static void* _spawn_wrapper_rae_PI_(void* data) {
  _spawn_args_rae_PI_* args = (_spawn_args_rae_PI_*)data;
  rae_PI_();
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_seed_rae_Int_(int64_t n) {
  rae_ext_rae_seed(n);
}

typedef struct {
  int64_t n;
} _spawn_args_rae_seed_rae_Int_;

static void* _spawn_wrapper_rae_seed_rae_Int_(void* data) {
  _spawn_args_rae_seed_rae_Int_* args = (_spawn_args_rae_seed_rae_Int_*)data;
  rae_seed_rae_Int_(args->n);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_random_(void) {
  double _ret = rae_ext_rae_random();
  return _ret;
}

typedef struct {
} _spawn_args_rae_random_;

static void* _spawn_wrapper_rae_random_(void* data) {
  _spawn_args_rae_random_* args = (_spawn_args_rae_random_*)data;
  rae_random_();
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_random_rae_Int_rae_Int_(int64_t min, int64_t max) {
  int64_t _ret = rae_ext_rae_random_int(min, max);
  return _ret;
}

typedef struct {
  int64_t min;
  int64_t max;
} _spawn_args_rae_random_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_random_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_random_rae_Int_rae_Int_* args = (_spawn_args_rae_random_rae_Int_rae_Int_*)data;
  rae_random_rae_Int_rae_Int_(args->min, args->max);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_abs_rae_Int_(int64_t n) {
  if (n < 0) {
  int64_t _ret = (-n);
  return _ret;
  }
  int64_t _ret = n;
  return _ret;
}

typedef struct {
  int64_t n;
} _spawn_args_rae_abs_rae_Int_;

static void* _spawn_wrapper_rae_abs_rae_Int_(void* data) {
  _spawn_args_rae_abs_rae_Int_* args = (_spawn_args_rae_abs_rae_Int_*)data;
  rae_abs_rae_Int_(args->n);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_abs_rae_Float_(double n) {
  if (n < 0.0) {
  double _ret = (-n);
  return _ret;
  }
  double _ret = n;
  return _ret;
}

typedef struct {
  double n;
} _spawn_args_rae_abs_rae_Float_;

static void* _spawn_wrapper_rae_abs_rae_Float_(void* data) {
  _spawn_args_rae_abs_rae_Float_* args = (_spawn_args_rae_abs_rae_Float_*)data;
  rae_abs_rae_Float_(args->n);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_min_rae_Int_rae_Int_(int64_t a, int64_t b) {
  if (a < b) {
  int64_t _ret = a;
  return _ret;
  }
  int64_t _ret = b;
  return _ret;
}

typedef struct {
  int64_t a;
  int64_t b;
} _spawn_args_rae_min_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_min_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_min_rae_Int_rae_Int_* args = (_spawn_args_rae_min_rae_Int_rae_Int_*)data;
  rae_min_rae_Int_rae_Int_(args->a, args->b);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_max_rae_Int_rae_Int_(int64_t a, int64_t b) {
  if (a > b) {
  int64_t _ret = a;
  return _ret;
  }
  int64_t _ret = b;
  return _ret;
}

typedef struct {
  int64_t a;
  int64_t b;
} _spawn_args_rae_max_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_max_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_max_rae_Int_rae_Int_* args = (_spawn_args_rae_max_rae_Int_rae_Int_*)data;
  rae_max_rae_Int_rae_Int_(args->a, args->b);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_clamp_rae_Int_rae_Int_rae_Int_(int64_t val, int64_t low, int64_t high) {
  if (val < low) {
  int64_t _ret = low;
  return _ret;
  }
  if (val > high) {
  int64_t _ret = high;
  return _ret;
  }
  int64_t _ret = val;
  return _ret;
}

typedef struct {
  int64_t val;
  int64_t low;
  int64_t high;
} _spawn_args_rae_clamp_rae_Int_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_clamp_rae_Int_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_clamp_rae_Int_rae_Int_rae_Int_* args = (_spawn_args_rae_clamp_rae_Int_rae_Int_rae_Int_*)data;
  rae_clamp_rae_Int_rae_Int_rae_Int_(args->val, args->low, args->high);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_lerp_rae_Float_rae_Float_rae_Float_(double a, double b, double t) {
  double _ret = a + (b - a) * t;
  return _ret;
}

typedef struct {
  double a;
  double b;
  double t;
} _spawn_args_rae_lerp_rae_Float_rae_Float_rae_Float_;

static void* _spawn_wrapper_rae_lerp_rae_Float_rae_Float_rae_Float_(void* data) {
  _spawn_args_rae_lerp_rae_Float_rae_Float_rae_Float_* args = (_spawn_args_rae_lerp_rae_Float_rae_Float_rae_Float_*)data;
  rae_lerp_rae_Float_rae_Float_rae_Float_(args->a, args->b, args->t);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_randomFloat_rae_Float_rae_Float_(double min, double max) {
  double _ret = min + rae_ext_rae_random() * (max - min);
  return _ret;
}

typedef struct {
  double min;
  double max;
} _spawn_args_rae_randomFloat_rae_Float_rae_Float_;

static void* _spawn_wrapper_rae_randomFloat_rae_Float_rae_Float_(void* data) {
  _spawn_args_rae_randomFloat_rae_Float_rae_Float_* args = (_spawn_args_rae_randomFloat_rae_Float_rae_Float_*)data;
  rae_randomFloat_rae_Float_rae_Float_(args->min, args->max);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_length_rae_String_(const char* this) {
  int64_t _ret = rae_ext_rae_str_len(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_length_rae_String_;

static void* _spawn_wrapper_rae_length_rae_String_(void* data) {
  _spawn_args_rae_length_rae_String_* args = (_spawn_args_rae_length_rae_String_*)data;
  rae_length_rae_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_compare_rae_String_rae_String_(const char* this, const char* other) {
  int64_t _ret = rae_ext_rae_str_compare(this, other);
  return _ret;
}

typedef struct {
  const char* this;
  const char* other;
} _spawn_args_rae_compare_rae_String_rae_String_;

static void* _spawn_wrapper_rae_compare_rae_String_rae_String_(void* data) {
  _spawn_args_rae_compare_rae_String_rae_String_* args = (_spawn_args_rae_compare_rae_String_rae_String_*)data;
  rae_compare_rae_String_rae_String_(args->this, args->other);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_equals_rae_String_rae_String_(const char* this, const char* other) {
  int8_t _ret = rae_ext_rae_str_eq(this, other);
  return _ret;
}

typedef struct {
  const char* this;
  const char* other;
} _spawn_args_rae_equals_rae_String_rae_String_;

static void* _spawn_wrapper_rae_equals_rae_String_rae_String_(void* data) {
  _spawn_args_rae_equals_rae_String_rae_String_* args = (_spawn_args_rae_equals_rae_String_rae_String_*)data;
  rae_equals_rae_String_rae_String_(args->this, args->other);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_hash_rae_String_(const char* this) {
  int64_t _ret = rae_ext_rae_str_hash(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_hash_rae_String_;

static void* _spawn_wrapper_rae_hash_rae_String_(void* data) {
  _spawn_args_rae_hash_rae_String_* args = (_spawn_args_rae_hash_rae_String_*)data;
  rae_hash_rae_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_concat_rae_String_rae_String_(const char* this, const char* other) {
  const char* _ret = rae_ext_rae_str_concat(this, other);
  return _ret;
}

typedef struct {
  const char* this;
  const char* other;
} _spawn_args_rae_concat_rae_String_rae_String_;

static void* _spawn_wrapper_rae_concat_rae_String_rae_String_(void* data) {
  _spawn_args_rae_concat_rae_String_rae_String_* args = (_spawn_args_rae_concat_rae_String_rae_String_*)data;
  rae_concat_rae_String_rae_String_(args->this, args->other);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_sub_rae_String_rae_Int_rae_Int_(const char* this, int64_t start, int64_t len) {
  const char* _ret = rae_ext_rae_str_sub(this, start, len);
  return _ret;
}

typedef struct {
  const char* this;
  int64_t start;
  int64_t len;
} _spawn_args_rae_sub_rae_String_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_sub_rae_String_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_sub_rae_String_rae_Int_rae_Int_* args = (_spawn_args_rae_sub_rae_String_rae_Int_rae_Int_*)data;
  rae_sub_rae_String_rae_Int_rae_Int_(args->this, args->start, args->len);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_contains_rae_String_rae_String_(const char* this, const char* sub) {
  int8_t _ret = rae_ext_rae_str_contains(this, sub);
  return _ret;
}

typedef struct {
  const char* this;
  const char* sub;
} _spawn_args_rae_contains_rae_String_rae_String_;

static void* _spawn_wrapper_rae_contains_rae_String_rae_String_(void* data) {
  _spawn_args_rae_contains_rae_String_rae_String_* args = (_spawn_args_rae_contains_rae_String_rae_String_*)data;
  rae_contains_rae_String_rae_String_(args->this, args->sub);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_startsWith_rae_String_rae_String_(const char* this, const char* prefix) {
  int8_t _ret = rae_ext_rae_str_starts_with(this, prefix);
  return _ret;
}

typedef struct {
  const char* this;
  const char* prefix;
} _spawn_args_rae_startsWith_rae_String_rae_String_;

static void* _spawn_wrapper_rae_startsWith_rae_String_rae_String_(void* data) {
  _spawn_args_rae_startsWith_rae_String_rae_String_* args = (_spawn_args_rae_startsWith_rae_String_rae_String_*)data;
  rae_startsWith_rae_String_rae_String_(args->this, args->prefix);
  free(args);
  return NULL;
}

RAE_UNUSED static int8_t rae_endsWith_rae_String_rae_String_(const char* this, const char* suffix) {
  int8_t _ret = rae_ext_rae_str_ends_with(this, suffix);
  return _ret;
}

typedef struct {
  const char* this;
  const char* suffix;
} _spawn_args_rae_endsWith_rae_String_rae_String_;

static void* _spawn_wrapper_rae_endsWith_rae_String_rae_String_(void* data) {
  _spawn_args_rae_endsWith_rae_String_rae_String_* args = (_spawn_args_rae_endsWith_rae_String_rae_String_*)data;
  rae_endsWith_rae_String_rae_String_(args->this, args->suffix);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_indexOf_rae_String_rae_String_(const char* this, const char* sub) {
  int64_t _ret = rae_ext_rae_str_index_of(this, sub);
  return _ret;
}

typedef struct {
  const char* this;
  const char* sub;
} _spawn_args_rae_indexOf_rae_String_rae_String_;

static void* _spawn_wrapper_rae_indexOf_rae_String_rae_String_(void* data) {
  _spawn_args_rae_indexOf_rae_String_rae_String_* args = (_spawn_args_rae_indexOf_rae_String_rae_String_*)data;
  rae_indexOf_rae_String_rae_String_(args->this, args->sub);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_trim_rae_String_(const char* this) {
  const char* _ret = rae_ext_rae_str_trim(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_trim_rae_String_;

static void* _spawn_wrapper_rae_trim_rae_String_(void* data) {
  _spawn_args_rae_trim_rae_String_* args = (_spawn_args_rae_trim_rae_String_*)data;
  rae_trim_rae_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_List_Any_ rae_split_rae_String_rae_String_(const char* this, const char* sep) {
  rae_List_Any_ result = ((union { rae_List_Any_ src; rae_List_Any_ dst; }){ .src = rae_createList_rae_Int_(4) }).dst;
  if (rae_length_rae_String_(sep) == 0) {
  rae_add_rae_List_Any__RaeAny_(((rae_List_Any_*)&(result)), rae_any(this));
  rae_List_Any_ _ret = result;
  return _ret;
  }
  const char* remaining = this;
  {
  while (1) {
  int64_t idx = rae_indexOf_rae_String_rae_String_(remaining, sep);
  if (idx == (-1)) {
  rae_add_rae_List_Any__RaeAny_(((rae_List_Any_*)&(result)), rae_any(remaining));
  rae_List_Any_ _ret = result;
  return _ret;
  }
  const char* part = rae_sub_rae_String_rae_Int_rae_Int_(remaining, 0, idx);
  rae_add_rae_List_Any__RaeAny_(((rae_List_Any_*)&(result)), rae_any(part));
  remaining = rae_sub_rae_String_rae_Int_rae_Int_(remaining, idx + rae_length_rae_String_(sep), rae_length_rae_String_(remaining) - idx - rae_length_rae_String_(sep));
  }
  }
  rae_List_Any_ _ret = result;
  return _ret;
}

typedef struct {
  const char* this;
  const char* sep;
} _spawn_args_rae_split_rae_String_rae_String_;

static void* _spawn_wrapper_rae_split_rae_String_rae_String_(void* data) {
  _spawn_args_rae_split_rae_String_rae_String_* args = (_spawn_args_rae_split_rae_String_rae_String_*)data;
  rae_split_rae_String_rae_String_(args->this, args->sep);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_replace_rae_String_rae_String_rae_String_(const char* this, const char* old, const char* new) {
  if (rae_length_rae_String_(old) == 0) {
  const char* _ret = this;
  return _ret;
  }
  rae_List_Any_ parts = rae_split_rae_String_rae_String_(this, old);
  const char* _ret = rae_join_rae_List_Any__rae_String_(((rae_List_Any_*)&(parts)), new);
  return _ret;
}

typedef struct {
  const char* this;
  const char* old;
  const char* new;
} _spawn_args_rae_replace_rae_String_rae_String_rae_String_;

static void* _spawn_wrapper_rae_replace_rae_String_rae_String_rae_String_(void* data) {
  _spawn_args_rae_replace_rae_String_rae_String_rae_String_* args = (_spawn_args_rae_replace_rae_String_rae_String_rae_String_*)data;
  rae_replace_rae_String_rae_String_rae_String_(args->this, args->old, args->new);
  free(args);
  return NULL;
}

RAE_UNUSED static const char* rae_join_rae_List_Any__rae_String_(const rae_List_Any_* this, const char* sep) {
  if (rae_length_rae_List_Any__(this) == 0) {
  const char* _ret = "";
  return _ret;
  }
  const char* result = ((const char*)(rae_get_rae_List_Any__rae_Int_(this, 0)).as.s);
  int64_t i = 1;
  {
  while (i < rae_length_rae_List_Any__(this)) {
  result = rae_concat_rae_String_rae_String_(rae_concat_rae_String_rae_String_(result, sep), ((const char*)(rae_get_rae_List_Any__rae_Int_(this, i)).as.s));
  i = i + 1;
  }
  }
  const char* _ret = result;
  return _ret;
}

typedef struct {
  rae_List_Any_* this;
  const char* sep;
} _spawn_args_rae_join_rae_List_Any__rae_String_;

static void* _spawn_wrapper_rae_join_rae_List_Any__rae_String_(void* data) {
  _spawn_args_rae_join_rae_List_Any__rae_String_* args = (_spawn_args_rae_join_rae_List_Any__rae_String_*)data;
  rae_join_rae_List_Any__rae_String_(args->this, args->sep);
  free(args);
  return NULL;
}

RAE_UNUSED static double rae_toFloat_rae_String_(const char* this) {
  double _ret = rae_ext_rae_str_to_f64(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_toFloat_rae_String_;

static void* _spawn_wrapper_rae_toFloat_rae_String_(void* data) {
  _spawn_args_rae_toFloat_rae_String_* args = (_spawn_args_rae_toFloat_rae_String_*)data;
  rae_toFloat_rae_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_toInt_rae_String_(const char* this) {
  int64_t _ret = rae_ext_rae_str_to_i64(this);
  return _ret;
}

typedef struct {
  const char* this;
} _spawn_args_rae_toInt_rae_String_;

static void* _spawn_wrapper_rae_toInt_rae_String_(void* data) {
  _spawn_args_rae_toInt_rae_String_* args = (_spawn_args_rae_toInt_rae_String_*)data;
  rae_toInt_rae_String_(args->this);
  free(args);
  return NULL;
}

RAE_UNUSED static Color rae_getModernColor_rae_Int_(int64_t index) {
  if (index == 1) {
  Color _ret = (Color){ .r = 100, .g = 230, .b = 255, .a = 255 };
  return _ret;
  }
  if (index == 2) {
  Color _ret = (Color){ .r = 120, .g = 160, .b = 255, .a = 255 };
  return _ret;
  }
  if (index == 3) {
  Color _ret = (Color){ .r = 255, .g = 180, .b = 120, .a = 255 };
  return _ret;
  }
  if (index == 4) {
  Color _ret = (Color){ .r = 255, .g = 230, .b = 120, .a = 255 };
  return _ret;
  }
  if (index == 5) {
  Color _ret = (Color){ .r = 120, .g = 255, .b = 160, .a = 255 };
  return _ret;
  }
  if (index == 6) {
  Color _ret = (Color){ .r = 200, .g = 160, .b = 255, .a = 255 };
  return _ret;
  }
  if (index == 7) {
  Color _ret = (Color){ .r = 255, .g = 120, .b = 140, .a = 255 };
  return _ret;
  }
  Color _ret = (Color){ .r = 40, .g = 40, .b = 50, .a = 255 };
  return _ret;
}

typedef struct {
  int64_t index;
} _spawn_args_rae_getModernColor_rae_Int_;

static void* _spawn_wrapper_rae_getModernColor_rae_Int_(void* data) {
  _spawn_args_rae_getModernColor_rae_Int_* args = (_spawn_args_rae_getModernColor_rae_Int_*)data;
  rae_getModernColor_rae_Int_(args->index);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_getKindId_rae_TetrominoKind_(rae_TetrominoKind kind) {
  if (kind == TetrominoKind_I) {
  int64_t _ret = 1;
  return _ret;
  }
  if (kind == TetrominoKind_J) {
  int64_t _ret = 2;
  return _ret;
  }
  if (kind == TetrominoKind_L) {
  int64_t _ret = 3;
  return _ret;
  }
  if (kind == TetrominoKind_O) {
  int64_t _ret = 4;
  return _ret;
  }
  if (kind == TetrominoKind_S) {
  int64_t _ret = 5;
  return _ret;
  }
  if (kind == TetrominoKind_T) {
  int64_t _ret = 6;
  return _ret;
  }
  int64_t _ret = 7;
  return _ret;
}

typedef struct {
  rae_TetrominoKind kind;
} _spawn_args_rae_getKindId_rae_TetrominoKind_;

static void* _spawn_wrapper_rae_getKindId_rae_TetrominoKind_(void* data) {
  _spawn_args_rae_getKindId_rae_TetrominoKind_* args = (_spawn_args_rae_getKindId_rae_TetrominoKind_*)data;
  rae_getKindId_rae_TetrominoKind_(args->kind);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_isOccupied_rae_Game_rae_Int_rae_Int_(const rae_Game* g, int64_t x, int64_t y) {
  if (x < 0 || x >= g->width || y >= g->height) {
  int64_t _ret = 1;
  return _ret;
  }
  if (y < 0) {
  int64_t _ret = 0;
  return _ret;
  }
  if (((int64_t)(rae_get_rae_List_Any__rae_Int_(((rae_List_Any_*)&(g->grid)), y * g->width + x)).as.i) > 0) {
  int64_t _ret = 1;
  return _ret;
  }
  int64_t _ret = 0;
  return _ret;
}

typedef struct {
  rae_Game* g;
  int64_t x;
  int64_t y;
} _spawn_args_rae_isOccupied_rae_Game_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_isOccupied_rae_Game_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_isOccupied_rae_Game_rae_Int_rae_Int_* args = (_spawn_args_rae_isOccupied_rae_Game_rae_Int_rae_Int_*)data;
  rae_isOccupied_rae_Game_rae_Int_rae_Int_(args->g, args->x, args->y);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(rae_TetrominoKind kind, int64_t rot, int64_t px, int64_t py) {
  if (kind == TetrominoKind_I) {
  if (rot == 0 || rot == 2) {
  if (px == 1) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (py == 1) {
  int64_t _ret = 1;
  return _ret;
  }
  }
  } else {
  if (kind == TetrominoKind_J) {
  if (rot == 0) {
  if (px == 1 && py >= 0 && py <= 2 || px == 0 && py == 2) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 1) {
  if (py == 1 && px >= 0 && px <= 2 || px == 0 && py == 0) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 2) {
  if (px == 1 && py >= 0 && py <= 2 || px == 2 && py == 0) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 3) {
  if (py == 1 && px >= 0 && px <= 2 || px == 2 && py == 2) {
  int64_t _ret = 1;
  return _ret;
  }
  }
  }
  }
  }
  } else {
  if (kind == TetrominoKind_L) {
  if (rot == 0) {
  if (px == 1 && py >= 0 && py <= 2 || px == 2 && py == 2) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 1) {
  if (py == 1 && px >= 0 && px <= 2 || px == 0 && py == 2) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 2) {
  if (px == 1 && py >= 0 && py <= 2 || px == 0 && py == 0) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 3) {
  if (py == 1 && px >= 0 && px <= 2 || px == 2 && py == 0) {
  int64_t _ret = 1;
  return _ret;
  }
  }
  }
  }
  }
  } else {
  if (kind == TetrominoKind_O) {
  if (px >= 1 && px <= 2 && py >= 1 && py <= 2) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (kind == TetrominoKind_S) {
  if (rot == 0 || rot == 2) {
  if (py == 1 && px >= 1 && px <= 2 || py == 2 && px >= 0 && px <= 1) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (px == 1 && py >= 0 && py <= 1 || px == 2 && py >= 1 && py <= 2) {
  int64_t _ret = 1;
  return _ret;
  }
  }
  } else {
  if (kind == TetrominoKind_T) {
  if (rot == 0) {
  if (py == 1 && px >= 0 && px <= 2 || px == 1 && py == 0) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 1) {
  if (px == 1 && py >= 0 && py <= 2 || px == 2 && py == 1) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 2) {
  if (py == 1 && px >= 0 && px <= 2 || px == 1 && py == 2) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (rot == 3) {
  if (px == 1 && py >= 0 && py <= 2 || px == 0 && py == 1) {
  int64_t _ret = 1;
  return _ret;
  }
  }
  }
  }
  }
  } else {
  if (kind == TetrominoKind_Z) {
  if (rot == 0 || rot == 2) {
  if (py == 1 && px >= 0 && px <= 1 || py == 2 && px >= 1 && px <= 2) {
  int64_t _ret = 1;
  return _ret;
  }
  } else {
  if (px == 1 && py >= 1 && py <= 2 || px == 2 && py >= 0 && py <= 1) {
  int64_t _ret = 1;
  return _ret;
  }
  }
  }
  }
  }
  }
  }
  }
  }
  int64_t _ret = 0;
  return _ret;
}

typedef struct {
  rae_TetrominoKind kind;
  int64_t rot;
  int64_t px;
  int64_t py;
} _spawn_args_rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_* args = (_spawn_args_rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_*)data;
  rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(args->kind, args->rot, args->px, args->py);
  free(args);
  return NULL;
}

RAE_UNUSED static int64_t rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(const rae_Game* g, int64_t dx, int64_t dy, int64_t dr) {
  const rae_Piece* cp = &(g->currentPiece);
  const rae_Pos* pos = &(cp->pos);
  int64_t newX = pos->x + dx;
  int64_t newY = pos->y + dy;
  int64_t newRot = (cp->rotation + dr) % 4;
  if (newRot < 0) {
  newRot = newRot + 4;
  }
  int64_t py = 0;
  {
  while (py < 4) {
  int64_t px = 0;
  {
  while (px < 4) {
  if (rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(cp->kind, newRot, px, py) == 1) {
  if (rae_isOccupied_rae_Game_rae_Int_rae_Int_(g, newX + px, newY + py) == 1) {
  int64_t _ret = 0;
  return _ret;
  }
  }
  px = px + 1;
  }
  }
  py = py + 1;
  }
  }
  int64_t _ret = 1;
  return _ret;
}

typedef struct {
  rae_Game* g;
  int64_t dx;
  int64_t dy;
  int64_t dr;
} _spawn_args_rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_;

static void* _spawn_wrapper_rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(void* data) {
  _spawn_args_rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_* args = (_spawn_args_rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_*)data;
  rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(args->g, args->dx, args->dy, args->dr);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_lockPiece_rae_Game_(rae_Game* g) {
  rae_Piece* cp = &(g->currentPiece);
  const rae_Pos* pos = &(cp->pos);
  rae_List_Any_* grid = &(g->grid);
  int64_t kindId = rae_getKindId_rae_TetrominoKind_(cp->kind);
  int64_t py = 0;
  {
  while (py < 4) {
  int64_t px = 0;
  {
  while (px < 4) {
  if (rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(cp->kind, cp->rotation, px, py) == 1) {
  int64_t gx = pos->x + px;
  int64_t gy = pos->y + py;
  if (gy >= 0 && gy < g->height) {
  rae_set_rae_List_Any__rae_Int_RaeAny_(grid, gy * g->width + gx, rae_any(kindId));
  }
  }
  px = px + 1;
  }
  }
  py = py + 1;
  }
  }
}

typedef struct {
  rae_Game* g;
} _spawn_args_rae_lockPiece_rae_Game_;

static void* _spawn_wrapper_rae_lockPiece_rae_Game_(void* data) {
  _spawn_args_rae_lockPiece_rae_Game_* args = (_spawn_args_rae_lockPiece_rae_Game_*)data;
  rae_lockPiece_rae_Game_(args->g);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_checkLines_rae_Game_(rae_Game* g) {
  rae_List_Any_* grid = &(g->grid);
  int64_t linesCleared = 0;
  int64_t y = g->height - 1;
  {
  while (y >= 0) {
  int64_t full = 1;
  int64_t x = 0;
  {
  while (x < g->width) {
  if (((int64_t)(rae_get_rae_List_Any__rae_Int_(grid, y * g->width + x)).as.i) == 0) {
  full = 0;
  }
  x = x + 1;
  }
  }
  if (full == 1) {
  linesCleared = linesCleared + 1;
  int64_t ty = y;
  {
  while (ty > 0) {
  int64_t tx = 0;
  {
  while (tx < g->width) {
  rae_set_rae_List_Any__rae_Int_RaeAny_(grid, ty * g->width + tx, rae_any(((int64_t)(rae_get_rae_List_Any__rae_Int_(grid, (ty - 1) * g->width + tx)).as.i)));
  tx = tx + 1;
  }
  }
  ty = ty - 1;
  }
  }
  int64_t tx = 0;
  {
  while (tx < g->width) {
  rae_set_rae_List_Any__rae_Int_RaeAny_(grid, tx, rae_any(0));
  tx = tx + 1;
  }
  }
  y = y + 1;
  }
  y = y - 1;
  }
  }
  if (linesCleared > 0) {
  g->lines = g->lines + linesCleared;
  g->score = g->score + linesCleared * 100 * g->level;
  int64_t newLevel = g->lines + 1;
  if (newLevel > g->level) {
  g->level = newLevel;
  g->moveDelay = 22 - g->level * 4 / 3;
  if (g->moveDelay < 2) {
  g->moveDelay = 2;
  }
  }
  }
}

typedef struct {
  rae_Game* g;
} _spawn_args_rae_checkLines_rae_Game_;

static void* _spawn_wrapper_rae_checkLines_rae_Game_(void* data) {
  _spawn_args_rae_checkLines_rae_Game_* args = (_spawn_args_rae_checkLines_rae_Game_*)data;
  rae_checkLines_rae_Game_(args->g);
  free(args);
  return NULL;
}

RAE_UNUSED static rae_TetrominoKind rae_randomKind_(void) {
  int64_t k = rae_random_rae_Int_rae_Int_(0, 6);
  if (k == 0) {
  rae_TetrominoKind _ret = TetrominoKind_I;
  return _ret;
  }
  if (k == 1) {
  rae_TetrominoKind _ret = TetrominoKind_J;
  return _ret;
  }
  if (k == 2) {
  rae_TetrominoKind _ret = TetrominoKind_L;
  return _ret;
  }
  if (k == 3) {
  rae_TetrominoKind _ret = TetrominoKind_O;
  return _ret;
  }
  if (k == 4) {
  rae_TetrominoKind _ret = TetrominoKind_S;
  return _ret;
  }
  if (k == 5) {
  rae_TetrominoKind _ret = TetrominoKind_T;
  return _ret;
  }
  rae_TetrominoKind _ret = TetrominoKind_Z;
  return _ret;
}

typedef struct {
} _spawn_args_rae_randomKind_;

static void* _spawn_wrapper_rae_randomKind_(void* data) {
  _spawn_args_rae_randomKind_* args = (_spawn_args_rae_randomKind_*)data;
  rae_randomKind_();
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_spawnPiece_rae_Game_(rae_Game* g) {
  rae_Piece* cp = &(g->currentPiece);
  cp->kind = g->nextPieceKind;
  g->nextPieceKind = rae_randomKind_();
  rae_Pos* pos = &(cp->pos);
  pos->x = g->width / 2 - 2;
  pos->y = (-1);
  cp->rotation = 0;
  g->lockTimer = 0;
  g->maxLockTimer = 0;
  g->moveTimer = 0;
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, 0, 0, 0) == 0) {
  g->state = GameState_GameOver;
  }
}

typedef struct {
  rae_Game* g;
} _spawn_args_rae_spawnPiece_rae_Game_;

static void* _spawn_wrapper_rae_spawnPiece_rae_Game_(void* data) {
  _spawn_args_rae_spawnPiece_rae_Game_* args = (_spawn_args_rae_spawnPiece_rae_Game_*)data;
  rae_spawnPiece_rae_Game_(args->g);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_initGame_rae_Game_(rae_Game* g) {
  g->state = GameState_Playing;
  g->width = 10;
  g->height = 20;
  g->score = 0;
  g->lines = 0;
  g->level = 1;
  g->moveTimer = 0;
  g->moveDelay = 21;
  g->hMoveTimer = 0;
  g->hMoveDelay = 4;
  g->dasDelay = 14;
  g->lockTimer = 0;
  g->lockDelay = 30;
  g->maxLockTimer = 0;
  g->nextPieceKind = rae_randomKind_();
  int64_t i = 0;
  rae_List_Any_* grid = &(g->grid);
  rae_clear_rae_List_Any__(grid);
  {
  while (i < g->width * g->height) {
  rae_add_rae_List_Any__RaeAny_(grid, rae_any(0));
  i = i + 1;
  }
  }
  rae_spawnPiece_rae_Game_(g);
}

typedef struct {
  rae_Game* g;
} _spawn_args_rae_initGame_rae_Game_;

static void* _spawn_wrapper_rae_initGame_rae_Game_(void* data) {
  _spawn_args_rae_initGame_rae_Game_* args = (_spawn_args_rae_initGame_rae_Game_*)data;
  rae_initGame_rae_Game_(args->g);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_draw_rae_Game_(const rae_Game* g) {
  rae_ext_beginDrawing();
  rae_ext_clearBackground((Color){ .r = 30, .g = 30, .b = 40, .a = 255 });
  double sw = rae_toFloat_rae_Int_(rae_ext_getScreenWidth());
  double sh = rae_toFloat_rae_Int_(rae_ext_getScreenHeight());
  double fontSize = sh / 25.0;
  if (fontSize < 10.0) {
  fontSize = 10.0;
  }
  double cellSize = sh * 0.8 / 20.0;
  double offsetX = (sw - rae_toFloat_rae_Int_(g->width) * cellSize) / 2.0;
  double offsetY = (sh - rae_toFloat_rae_Int_(g->height) * cellSize) / 2.0;
  rae_ext_drawRectangleLines(offsetX - 2.0, offsetY - 2.0, rae_toFloat_rae_Int_(g->width) * cellSize + 4.0, rae_toFloat_rae_Int_(g->height) * cellSize + 4.0, (Color){ .r = 180, .g = 180, .b = 255, .a = 255 });
  int64_t y = 0;
  {
  while (y < g->height) {
  int64_t x = 0;
  {
  while (x < g->width) {
  int64_t val = ((int64_t)(rae_get_rae_List_Any__rae_Int_(((rae_List_Any_*)&(g->grid)), y * g->width + x)).as.i);
  if (val > 0) {
  rae_ext_drawRectangle(rae_toFloat_rae_Int_(x) * cellSize + offsetX, rae_toFloat_rae_Int_(y) * cellSize + offsetY, cellSize - 1.0, cellSize - 1.0, rae_getModernColor_rae_Int_(val));
  }
  x = x + 1;
  }
  }
  y = y + 1;
  }
  }
  const rae_Piece* cp = &(g->currentPiece);
  const rae_Pos* pos = &(cp->pos);
  int64_t kindId = rae_getKindId_rae_TetrominoKind_(cp->kind);
  int64_t py = 0;
  {
  while (py < 4) {
  int64_t px = 0;
  {
  while (px < 4) {
  if (rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(cp->kind, cp->rotation, px, py) == 1) {
  rae_ext_drawRectangle(rae_toFloat_rae_Int_(pos->x + px) * cellSize + offsetX, rae_toFloat_rae_Int_(pos->y + py) * cellSize + offsetY, cellSize - 1.0, cellSize - 1.0, rae_getModernColor_rae_Int_(kindId));
  }
  px = px + 1;
  }
  }
  py = py + 1;
  }
  }
  double previewOffsetX = offsetX + rae_toFloat_rae_Int_(g->width) * cellSize + cellSize * 2.0;
  double previewOffsetY = offsetY + cellSize * 2.0;
  rae_ext_drawRectangleLines(previewOffsetX - 5.0, previewOffsetY - 5.0, cellSize * 4.0 + 10.0, cellSize * 4.0 + 10.0, (Color){ .r = 180, .g = 180, .b = 255, .a = 255 });
  rae_ext_drawText("NEXT", previewOffsetX, previewOffsetY - cellSize - 10.0, fontSize, (Color){ .r = 200, .g = 200, .b = 255, .a = 255 });
  int64_t nextKindId = rae_getKindId_rae_TetrominoKind_(g->nextPieceKind);
  py = 0;
  {
  while (py < 4) {
  int64_t px = 0;
  {
  while (px < 4) {
  if (rae_getShapeCell_rae_TetrominoKind_rae_Int_rae_Int_rae_Int_(g->nextPieceKind, 0, px, py) == 1) {
  rae_ext_drawRectangle(previewOffsetX + rae_toFloat_rae_Int_(px) * cellSize, previewOffsetY + rae_toFloat_rae_Int_(py) * cellSize, cellSize - 1.0, cellSize - 1.0, rae_getModernColor_rae_Int_(nextKindId));
  }
  px = px + 1;
  }
  }
  py = py + 1;
  }
  }
  rae_ext_drawText(rae_ext_rae_str_concat(rae_ext_rae_str_concat("SCORE: ", rae_ext_rae_str(g->score)), ""), 10.0, 10.0, fontSize, (Color){ .r = 255, .g = 200, .b = 200, .a = 255 });
  rae_ext_drawText(rae_ext_rae_str_concat(rae_ext_rae_str_concat("LEVEL: ", rae_ext_rae_str(g->level)), ""), 10.0, 10.0 + fontSize * 1.2, fontSize, (Color){ .r = 200, .g = 255, .b = 200, .a = 255 });
  rae_ext_drawText(rae_ext_rae_str_concat(rae_ext_rae_str_concat("LINES: ", rae_ext_rae_str(g->lines)), ""), 10.0, 10.0 + fontSize * 2.4, fontSize, (Color){ .r = 200, .g = 200, .b = 255, .a = 255 });
  if (g->state == GameState_Paused) {
  rae_ext_drawText("PAUSED", sw / 2.0 - fontSize * 3.0, sh / 2.0, fontSize * 2.0, (Color){ .r = 255, .g = 255, .b = 180, .a = 255 });
  }
  if (g->state == GameState_GameOver) {
  rae_ext_drawText("GAME OVER", sw / 2.0 - fontSize * 4.0, sh / 2.0, fontSize * 2.0, (Color){ .r = 255, .g = 150, .b = 150, .a = 255 });
  rae_ext_drawText("Press R to Restart", sw / 2.0 - fontSize * 4.0, sh / 2.0 + fontSize * 2.2, fontSize, (Color){ .r = 220, .g = 220, .b = 220, .a = 255 });
  }
  rae_ext_endDrawing();
}

typedef struct {
  rae_Game* g;
} _spawn_args_rae_draw_rae_Game_;

static void* _spawn_wrapper_rae_draw_rae_Game_(void* data) {
  _spawn_args_rae_draw_rae_Game_* args = (_spawn_args_rae_draw_rae_Game_*)data;
  rae_draw_rae_Game_(args->g);
  free(args);
  return NULL;
}

RAE_UNUSED static void rae_handleInput_rae_Game_(rae_Game* g) {
  if (g->state == (!(GameState_Playing))) {
  return;
  }
  rae_Piece* cp = &(g->currentPiece);
  rae_Pos* pos = &(cp->pos);
  int64_t moved = 0;
  int64_t moveLeft = rae_ext_isKeyDown(263) || rae_ext_isKeyDown(65);
  int64_t moveRight = rae_ext_isKeyDown(262) || rae_ext_isKeyDown(68);
  if (moveLeft == 1 || moveRight == 1) {
  if (g->hMoveTimer == 0) {
  if (moveLeft == 1) {
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, (-1), 0, 0) == 1) {
  pos->x = pos->x - 1;
  moved = 1;
  }
  } else {
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, 1, 0, 0) == 1) {
  pos->x = pos->x + 1;
  moved = 1;
  }
  }
  g->hMoveTimer = 1;
  } else {
  g->hMoveTimer = g->hMoveTimer + 1;
  if (g->hMoveTimer >= g->dasDelay) {
  if ((g->hMoveTimer - g->dasDelay) % g->hMoveDelay == 0) {
  if (moveLeft == 1) {
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, (-1), 0, 0) == 1) {
  pos->x = pos->x - 1;
  moved = 1;
  }
  } else {
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, 1, 0, 0) == 1) {
  pos->x = pos->x + 1;
  moved = 1;
  }
  }
  }
  }
  }
  } else {
  g->hMoveTimer = 0;
  }
  if (rae_ext_isKeyPressed(32) == 1 || rae_ext_isKeyPressed(265) == 1 || rae_ext_isKeyPressed(87) == 1) {
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, 0, 0, 1) == 1) {
  cp->rotation = (cp->rotation + 1) % 4;
  moved = 1;
  }
  }
  if (moved == 1) {
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, 0, 1, 0) == 0) {
  g->lockTimer = 0;
  }
  }
  int64_t effectiveDelay = g->moveDelay;
  if (rae_ext_isKeyDown(264) == 1 || rae_ext_isKeyDown(83) == 1) {
  effectiveDelay = 2;
  }
  g->moveTimer = g->moveTimer + 1;
  if (g->moveTimer >= effectiveDelay) {
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, 0, 1, 0) == 1) {
  pos->y = pos->y + 1;
  g->moveTimer = 0;
  g->lockTimer = 0;
  g->maxLockTimer = 0;
  } else {
  g->moveTimer = effectiveDelay;
  }
  }
  if (rae_canMove_rae_Game_rae_Int_rae_Int_rae_Int_(g, 0, 1, 0) == 0) {
  g->lockTimer = g->lockTimer + 1;
  g->maxLockTimer = g->maxLockTimer + 1;
  if (g->lockTimer >= g->lockDelay || g->maxLockTimer >= 120) {
  rae_lockPiece_rae_Game_(g);
  rae_checkLines_rae_Game_(g);
  rae_spawnPiece_rae_Game_(g);
  }
  }
}

typedef struct {
  rae_Game* g;
} _spawn_args_rae_handleInput_rae_Game_;

static void* _spawn_wrapper_rae_handleInput_rae_Game_(void* data) {
  _spawn_args_rae_handleInput_rae_Game_* args = (_spawn_args_rae_handleInput_rae_Game_*)data;
  rae_handleInput_rae_Game_(args->g);
  free(args);
  return NULL;
}

int main(void) {
  rae_ext_setConfigFlags(4);
  rae_ext_initWindow(600, 500, "Rae Tetris 2D");
  rae_ext_setTargetFPS(60);
  rae_Game g = (rae_Game)(rae_Game){ .state = GameState_Menu, .grid = ((union { rae_List_Any_ src; rae_List_Any_ dst; }){ .src = rae_createList_rae_Int_(200) }).dst, .width = 0, .height = 0, .currentPiece = (rae_Piece){ .kind = TetrominoKind_I, .pos = (rae_Pos){ .x = 0, .y = 0 }, .rotation = 0 }, .nextPieceKind = TetrominoKind_I, .score = 0, .lines = 0, .level = 1, .moveTimer = 0, .moveDelay = 0, .hMoveTimer = 0, .hMoveDelay = 0, .dasDelay = 0, .lockTimer = 0, .lockDelay = 0, .maxLockTimer = 0 };
  rae_initGame_rae_Game_(&(g));
  {
  while (rae_ext_windowShouldClose() == 0) {
  if (g.state == GameState_GameOver) {
  if (rae_ext_isKeyPressed(82) == 1) {
  rae_initGame_rae_Game_(&(g));
  }
  }
  if (rae_ext_isKeyPressed(80) == 1) {
  if (g.state == GameState_Playing) {
  g.state = GameState_Paused;
  } else {
  if (g.state == GameState_Paused) {
  g.state = GameState_Playing;
  }
  }
  }
  if (g.state == GameState_Playing) {
  rae_handleInput_rae_Game_(&(g));
  }
  rae_draw_rae_Game_(&(g));
  }
  }
  rae_ext_closeWindow();
  return 0;
}

