#include "rae_runtime.h"

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
RAE_UNUSED static void rae_takeViewString_rae_View_String_(rae_View_String s);
RAE_UNUSED static void rae_takeViewInt_rae_View_Int64_(rae_View_Int64 i);
RAE_UNUSED static void rae_takeViewFloat_rae_View_Float64_(rae_View_Float64 f);
RAE_UNUSED static void rae_takeViewBool_rae_View_Bool_(rae_View_Bool b);
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

RAE_UNUSED static void rae_takeViewString_rae_View_String_(rae_View_String s) {
rae_log_RaeAny_(rae_any((rae_ext_rae_str_concat(rae_ext_rae_str_concat((rae_String){(uint8_t*)"view ", 5}, rae_ext_rae_str(((*s.ptr)))), (rae_String){(uint8_t*)"", 0}))));
}

RAE_UNUSED static void rae_takeViewInt_rae_View_Int64_(rae_View_Int64 i) {
rae_log_RaeAny_(rae_any((rae_ext_rae_str_concat(rae_ext_rae_str_concat((rae_String){(uint8_t*)"view ", 5}, rae_ext_rae_str(((*i.ptr)))), (rae_String){(uint8_t*)"", 0}))));
}

RAE_UNUSED static void rae_takeViewFloat_rae_View_Float64_(rae_View_Float64 f) {
rae_log_RaeAny_(rae_any((rae_ext_rae_str_concat(rae_ext_rae_str_concat((rae_String){(uint8_t*)"view ", 5}, rae_ext_rae_str(((*f.ptr)))), (rae_String){(uint8_t*)"", 0}))));
}

RAE_UNUSED static void rae_takeViewBool_rae_View_Bool_(rae_View_Bool b) {
rae_log_RaeAny_(rae_any((rae_ext_rae_str_concat(rae_ext_rae_str_concat((rae_String){(uint8_t*)"view ", 5}, rae_ext_rae_str(((*b.ptr)))), (rae_String){(uint8_t*)"", 0}))));
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
rae_takeViewString_rae_View_String_((rae_String){(uint8_t*)"Literal String", 14});
rae_takeViewInt_rae_View_Int64_(((int64_t)42LL));
rae_takeViewFloat_rae_View_Float64_(3.14);
rae_takeViewBool_rae_View_Bool_((bool)true);
  return 0;
}

