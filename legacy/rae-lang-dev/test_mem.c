#include "rae_runtime.h"

typedef struct {
  double dx;
  double dy;
} Velocity;

typedef struct {
  double x;
  double y;
} Point;

typedef struct {
  Point pos;
  RaeAny vel;
} Entity;

RAE_UNUSED static const char* rae_toJson_Velocity_(Velocity* this);
RAE_UNUSED static Velocity rae_fromJson_Velocity_(const char* json);
RAE_UNUSED static void* rae_toBinary_Velocity_(Velocity* this, int64_t* out_size);
RAE_UNUSED static Velocity rae_fromBinary_Velocity_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Entity_(Entity* this);
RAE_UNUSED static Entity rae_fromJson_Entity_(const char* json);
RAE_UNUSED static void* rae_toBinary_Entity_(Entity* this, int64_t* out_size);
RAE_UNUSED static Entity rae_fromBinary_Entity_(void* data, int64_t size);
RAE_UNUSED static const char* rae_toJson_Point_(Point* this);
RAE_UNUSED static Point rae_fromJson_Point_(const char* json);
RAE_UNUSED static void* rae_toBinary_Point_(Point* this, int64_t* out_size);
RAE_UNUSED static Point rae_fromBinary_Point_(void* data, int64_t size);

RAE_UNUSED static const char* rae_toJson_Velocity_(Velocity* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"dx\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->dx));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"dy\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->dy));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static Velocity rae_fromJson_Velocity_(const char* json) {
  Velocity res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "dx");
  if (val.type == RAE_TYPE_FLOAT) res.dx = val.as.f;
  else if (val.type == RAE_TYPE_INT) res.dx = (double)val.as.i;
  val = rae_ext_json_get(json, "dy");
  if (val.type == RAE_TYPE_FLOAT) res.dy = val.as.f;
  else if (val.type == RAE_TYPE_INT) res.dy = (double)val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_Velocity_(Velocity* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static Velocity rae_fromBinary_Velocity_(void* data, int64_t size) {
  (void)data; (void)size;
  Velocity res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_Entity_(Entity* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"pos\": ");
  res = rae_ext_rae_str_concat(res, rae_toJson_Point_(&this->pos));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"vel\": ");
  res = rae_ext_rae_str_concat(res, rae_toJson_Velocity_(&this->vel));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static Entity rae_fromJson_Entity_(const char* json) {
  Entity res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "pos");
  if (val.type == RAE_TYPE_STRING) res.pos = rae_fromJson_Point_(val.as.s);
  val = rae_ext_json_get(json, "vel");
  if (val.type == RAE_TYPE_STRING) res.vel = rae_fromJson_Velocity_(val.as.s);
  return res;
}

RAE_UNUSED static void* rae_toBinary_Entity_(Entity* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static Entity rae_fromBinary_Entity_(void* data, int64_t size) {
  (void)data; (void)size;
  Entity res = {0};
  return res;
}

RAE_UNUSED static const char* rae_toJson_Point_(Point* this) {
  const char* res = "{";
  res = rae_ext_rae_str_concat(res, "\"x\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->x));
  res = rae_ext_rae_str_concat(res, ", ");
  res = rae_ext_rae_str_concat(res, "\"y\": ");
  res = rae_ext_rae_str_concat(res, rae_ext_rae_str_f64(this->y));
  res = rae_ext_rae_str_concat(res, "}");
  return res;
}

RAE_UNUSED static Point rae_fromJson_Point_(const char* json) {
  Point res = {0};
  RaeAny val;
  val = rae_ext_json_get(json, "x");
  if (val.type == RAE_TYPE_FLOAT) res.x = val.as.f;
  else if (val.type == RAE_TYPE_INT) res.x = (double)val.as.i;
  val = rae_ext_json_get(json, "y");
  if (val.type == RAE_TYPE_FLOAT) res.y = val.as.f;
  else if (val.type == RAE_TYPE_INT) res.y = (double)val.as.i;
  return res;
}

RAE_UNUSED static void* rae_toBinary_Point_(Point* this, int64_t* out_size) {
  (void)this;
  if (out_size) *out_size = 0;
  return NULL;
}

RAE_UNUSED static Point rae_fromBinary_Point_(void* data, int64_t size) {
  (void)data; (void)size;
  Point res = {0};
  return res;
}

RAE_UNUSED static void rae_updatePosition_Point_Velocity_(Point* p, const Velocity* v);

RAE_UNUSED static void rae_updatePosition_Point_Velocity_(Point* p, const Velocity* v) {
  p->x = p->x + v->dx;
  p->y = p->y + v->dy;
}

typedef struct {
  Point* p;
  Velocity* v;
} _spawn_args_rae_updatePosition_Point_Velocity_;

static void* _spawn_wrapper_rae_updatePosition_Point_Velocity_(void* data) {
  _spawn_args_rae_updatePosition_Point_Velocity_* args = (_spawn_args_rae_updatePosition_Point_Velocity_*)data;
  rae_updatePosition_Point_Velocity_(args->p, args->v);
  free(args);
  return NULL;
}

int main(void) {
  Velocity v1 = (Velocity){ .dx = 1.0, .dy = 1.0 };
  Velocity v2 = v1;
  v2.dx = 2.0;
  rae_ext_rae_log_cstr("v1.dx (should be 1.0):");
  rae_ext_rae_log_float(v1.dx);
  rae_ext_rae_log_cstr("v2.dx (should be 2.0):");
  rae_ext_rae_log_float(v2.dx);
  Point p = (Point){ .x = 0.0, .y = 0.0 };
  Point* pRef = &(p);
  pRef->x = 10.0;
  rae_ext_rae_log_cstr("p.x after pRef modification (should be 10.0):");
  rae_ext_rae_log_float(p.x);
  Point target = (Point){ .x = 5.0, .y = 5.0 };
  Point* selected = &(target);
  rae_ext_rae_log_cstr("Selected point exists, moving it...");
  rae_updatePosition_Point_Velocity_(selected, &(v1));
  rae_ext_rae_log_cstr("target.x after update (should be 6.0):");
  rae_ext_rae_log_float(target.x);
  return 0;
}

