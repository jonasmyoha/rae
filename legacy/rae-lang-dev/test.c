#include "rae_runtime.h"

typedef enum {
  Status_Active,
  Status_Inactive
} Status;

typedef struct {
  int64_t x;
  int64_t y;
} Point;

typedef struct {
  int64_t id;
  Point pos;
  int8_t active;
  const char* name;
  Status status;
} Entity;

int main(void) {
  int64_t i = {0};
  double f = {0};
  int8_t b = {0};
  const char* s = {0};
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Int: ", rae_ext_rae_str(i)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Float: ", rae_ext_rae_str(f)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Bool: ", rae_ext_rae_str(b)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("String: '", rae_ext_rae_str(s)), "'"));
  Status st = {0};
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Enum: ", rae_ext_rae_str(st == Status_Active)), ""));
  Entity e = {0};
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity.id: ", rae_ext_rae_str(e.id)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity.pos.x: ", rae_ext_rae_str(e.pos.x)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity.pos.y: ", rae_ext_rae_str(e.pos.y)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity.active: ", rae_ext_rae_str(e.active)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity.name: '", rae_ext_rae_str(e.name)), "'"));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity.status is Active: ", rae_ext_rae_str(e.status == Status_Active)), ""));
  Entity e2 = (Entity){ .id = 42 };
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity2.id: ", rae_ext_rae_str(e2.id)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity2.pos.x: ", rae_ext_rae_str(e2.pos.x)), ""));
  rae_ext_rae_log_cstr(rae_ext_rae_str_concat(rae_ext_rae_str_concat("Entity2.active: ", rae_ext_rae_str(e2.active)), ""));
  return 0;
}

