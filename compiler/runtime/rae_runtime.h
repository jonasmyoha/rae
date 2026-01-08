#ifndef RAE_RUNTIME_H
#define RAE_RUNTIME_H

#include <stdint.h>

typedef struct {
  const char* data;
  uint64_t len;
} RaeString;

void rae_log_cstr(const char* text);
void rae_log_stream_cstr(const char* text);
void rae_log_i64(int64_t value);
void rae_log_stream_i64(int64_t value);

const char* rae_str_concat(const char* a, const char* b);
const char* rae_str_i64(int64_t v);
const char* rae_str_cstr(const char* s);

#define rae_str(X) _Generic((X), \
    int64_t: rae_str_i64, \
    const char*: rae_str_cstr \
)(X)

void rae_seed(int64_t seed);
double rae_random(void);
int64_t rae_random_int(int64_t min, int64_t max);

int64_t nextTick(void);
void sleepMs(int64_t ms);

#endif /* RAE_RUNTIME_H */
