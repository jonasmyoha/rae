#ifndef RAE_RUNTIME_H
#define RAE_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GNUC__
#define RAE_UNUSED __attribute__((unused))
#else
#define RAE_UNUSED
#endif

typedef struct { int64_t v; } rae_Char;

typedef struct {
  const char* data;
  uint64_t len;
} RaeString;

typedef enum {
  RAE_TYPE_NONE,
  RAE_TYPE_INT,
  RAE_TYPE_FLOAT,
  RAE_TYPE_BOOL,
  RAE_TYPE_STRING,
  RAE_TYPE_CHAR,
  RAE_TYPE_ID,
  RAE_TYPE_KEY,
  RAE_TYPE_LIST,
  RAE_TYPE_BUFFER
} RaeType;

typedef struct {
  void* data;
  int64_t length;
  int64_t capacity;
} RaeList;

typedef struct {
  void* data;
  int64_t length;
  int64_t capacity;
} RaeMap;

typedef struct {
  RaeType type;
  bool is_view;
  bool is_mod;
  union {
    int64_t i;
    double f;
    int8_t b;
    const char* s;
    void* ptr;
  } as;
} RaeAny;

void rae_flush_stdout(void);
void* rae_ext_rae_buf_alloc(int64_t count, int64_t elem_size);
void rae_ext_rae_buf_free(void* buf);
void* rae_ext_rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size);
void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elem_size);

void rae_ext_rae_log_any(RaeAny value);
void rae_ext_rae_log_stream_any(RaeAny value);

/* Conversion Helpers */
RAE_UNUSED static RaeAny rae_any_int(int64_t v) { return (RaeAny){RAE_TYPE_INT, false, false, {.i = v}}; }
RAE_UNUSED static RaeAny rae_any_int_ptr(const int64_t* v) { return (RaeAny){RAE_TYPE_INT, true, false, {.i = *v}}; }
RAE_UNUSED static RaeAny rae_any_float(double v) { return (RaeAny){RAE_TYPE_FLOAT, false, false, {.f = v}}; }
RAE_UNUSED static RaeAny rae_any_float_ptr(const double* v) { return (RaeAny){RAE_TYPE_FLOAT, true, false, {.f = *v}}; }
RAE_UNUSED static RaeAny rae_any_bool(bool v) { return (RaeAny){RAE_TYPE_BOOL, false, false, {.b = v ? 1 : 0}}; }
RAE_UNUSED static RaeAny rae_any_bool_ptr(const bool* v) { return (RaeAny){RAE_TYPE_BOOL, true, false, {.b = *v ? 1 : 0}}; }
RAE_UNUSED static RaeAny rae_any_char(rae_Char v) { return (RaeAny){RAE_TYPE_CHAR, false, false, {.i = v.v}}; }
RAE_UNUSED static RaeAny rae_any_char_ptr(const rae_Char* v) { return (RaeAny){RAE_TYPE_CHAR, true, false, {.i = v->v}}; }
RAE_UNUSED static RaeAny rae_any_string(const char* v) { return (RaeAny){RAE_TYPE_STRING, false, false, {.s = v}}; }
RAE_UNUSED static RaeAny rae_any_string_ptr(const char* const* v) { return (RaeAny){RAE_TYPE_STRING, true, false, {.s = *v}}; }

// Special wrappers for Rae View/Mod structs
typedef struct { int64_t* ptr; } rae_View_Int;
typedef struct { int64_t* ptr; } rae_Mod_Int;
typedef struct { double* ptr; } rae_View_Float;
typedef struct { double* ptr; } rae_Mod_Float;
typedef struct { int8_t* ptr; } rae_View_Bool;
typedef struct { int8_t* ptr; } rae_Mod_Bool;
typedef struct { rae_Char* ptr; } rae_View_Char;
typedef struct { rae_Char* ptr; } rae_Mod_Char;
typedef struct { const char** ptr; } rae_View_String;
typedef struct { const char** ptr; } rae_Mod_String;

