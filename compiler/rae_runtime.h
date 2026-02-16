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

typedef uint32_t rae_Char32;
typedef uint32_t rae_Char;

#ifdef RAE_HAS_RAYLIB
typedef bool rae_Bool;
#else
typedef int8_t rae_Bool;
#endif

typedef struct {
  uint8_t* data;
  int64_t len;
} rae_String;

typedef enum {
  RAE_TYPE_NONE,
  RAE_TYPE_INT64,
  RAE_TYPE_INT32,
  RAE_TYPE_UINT64,
  RAE_TYPE_UINT32,
  RAE_TYPE_FLOAT64,
  RAE_TYPE_FLOAT32,
  RAE_TYPE_BOOL,
  RAE_TYPE_STRING,
  RAE_TYPE_CHAR,
  RAE_TYPE_ID,
  RAE_TYPE_KEY,
  RAE_TYPE_LIST,
  RAE_TYPE_BUFFER,
  RAE_TYPE_ANY
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
    rae_String s;
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
RAE_UNUSED static RaeAny rae_any_int(int64_t v) { return (RaeAny){RAE_TYPE_INT64, false, false, {.i = v}}; }
RAE_UNUSED static RaeAny rae_any_int32(int32_t v) { return (RaeAny){RAE_TYPE_INT32, false, false, {.i = v}}; }
RAE_UNUSED static RaeAny rae_any_uint64(uint64_t v) { return (RaeAny){RAE_TYPE_UINT64, false, false, {.i = (int64_t)v}}; }
RAE_UNUSED static RaeAny rae_any_int_ptr(const int64_t* v) { return (RaeAny){RAE_TYPE_INT64, true, false, {.ptr = (void*)v}}; }
RAE_UNUSED static RaeAny rae_any_float(double v) { return (RaeAny){RAE_TYPE_FLOAT64, false, false, {.f = v}}; }
RAE_UNUSED static RaeAny rae_any_float32(float v) { return (RaeAny){RAE_TYPE_FLOAT32, false, false, {.f = v}}; }
RAE_UNUSED static RaeAny rae_any_float_ptr(const void* v) { return (RaeAny){RAE_TYPE_FLOAT64, true, false, {.ptr = (void*)v}}; }
RAE_UNUSED static RaeAny rae_any_bool(int8_t v) { return (RaeAny){RAE_TYPE_BOOL, false, false, {.b = v}}; }
RAE_UNUSED static RaeAny rae_any_char(uint32_t v) { return (RaeAny){RAE_TYPE_CHAR, false, false, {.i = (int64_t)v}}; }
RAE_UNUSED static RaeAny rae_any_char_ptr(const uint32_t* v) { return (RaeAny){RAE_TYPE_CHAR, true, false, {.ptr = (void*)v}}; }
RAE_UNUSED static RaeAny rae_any_string(rae_String v) { return (RaeAny){RAE_TYPE_STRING, false, false, {.s = v}}; }
RAE_UNUSED static RaeAny rae_any_string_ptr(const rae_String* v) { return (RaeAny){RAE_TYPE_STRING, true, false, {.ptr = (void*)v}}; }

RAE_UNUSED static RaeAny rae_any_none(void) { return (RaeAny){RAE_TYPE_NONE, false, false, {.i = 0}}; }
RAE_UNUSED static RaeAny rae_any_ptr(void* v) { return (RaeAny){RAE_TYPE_BUFFER, false, false, {.ptr = v}}; }

RAE_UNUSED static RaeAny rae_any_view(void* v, RaeType type) {
    if (type == RAE_TYPE_ANY) {
         RaeAny* res = (RaeAny*)v;
         if (res->is_view || res->is_mod) return *res;
         RaeAny out = *res;
         out.is_view = true;
         return out;
    }
    return (RaeAny){type, true, false, {.ptr = v}};
}

RAE_UNUSED static RaeAny rae_any_mod(void* v, RaeType type) {
    if (type == RAE_TYPE_ANY) {
         RaeAny* res = (RaeAny*)v;
         if (res->is_view || res->is_mod) return *res;
         RaeAny out = *res;
         out.is_mod = true;
         return out;
    }
    return (RaeAny){type, false, true, {.ptr = v}};
}

RAE_UNUSED static RaeAny rae_any_identity(RaeAny a) { return a; }
RAE_UNUSED static RaeAny rae_any_identity_ptr(const RaeAny* a) { 
    RaeAny res = *a;
    res.is_view = true;
    return res;
}

// Helpers for reference structs
typedef struct { int64_t* ptr; } rae_View_Int64;
typedef struct { int64_t* ptr; } rae_Mod_Int64;
typedef struct { int32_t* ptr; } rae_View_Int32;
typedef struct { int32_t* ptr; } rae_Mod_Int32;
typedef struct { uint64_t* ptr; } rae_View_UInt64;
typedef struct { uint64_t* ptr; } rae_Mod_UInt64;
typedef struct { uint32_t* ptr; } rae_View_UInt32;
typedef struct { uint32_t* ptr; } rae_Mod_UInt32;
typedef struct { double* ptr; } rae_View_Float64;
typedef struct { double* ptr; } rae_Mod_Float64;
typedef struct { float* ptr; } rae_View_Float32;
typedef struct { float* ptr; } rae_Mod_Float32;
typedef struct { rae_Bool* ptr; } rae_View_Bool;
typedef struct { rae_Bool* ptr; } rae_Mod_Bool;
typedef struct { uint32_t* ptr; } rae_View_Char32;
typedef struct { uint32_t* ptr; } rae_Mod_Char32;
typedef struct { uint32_t* ptr; } rae_View_Char;
typedef struct { uint32_t* ptr; } rae_Mod_Char;
typedef struct { rae_String* ptr; } rae_View_String;
typedef struct { rae_String* ptr; } rae_Mod_String;

RAE_UNUSED static RaeAny rae_any_view_int64(rae_View_Int64 v) { return rae_any_view(v.ptr, RAE_TYPE_INT64); }
RAE_UNUSED static RaeAny rae_any_mod_int64(rae_Mod_Int64 v) { return rae_any_mod(v.ptr, RAE_TYPE_INT64); }
RAE_UNUSED static RaeAny rae_any_view_int32(rae_View_Int32 v) { return rae_any_view(v.ptr, RAE_TYPE_INT32); }
RAE_UNUSED static RaeAny rae_any_mod_int32(rae_Mod_Int32 v) { return rae_any_mod(v.ptr, RAE_TYPE_INT32); }
RAE_UNUSED static RaeAny rae_any_view_uint64(rae_View_UInt64 v) { return rae_any_view(v.ptr, RAE_TYPE_UINT64); }
RAE_UNUSED static RaeAny rae_any_mod_uint64(rae_Mod_UInt64 v) { return rae_any_mod(v.ptr, RAE_TYPE_UINT64); }
RAE_UNUSED static RaeAny rae_any_view_uint32(rae_View_UInt32 v) { return rae_any_view(v.ptr, RAE_TYPE_UINT32); }
RAE_UNUSED static RaeAny rae_any_mod_uint32(rae_Mod_UInt32 v) { return rae_any_mod(v.ptr, RAE_TYPE_UINT32); }
RAE_UNUSED static RaeAny rae_any_view_float64(rae_View_Float64 v) { return rae_any_view(v.ptr, RAE_TYPE_FLOAT64); }
RAE_UNUSED static RaeAny rae_any_mod_float64(rae_Mod_Float64 v) { return rae_any_mod(v.ptr, RAE_TYPE_FLOAT64); }
RAE_UNUSED static RaeAny rae_any_view_float32(rae_View_Float32 v) { return rae_any_view(v.ptr, RAE_TYPE_FLOAT32); }
RAE_UNUSED static RaeAny rae_any_mod_float32(rae_Mod_Float32 v) { return rae_any_mod(v.ptr, RAE_TYPE_FLOAT32); }
RAE_UNUSED static RaeAny rae_any_view_bool(rae_View_Bool v) { return rae_any_view(v.ptr, RAE_TYPE_BOOL); }
RAE_UNUSED static RaeAny rae_any_mod_bool(rae_Mod_Bool v) { return rae_any_mod(v.ptr, RAE_TYPE_BOOL); }
RAE_UNUSED static RaeAny rae_any_view_char32(rae_View_Char32 v) { return rae_any_view(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_mod_char32(rae_Mod_Char32 v) { return rae_any_mod(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_view_char(rae_View_Char v) { return rae_any_view(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_mod_char(rae_Mod_Char v) { return rae_any_mod(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_view_string(rae_View_String v) { return rae_any_view(v.ptr, RAE_TYPE_STRING); }
RAE_UNUSED static RaeAny rae_any_mod_string(rae_Mod_String v) { return rae_any_mod(v.ptr, RAE_TYPE_STRING); }

RAE_UNUSED static bool rae_any_is_none(RaeAny a) { return a.type == RAE_TYPE_NONE; }
RAE_UNUSED static bool rae_any_eq(RaeAny a, RaeAny b) {
    if (a.type != b.type) return false;
    if (a.type == RAE_TYPE_NONE) return true;
    if (a.type == RAE_TYPE_STRING) {
        if (a.as.s.len != b.as.s.len) return false;
        if (a.as.s.len == 0) return true;
        if (!a.as.s.data || !b.as.s.data) return a.as.s.data == b.as.s.data;
        return memcmp(a.as.s.data, b.as.s.data, a.as.s.len) == 0;
    }
    return a.as.i == b.as.i;
}

RAE_UNUSED static RaeAny rae_any_bool_ptr(rae_Bool* v) { return rae_any_view(v, RAE_TYPE_BOOL); }

#define rae_any(X) _Generic(((X)), \
    int64_t: rae_any_int, \
    int32_t: rae_any_int32, \
    uint64_t: rae_any_uint64, \
    double: rae_any_float, \
    float: rae_any_float32, \
    rae_String: rae_any_string, \
    RaeAny: rae_any_identity, \
    RaeAny*: rae_any_identity_ptr, \
    bool: rae_any_bool, \
    int8_t: rae_any_bool, \
    rae_Bool*: rae_any_bool_ptr, \
    uint32_t: rae_any_char, \
    uint8_t: rae_any_int, \
    rae_View_Int64: rae_any_view_int64, \
    rae_Mod_Int64: rae_any_mod_int64, \
    rae_View_Int32: rae_any_view_int32, \
    rae_Mod_Int32: rae_any_mod_int32, \
    rae_View_UInt64: rae_any_view_uint64, \
    rae_Mod_UInt64: rae_any_mod_uint64, \
    rae_View_UInt32: rae_any_view_uint32, \
    rae_Mod_UInt32: rae_any_mod_uint32, \
    rae_View_Float64: rae_any_view_float64, \
    rae_Mod_Float64: rae_any_mod_float64, \
    rae_View_Float32: rae_any_view_float32, \
    rae_Mod_Float32: rae_any_mod_float32, \
    rae_View_Bool: rae_any_view_bool, \
    rae_Mod_Bool: rae_any_mod_bool, \
    rae_View_Char32: rae_any_view_char32, \
    rae_Mod_Char32: rae_any_mod_char32, \
    rae_View_Char: rae_any_view_char, \
    rae_Mod_Char: rae_any_mod_char, \
    rae_View_String: rae_any_view_string, \
    rae_Mod_String: rae_any_mod_string, \
    default: rae_any_ptr \
)(X)

void rae_ext_rae_log_cstr(const char* text);
void rae_ext_rae_log_stream_cstr(const char* text);
void rae_ext_rae_log_i64(int64_t value);
void rae_ext_rae_log_stream_i64(int64_t value);
void rae_ext_rae_log_bool(int8_t value);
void rae_ext_rae_log_stream_bool(int8_t value);
void rae_ext_rae_log_string(rae_String value);
void rae_ext_rae_log_stream_string(rae_String value);
void rae_ext_rae_log_char(uint32_t value);
void rae_ext_rae_log_stream_char(uint32_t value);
void rae_ext_rae_log_id(int64_t value);
void rae_ext_rae_log_stream_id(int64_t value);
void rae_ext_rae_log_key(rae_String value);
void rae_ext_rae_log_stream_key(rae_String value);
void rae_ext_rae_log_float(double value);
void rae_ext_rae_log_stream_float(double value);

void rae_ext_rae_log_list_fields(RaeAny* items, int64_t length, int64_t capacity);
void rae_ext_rae_log_stream_list_fields(RaeAny* items, int64_t length, int64_t capacity);

rae_String rae_ext_rae_str_from_cstr(void* s);
rae_String rae_ext_rae_str_from_buf(const uint8_t* data, int64_t len);
void* rae_ext_rae_str_to_cstr(rae_String s);
void rae_ext_rae_str_free(rae_String s);

rae_String rae_ext_rae_str_concat(rae_String a, rae_String b);
rae_String rae_ext_rae_str_concat_cstr(rae_String a, rae_String b); // Legacy/helper name
int64_t rae_ext_rae_str_len(rae_String s);
int64_t rae_ext_rae_str_compare(rae_String a, rae_String b);
int8_t rae_ext_rae_str_eq(rae_String a, rae_String b);
int64_t rae_ext_rae_str_hash(rae_String s);
rae_String rae_ext_rae_str_sub(rae_String s, int64_t start, int64_t len);
int8_t rae_ext_rae_str_contains(rae_String s, rae_String sub);
int8_t rae_ext_rae_str_starts_with(rae_String s, rae_String prefix);
int8_t rae_ext_rae_str_ends_with(rae_String s, rae_String suffix);
int64_t rae_ext_rae_str_index_of(rae_String s, rae_String sub);
rae_String rae_ext_rae_str_trim(rae_String s);
uint32_t rae_ext_rae_str_at(rae_String s, int64_t index);
double rae_ext_rae_str_to_f64(rae_String s);
int64_t rae_ext_rae_str_to_i64(rae_String s);

rae_String rae_ext_rae_io_read_line(void);
rae_Char rae_ext_rae_io_read_char(void);

void rae_ext_rae_sys_exit(int64_t code);
rae_String rae_ext_rae_sys_get_env(rae_String name);
rae_String rae_ext_rae_sys_read_file(rae_String path);
int8_t rae_ext_rae_sys_write_file(rae_String path, rae_String content);
int8_t rae_ext_rae_sys_rename(rae_String oldPath, rae_String newPath);
int8_t rae_ext_rae_sys_delete(rae_String path);
int8_t rae_ext_rae_sys_exists(rae_String path);
int8_t rae_ext_rae_sys_lock_file(rae_String path);
int8_t rae_ext_rae_sys_unlock_file(rae_String path);

rae_String rae_ext_rae_str_i64(int64_t v);
rae_String rae_ext_rae_str_i64_ptr(const int64_t* v);
rae_String rae_ext_rae_str_f64(double v);
rae_String rae_ext_rae_str_f64_ptr(const double* v);
rae_String rae_ext_rae_str_bool(int8_t v);
rae_String rae_ext_rae_str_bool_ptr(const int8_t* v);
rae_String rae_ext_rae_str_char(uint32_t v);
rae_String rae_ext_rae_str_char_ptr(const uint32_t* v);
rae_String rae_ext_rae_str_string(rae_String s);
rae_String rae_ext_rae_str_string_ptr(const rae_String* s);
rae_String rae_ext_rae_str_cstr(const char* s); // Legacy/helper
rae_String rae_ext_rae_str_cstr_ptr(const char** s); // Legacy/helper

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

RAE_UNUSED static const char* rae_str_any(RaeAny v) {
    if (v.type == RAE_TYPE_ANY) {
        RaeAny inner = *(RaeAny*)v.as.ptr;
        if (v.is_view) inner.is_view = true;
        if (v.is_mod) inner.is_mod = true;
        return rae_str_any(inner);
    }
    const char* res = "";
    switch (v.type) {
        case RAE_TYPE_INT64: res = rae_ext_rae_str_to_cstr(rae_ext_rae_str_i64(v.as.i)); break;
        case RAE_TYPE_INT32: res = rae_ext_rae_str_to_cstr(rae_ext_rae_str_i64(v.as.i)); break;
        case RAE_TYPE_UINT64: res = rae_ext_rae_str_to_cstr(rae_ext_rae_str_i64(v.as.i)); break;
        case RAE_TYPE_FLOAT64: res = rae_ext_rae_str_to_cstr(rae_ext_rae_str_f64(v.as.f)); break;
        case RAE_TYPE_FLOAT32: res = rae_ext_rae_str_to_cstr(rae_ext_rae_str_f64(v.as.f)); break;
        case RAE_TYPE_BOOL: res = rae_ext_rae_str_to_cstr(rae_ext_rae_str_bool(v.as.b)); break;
        case RAE_TYPE_STRING: res = rae_ext_rae_str_to_cstr(v.as.s); break;
        case RAE_TYPE_CHAR: res = rae_ext_rae_str_to_cstr(rae_ext_rae_str_char((uint32_t)v.as.i)); break;
        case RAE_TYPE_NONE: res = "none"; break;
        default: res = ""; break;
    }
    if (v.is_view) {
        if (strncmp(res, "view ", 5) == 0) return res;
        return rae_ext_rae_str_to_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_from_cstr((void*)"view "), rae_ext_rae_str_from_cstr((void*)res)));
    }
    if (v.is_mod) {
        if (strncmp(res, "mod ", 4) == 0) return res;
        return rae_ext_rae_str_to_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_from_cstr((void*)"mod "), rae_ext_rae_str_from_cstr((void*)res)));
    }
    return res;
}

#define rae_ext_rae_str(X) _Generic((X), \
    int64_t: rae_ext_rae_str_i64, \
    int64_t*: rae_ext_rae_str_i64_ptr, \
    const int64_t*: rae_ext_rae_str_i64_ptr, \
    double: rae_ext_rae_str_f64, \
    double*: rae_ext_rae_str_f64_ptr, \
    const double*: rae_ext_rae_str_f64_ptr, \
    float: rae_ext_rae_str_f64, \
    bool: rae_ext_rae_str_bool, \
    int8_t: rae_ext_rae_str_bool, \
    rae_String: rae_ext_rae_str_string, \
    rae_String*: rae_ext_rae_str_string_ptr, \
    uint32_t: rae_ext_rae_str_char, \
    uint32_t*: rae_ext_rae_str_char_ptr, \
    default: rae_ext_rae_str_string \
)(X)

#endif
