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

typedef struct {
  int64_t* items;
  int64_t len;
  int64_t cap;
} RaeList;

RaeList* rae_list_create(int64_t cap);
void rae_list_add(RaeList* list, int64_t item);
int64_t rae_list_get(RaeList* list, int64_t index);
int64_t rae_list_length(RaeList* list);

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
void rae_log_list(RaeList* list);
void rae_log_stream_list(RaeList* list);

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
void sleepMs(int64_t ms);

#endif /* RAE_RUNTIME_H */
