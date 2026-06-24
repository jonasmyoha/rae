#include "rae_runtime.h"

typedef struct rae_Point rae_Point;
struct rae_Point {
  int64_t x;
};

RAE_UNUSED static rae_String rae_toJson_rae_Point_(rae_Point* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"x\": %lld", (long long)this->x);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Point rae_fromJson_rae_Point_(rae_String json) {
  rae_Point __r = {0};
  __r.x = rae_json_extract_int(json, "x");
  return __r;
}

int64_t rae_ext_nextTick(void);
int64_t rae_ext_nowMs(void);
int64_t rae_ext_nowNs(void);
void rae_ext_rae_sleep(int64_t ms);
RAE_UNUSED static double rae_toFloat_int64_t_(int64_t this);
RAE_UNUSED static int64_t rae_toInt_double_(double this);
RAE_UNUSED static void rae_log_RaeAny_(RaeAny value);
RAE_UNUSED static void rae_logS_RaeAny_(RaeAny value);
RAE_UNUSED static rae_String rae_readLine_(void);
RAE_UNUSED static uint32_t rae_readChar_(void);
RAE_UNUSED static rae_Mod_Int64 rae_getX_rae_Point_(rae_Point* p);
RAE_UNUSED static double rae_toFloat_int64_t_(int64_t this) {
  return rae_ext_rae_int_to_float(this);
}

RAE_UNUSED static int64_t rae_toInt_double_(double this) {
  return rae_ext_rae_float_to_int(this);
}

RAE_UNUSED static void rae_log_RaeAny_(RaeAny value) {
rae_ext_rae_log_any(rae_any((value)));
}

RAE_UNUSED static void rae_logS_RaeAny_(RaeAny value) {
rae_ext_rae_log_stream_any(rae_any((value)));
}

RAE_UNUSED static rae_String rae_readLine_(void) {
  return rae_ext_rae_io_read_line();
}

RAE_UNUSED static uint32_t rae_readChar_(void) {
  return rae_ext_rae_io_read_char();
}

RAE_UNUSED static rae_Mod_Int64 rae_getX_rae_Point_(rae_Point* p) {
  return (rae_Mod_Int64){ .ptr = &p->x };
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  rae_Point p = (rae_Point){ .x = ((int64_t)1LL) };
  rae_Mod_Int64 rx = rae_getX_rae_Point_(&p);
  *rx.ptr = ((int64_t)10LL);
rae_log_RaeAny_(rae_any((p.x)));
  return 0;
}

