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
typedef struct rae_StringMapEntry_RaeAny rae_StringMapEntry_RaeAny;
typedef struct rae_IntMapEntry_RaeAny rae_IntMapEntry_RaeAny;
typedef struct rae_MapEntry_RaeAny rae_MapEntry_RaeAny;

typedef struct rae_State rae_State;
struct rae_State {
  int64_t score;
  rae_Bool active;
  const char* name;
};

RAE_UNUSED static const char* rae_toJson_rae_State_(rae_State* this);
RAE_UNUSED static rae_State rae_fromJson_rae_State_(const char* json);
RAE_UNUSED static void rae_log_rae_State_(rae_State val);
RAE_UNUSED static void rae_log_stream_rae_State_(rae_State val);
RAE_UNUSED static const char* rae_str_rae_State_(rae_State val);

RAE_UNUSED static const char* rae_toJson_rae_State_(rae_State* this) { (void)this; return "{}"; }
RAE_UNUSED static rae_State rae_fromJson_rae_State_(const char* json) { (void)json; rae_State res = {0}; return res; }
RAE_UNUSED static void rae_log_rae_State_(rae_State val) { rae_log_stream_rae_State_(val); printf("\n"); }
RAE_UNUSED static void rae_log_stream_rae_State_(rae_State val) { (void)val; printf("State(...)"); }
RAE_UNUSED static const char* rae_str_rae_State_(rae_State val) { (void)val; return "State(...)"; }

RAE_UNUSED  const char* rae_ext_rae_io_read_line(void);
RAE_UNUSED  rae_Char rae_ext_rae_io_read_char(void);
RAE_UNUSED static const char* rae_readLine_(void);
RAE_UNUSED static rae_Char rae_readChar_(void);
int main(void);

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
  rae_State s = {0};
  if ((bool)(s.score == ((int64_t)0LL))) {
  {
  rae_ext_rae_log_any(rae_any("score ok"));
  }
  }
  if ((bool)(s.active == (bool)false)) {
  {
  rae_ext_rae_log_any(rae_any("active ok"));
  }
  }
  if ((bool)rae_ext_rae_str_eq(s.name, "")) {
  {
  rae_ext_rae_log_any(rae_any("name ok"));
  }
  }
  int64_t x = {0};
  if ((bool)(x == ((int64_t)0LL))) {
  {
  rae_ext_rae_log_any(rae_any("primitive ok"));
  }
  }
  if ((bool)(rae_str_any(rae_any(s.active)) == "false")) {
  {
  rae_ext_rae_log_any(rae_any("bool toString ok"));
  }
  }
  }
  return 0;
}

