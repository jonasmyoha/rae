#ifndef RAE_RUNTIME_H
#define RAE_RUNTIME_H

#include <stdint.h>

#ifdef __GNUC__
#define RAE_UNUSED __attribute__((unused))
#else
#define RAE_UNUSED
#endif

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
  RaeType type;
  union {
    int64_t i;
    double f;
    int8_t b;
    const char* s;
    void* ptr;
  } as;
} RaeAny;

/* Buffer Ops */
void* rae_buf_alloc(int64_t count, int64_t elem_size);
void rae_buf_free(void* buf);
void* rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size);
void rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elem_size);

void rae_log_cstr(const char* text);
void rae_log_stream_cstr(const char* text);
void rae_log_i64(int64_t value);
void rae_log_stream_i64(int64_t value);
void rae_log_bool(int8_t value);
void rae_log_stream_bool(int8_t value);
void rae_log_char(int64_t value);
void rae_log_stream_char(int64_t value);
void rae_log_id(int64_t value);
void rae_log_stream_id(int64_t value);
void rae_log_key(const char* value);
void rae_log_stream_key(const char* value);
void rae_log_float(double value);
void rae_log_stream_float(double value);

void rae_log_any(RaeAny value);
void rae_log_stream_any(RaeAny value);

/* Conversion Helpers */
RAE_UNUSED static RaeAny rae_any_int(int64_t v) { return (RaeAny){RAE_TYPE_INT, {.i = v}}; }
RAE_UNUSED static RaeAny rae_any_float(double v) { return (RaeAny){RAE_TYPE_FLOAT, {.f = v}}; }
RAE_UNUSED static RaeAny rae_any_bool(int8_t v) { return (RaeAny){RAE_TYPE_BOOL, {.b = v}}; }
RAE_UNUSED static RaeAny rae_any_string(const char* v) { return (RaeAny){RAE_TYPE_STRING, {.s = v}}; }
RAE_UNUSED static RaeAny rae_any_none(void) { return (RaeAny){RAE_TYPE_NONE, {.i = 0}}; }
RAE_UNUSED static RaeAny rae_any_identity(RaeAny a) { return a; }

#define rae_any(X) _Generic((X), \
    int64_t: rae_any_int, \
    int: rae_any_int, \
    long: rae_any_int, \
    double: rae_any_float, \
    float: rae_any_float, \
    int8_t: rae_any_bool, \
    char*: rae_any_string, \
    const char*: rae_any_string, \
    RaeAny: rae_any_identity \
)(X)

const char* rae_str_concat(const char* a, const char* b);
const char* rae_str_i64(int64_t v);
const char* rae_str_f64(double v);
const char* rae_str_bool(int8_t v);
const char* rae_str_char(int64_t v);
const char* rae_str_cstr(const char* s);

#define rae_str(X) _Generic((X), \
    int64_t: rae_str_i64, \
    int: rae_str_i64, \
    double: rae_str_f64, \
    float: rae_str_f64, \
    int8_t: rae_str_bool, \
    char*: rae_str_cstr, \
    const char*: rae_str_cstr \
)(X)

void rae_seed(int64_t seed);
double rae_random(void);
int64_t rae_random_int(int64_t min, int64_t max);

int64_t nextTick(void);
int64_t rae_time_ms(void);
void sleepMs(int64_t ms);

#endif /* RAE_RUNTIME_H */