RAE_UNUSED static RaeAny rae_any_view_int(rae_View_Int v) { return (RaeAny){RAE_TYPE_INT, true, false, {.i = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_mod_int(rae_Mod_Int v) { return (RaeAny){RAE_TYPE_INT, false, true, {.i = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_view_float(rae_View_Float v) { return (RaeAny){RAE_TYPE_FLOAT, true, false, {.f = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_mod_float(rae_Mod_Float v) { return (RaeAny){RAE_TYPE_FLOAT, false, true, {.f = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_view_bool(rae_View_Bool v) { return (RaeAny){RAE_TYPE_BOOL, true, false, {.b = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_mod_bool(rae_Mod_Bool v) { return (RaeAny){RAE_TYPE_BOOL, false, true, {.b = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_view_char(rae_View_Char v) { return (RaeAny){RAE_TYPE_CHAR, true, false, {.i = v.ptr->v}}; }
RAE_UNUSED static RaeAny rae_any_mod_char(rae_Mod_Char v) { return (RaeAny){RAE_TYPE_CHAR, false, true, {.i = v.ptr->v}}; }
RAE_UNUSED static RaeAny rae_any_view_string(rae_View_String v) { return (RaeAny){RAE_TYPE_STRING, true, false, {.s = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_mod_string(rae_Mod_String v) { return (RaeAny){RAE_TYPE_STRING, false, true, {.s = *v.ptr}}; }
RAE_UNUSED static RaeAny rae_any_none(void) { return (RaeAny){RAE_TYPE_NONE, false, false, {.i = 0}}; }
RAE_UNUSED static RaeAny rae_any_ptr(void* v) { return (RaeAny){RAE_TYPE_BUFFER, false, false, {.ptr = v}}; }
RAE_UNUSED static RaeAny rae_any_view(RaeAny a) { a.is_view = true; return a; }
RAE_UNUSED static RaeAny rae_any_mod(RaeAny a) { a.is_mod = true; return a; }
RAE_UNUSED static RaeAny rae_any_identity(RaeAny a) { return a; }
RAE_UNUSED static RaeAny rae_any_identity_ptr(const RaeAny* a) { 
    RaeAny res = *a;
    res.is_view = true;
    return res;
}
RAE_UNUSED static bool rae_any_is_none(RaeAny a) { return a.type == RAE_TYPE_NONE; }
RAE_UNUSED static bool rae_any_eq(RaeAny a, RaeAny b) {
    if (a.type != b.type) return false;
    if (a.type == RAE_TYPE_NONE) return true;
    if (a.type == RAE_TYPE_STRING) {
        if (!a.as.s || !b.as.s) return a.as.s == b.as.s;
        return strcmp(a.as.s, b.as.s) == 0;
    }
    return a.as.i == b.as.i;
}

#define rae_any(X) _Generic((X), \
    int64_t: rae_any_int, \
    double: rae_any_float, \
    char*: rae_any_string, \
    const char*: rae_any_string, \
    RaeAny: rae_any_identity, \
    RaeAny*: rae_any_identity_ptr, \
    bool: rae_any_bool, \
    int8_t: rae_any_bool, \
    rae_Char: rae_any_char, \
    rae_View_Int: rae_any_view_int, \
    rae_Mod_Int: rae_any_mod_int, \
    rae_View_Float: rae_any_view_float, \
    rae_Mod_Float: rae_any_mod_float, \
    rae_View_Bool: rae_any_view_bool, \
    rae_Mod_Bool: rae_any_mod_bool, \
    rae_View_Char: rae_any_view_char, \
    rae_Mod_Char: rae_any_mod_char, \
    rae_View_String: rae_any_view_string, \
    rae_Mod_String: rae_any_mod_string, \
    uint8_t: rae_any_int, \
    default: rae_any_ptr \
)(X)

void rae_ext_rae_log_cstr(const char* text);
void rae_ext_rae_log_stream_cstr(const char* text);
void rae_ext_rae_log_i64(int64_t value);
void rae_ext_rae_log_stream_i64(int64_t value);
void rae_ext_rae_log_bool(int8_t value);
void rae_ext_rae_log_stream_bool(int8_t value);
void rae_ext_rae_log_char(int64_t value);
void rae_ext_rae_log_stream_char(int64_t value);
void rae_ext_rae_log_id(int64_t value);
void rae_ext_rae_log_stream_id(int64_t value);
void rae_ext_rae_log_key(const char* value);
void rae_ext_rae_log_stream_key(const char* value);
void rae_ext_rae_log_float(double value);
void rae_ext_rae_log_stream_float(double value);

void rae_ext_rae_log_list_fields(RaeAny* items, int64_t length, int64_t capacity);
void rae_ext_rae_log_stream_list_fields(RaeAny* items, int64_t length, int64_t capacity);

const char* rae_ext_rae_str_concat(const char* a, const char* b);
int64_t rae_ext_rae_str_len(const char* s);
int64_t rae_ext_rae_str_compare(const char* a, const char* b);
int8_t rae_ext_rae_str_eq(const char* a, const char* b);
int64_t rae_ext_rae_str_hash(const char* s);
const char* rae_ext_rae_str_sub(const char* s, int64_t start, int64_t len);
int8_t rae_ext_rae_str_contains(const char* s, const char* sub);
int8_t rae_ext_rae_str_starts_with(const char* s, const char* prefix);
int8_t rae_ext_rae_str_ends_with(const char* s, const char* suffix);
int64_t rae_ext_rae_str_index_of(const char* s, const char* sub);
const char* rae_ext_rae_str_trim(const char* s);
double rae_ext_rae_str_to_f64(const char* s);
int64_t rae_ext_rae_str_to_i64(const char* s);

const char* rae_ext_rae_io_read_line(void);
rae_Char rae_ext_rae_io_read_char(void);

void rae_ext_rae_sys_exit(int64_t code);
const char* rae_ext_rae_sys_get_env(const char* name);
const char* rae_ext_rae_sys_read_file(const char* path);
int8_t rae_ext_rae_sys_write_file(const char* path, const char* content);
int8_t rae_ext_rae_sys_rename(const char* oldPath, const char* newPath);
int8_t rae_ext_rae_sys_delete(const char* path);
int8_t rae_ext_rae_sys_exists(const char* path);
int8_t rae_ext_rae_sys_lock_file(const char* path);
int8_t rae_ext_rae_sys_unlock_file(const char* path);

const char* rae_ext_rae_str_i64(int64_t v);
const char* rae_ext_rae_str_i64_ptr(const int64_t* v);
const char* rae_ext_rae_str_f64(double v);
const char* rae_ext_rae_str_f64_ptr(const double* v);
const char* rae_ext_rae_str_bool(int8_t v);
const char* rae_ext_rae_str_bool_ptr(const int8_t* v);
const char* rae_ext_rae_str_char(int64_t v);
const char* rae_ext_rae_str_cstr(const char* s);
const char* rae_ext_rae_str_cstr_ptr(const char** s);

int64_t rae_ext_nowMs(void);
int64_t rae_ext_nowNs(void);
void rae_ext_rae_sleep(int64_t ms);

double rae_ext_rae_math_sin(double x);
double rae_ext_rae_math_cos(double x);
double rae_ext_rae_math_tan(double x);
double rae_ext_rae_math_asin(double x);
double rae_ext_rae_math_acos(double x);
double rae_ext_rae_math_atan(double x);
double rae_ext_rae_math_atan2(double y, double x);
double rae_ext_rae_math_sqrt(double x);
double rae_ext_rae_math_pow(double b, double e);
double rae_ext_rae_math_exp(double x);
double rae_ext_rae_math_log(double x);
double rae_ext_rae_math_floor(double x);
double rae_ext_rae_math_ceil(double x);
double rae_ext_rae_math_round(double x);

RaeAny rae_ext_json_get(const char* json, const char* field);
const char* rae_ext_rae_str_i64(int64_t v);
const char* rae_ext_rae_str_f64(double v);
const char* rae_ext_rae_str_bool(int8_t v);
const char* rae_ext_rae_str_char(int64_t v);

RAE_UNUSED static const char* rae_str_any(RaeAny v) {
    const char* res = "";
    switch (v.type) {
        case RAE_TYPE_INT: res = rae_ext_rae_str_i64(v.as.i); break;
        case RAE_TYPE_FLOAT: res = rae_ext_rae_str_f64(v.as.f); break;
        case RAE_TYPE_BOOL: res = rae_ext_rae_str_bool(v.as.b); break;
        case RAE_TYPE_STRING: res = v.as.s ? v.as.s : ""; break;
        case RAE_TYPE_CHAR: res = rae_ext_rae_str_char(v.as.i); break;
        case RAE_TYPE_NONE: res = "none"; break;
        default: res = ""; break;
    }
    if (v.is_view) return rae_ext_rae_str_concat("view ", res);
    if (v.is_mod) return rae_ext_rae_str_concat("mod ", res);
    return res;
}

RAE_UNUSED static const char* rae_str_view_int(rae_View_Int v) { return rae_str_any(rae_any_view_int(v)); }
RAE_UNUSED static const char* rae_str_mod_int(rae_Mod_Int v) { return rae_str_any(rae_any_mod_int(v)); }
RAE_UNUSED static const char* rae_str_view_float(rae_View_Float v) { return rae_str_any(rae_any_view_float(v)); }
RAE_UNUSED static const char* rae_str_mod_float(rae_Mod_Float v) { return rae_str_any(rae_any_mod_float(v)); }
RAE_UNUSED static const char* rae_str_view_bool(rae_View_Bool v) { return rae_str_any(rae_any_view_bool(v)); }
RAE_UNUSED static const char* rae_str_mod_bool(rae_Mod_Bool v) { return rae_str_any(rae_any_mod_bool(v)); }
RAE_UNUSED static const char* rae_str_view_char(rae_View_Char v) { return rae_str_any(rae_any_view_char(v)); }
RAE_UNUSED static const char* rae_str_mod_char(rae_Mod_Char v) { return rae_str_any(rae_any_mod_char(v)); }
RAE_UNUSED static const char* rae_str_view_string(rae_View_String v) { return rae_str_any(rae_any_view_string(v)); }
RAE_UNUSED static const char* rae_str_mod_string(rae_Mod_String v) { return rae_str_any(rae_any_mod_string(v)); }

#define rae_ext_rae_str(X) _Generic((X), \
    long long: rae_ext_rae_str_i64, \
    long long*: rae_ext_rae_str_i64_ptr, \
    const long long*: rae_ext_rae_str_i64_ptr, \
    double: rae_ext_rae_str_f64, \
    double*: rae_ext_rae_str_f64_ptr, \
    const double*: rae_ext_rae_str_f64_ptr, \
    float: rae_ext_rae_str_f64, \
    bool: rae_ext_rae_str_bool, \
    char*: rae_ext_rae_str_cstr, \
    const char*: rae_ext_rae_str_cstr, \
    const char**: rae_ext_rae_str_cstr_ptr, \
    rae_Char: rae_ext_rae_str_char, \
    rae_Char*: rae_ext_rae_str_char_ptr, \
    rae_View_Int: rae_str_view_int, \
    rae_Mod_Int: rae_str_mod_int, \
    rae_View_Float: rae_str_view_float, \
    rae_Mod_Float: rae_str_mod_float, \
    rae_View_Bool: rae_str_view_bool, \
    rae_Mod_Bool: rae_str_mod_bool, \
    rae_View_Char: rae_str_view_char, \
    rae_Mod_Char: rae_str_mod_char, \
    rae_View_String: rae_str_view_string, \
    rae_Mod_String: rae_str_mod_string, \
    default: rae_ext_rae_str_cstr \
)(X)

#endif
