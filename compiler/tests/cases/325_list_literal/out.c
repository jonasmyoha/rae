#include "rae_runtime.h"

typedef struct rae_List_int64_t rae_List_int64_t;
struct rae_List_int64_t {
  int64_t* data;
  int64_t length;
  int64_t capacity;
};

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
RAE_UNUSED static rae_List_int64_t rae_createList_int64_t_int64_t_(int64_t initialCap);
RAE_UNUSED static void rae_add_int64_t_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value);
RAE_UNUSED static int64_t rae_sizeof_int64_t_(void);
RAE_UNUSED static void rae_grow_int64_t_rae_List_int64_t_(rae_List_int64_t* this);
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

RAE_UNUSED static rae_List_int64_t rae_createList_int64_t_int64_t_(int64_t initialCap);
RAE_UNUSED static void rae_add_int64_t_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value);
RAE_UNUSED static int64_t rae_sizeof_int64_t_(void);
RAE_UNUSED static void rae_grow_int64_t_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static rae_List_int64_t rae_createList_int64_t_int64_t_(int64_t initialCap) {
  return (rae_List_int64_t){ .data = rae_ext_rae_buf_alloc(initialCap, sizeof(int64_t)), .length = ((int64_t)0LL), .capacity = initialCap };
}

RAE_UNUSED static void rae_add_int64_t_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value) {
  if ((bool)(this->length == this->capacity)) {
rae_grow_int64_t_rae_List_int64_t_(this);
  }
rae_ext_rae_buf_set_rae_V_rae_Buffer_rae_V_int64_t_rae_V_(this->data, this->length, value);
  this->length = this->length + ((int64_t)1LL);
}

RAE_UNUSED static int64_t rae_sizeof_int64_t_(void) {
}

RAE_UNUSED static void rae_grow_int64_t_rae_List_int64_t_(rae_List_int64_t* this) {
  int64_t newCap = this->capacity * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  int64_t* newData = rae_ext_rae_buf_alloc(newCap, sizeof(int64_t));
rae_ext_rae_buf_copy_rae_V_rae_Buffer_rae_V_int64_t_rae_Buffer_rae_V_int64_t_int64_t_int64_t_(this->data, ((int64_t)0LL), newData, ((int64_t)0LL), this->length, sizeof(int64_t));
rae_ext_rae_buf_free(this->data);
  this->data = newData;
  this->capacity = newCap;
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  rae_List_int64_t x = rae_createList_int64_t_int64_t_(((int64_t)3LL));
  rae_add_int64_t_rae_List_int64_t_int64_t_(&x, ((int64_t)10LL));
  rae_add_int64_t_rae_List_int64_t_int64_t_(&x, ((int64_t)20LL));
  rae_add_int64_t_rae_List_int64_t_int64_t_(&x, ((int64_t)30LL));
rae_ext_rae_log_list_fields((RaeAny*)(x).data, (x).length, (x).capacity);
  return 0;
}

