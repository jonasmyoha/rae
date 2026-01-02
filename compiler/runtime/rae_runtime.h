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

#endif /* RAE_RUNTIME_H */
