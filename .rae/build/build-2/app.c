#include "rae_runtime.h"

typedef struct rae_List_rae_String rae_List_rae_String;
struct rae_List_rae_String {
  rae_String* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_JsonValue rae_JsonValue;
struct rae_JsonValue {
  int64_t kind;
  rae_Bool asBool;
  double asNumber;
  rae_String asString;
  int64_t rangeStart;
  int64_t rangeLen;
};

typedef struct rae_List_rae_JsonValue rae_List_rae_JsonValue;
struct rae_List_rae_JsonValue {
  rae_JsonValue* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_List_int64_t rae_List_int64_t;
struct rae_List_int64_t {
  int64_t* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_JsonField rae_JsonField;
struct rae_JsonField {
  rae_String key;
  int64_t valueIdx;
};

typedef struct rae_List_rae_JsonField rae_List_rae_JsonField;
struct rae_List_rae_JsonField {
  rae_JsonField* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_SdfGlyph rae_SdfGlyph;
struct rae_SdfGlyph {
  int64_t unicode;
  double advance;
  rae_Bool hasBounds;
  double planeLeft;
  double planeBottom;
  double planeRight;
  double planeTop;
  double atlasLeft;
  double atlasBottom;
  double atlasRight;
  double atlasTop;
};

typedef struct rae_List_rae_SdfGlyph rae_List_rae_SdfGlyph;
struct rae_List_rae_SdfGlyph {
  rae_SdfGlyph* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_Vec3 rae_Vec3;
struct rae_Vec3 {
  double x;
  double y;
  double z;
};

typedef struct rae_Material rae_Material;
struct rae_Material {
  int64_t kind;
  rae_Vec3 albedo;
  double fuzz;
  double ior;
};

typedef struct rae_Triangle rae_Triangle;
struct rae_Triangle {
  rae_Vec3 v0;
  rae_Vec3 v1;
  rae_Vec3 v2;
  rae_Vec3 n0;
  rae_Vec3 n1;
  rae_Vec3 n2;
  rae_Material material;
};

typedef struct rae_List_rae_Triangle rae_List_rae_Triangle;
struct rae_List_rae_Triangle {
  rae_Triangle* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_List_double rae_List_double;
struct rae_List_double {
  double* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_Box rae_Box;
struct rae_Box {
  rae_Vec3 lo;
  rae_Vec3 hi;
  rae_Material material;
};

typedef struct rae_List_rae_Box rae_List_rae_Box;
struct rae_List_rae_Box {
  rae_Box* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_Sphere rae_Sphere;
struct rae_Sphere {
  rae_Vec3 center;
  double radius;
  rae_Material material;
};

typedef struct rae_List_rae_Sphere rae_List_rae_Sphere;
struct rae_List_rae_Sphere {
  rae_Sphere* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_PrimRec rae_PrimRec;
struct rae_PrimRec {
  double mnx;
  double mny;
  double mnz;
  double mxx;
  double mxy;
  double mxz;
  double cx;
  double cy;
  double cz;
  int64_t ref;
};

typedef struct rae_List_rae_PrimRec rae_List_rae_PrimRec;
struct rae_List_rae_PrimRec {
  rae_PrimRec* data;
  int64_t length;
  int64_t cap;
};

#define JsonKind_null ((int64_t)0LL)
#define JsonKind_bool ((int64_t)1LL)
#define JsonKind_number ((int64_t)2LL)
#define JsonKind_string ((int64_t)3LL)
#define JsonKind_array ((int64_t)4LL)
#define JsonKind_object ((int64_t)5LL)

typedef struct rae_List2Int rae_List2Int;
struct rae_List2Int {
  int64_t* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_List2 rae_List2;
struct rae_List2 {
  void* data;
  int64_t length;
  int64_t cap;
};

typedef struct rae_JsonDoc rae_JsonDoc;
struct rae_JsonDoc {
  rae_List_rae_JsonValue values;
  rae_List_int64_t children;
  rae_List_rae_JsonField fields;
  int64_t rootIdx;
  rae_Bool ok;
  int64_t errorPos;
};

typedef struct rae_JsonParser rae_JsonParser;
struct rae_JsonParser {
  rae_String source;
  int64_t pos;
  rae_Bool ok;
};

typedef struct rae_SdfFont rae_SdfFont;
struct rae_SdfFont {
  int64_t atlas;
  double atlasWidth;
  double atlasHeight;
  double pxRange;
  double emSize;
  double lineHeight;
  double ascender;
  rae_List_rae_SdfGlyph glyphs;
};

typedef struct rae_Ray rae_Ray;
struct rae_Ray {
  rae_Vec3 origin;
  rae_Vec3 dir;
};

typedef struct rae_Camera rae_Camera;
struct rae_Camera {
  rae_Vec3 origin;
  rae_Vec3 lowerLeft;
  rae_Vec3 horizontal;
  rae_Vec3 vertical;
  rae_Vec3 right;
  rae_Vec3 up;
  double lensRadius;
};

typedef struct rae_GpuRT rae_GpuRT;
struct rae_GpuRT {
  int64_t sceneBuf;
  int64_t boxBuf;
  int64_t triBuf;
  int64_t bvhBuf;
  int64_t refBuf;
  int64_t accumBuf;
  int64_t outBuf;
  int64_t paramsBuf;
  int64_t kernel;
};

typedef struct rae_SceneGpu rae_SceneGpu;
struct rae_SceneGpu {
  rae_List_double nodes;
  rae_List_double refs;
};

RAE_UNUSED static rae_String rae_toJson_rae_List2Int_(rae_List2Int* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"data\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"length\": %lld", (long long)this->length);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"cap\": %lld", (long long)this->cap);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_List2Int rae_fromJson_rae_List2Int_(rae_String json) {
  rae_List2Int __r = {0};
  __r.length = rae_json_extract_int(json, "length");
  __r.cap = rae_json_extract_int(json, "cap");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_List2_(rae_List2* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"data\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"length\": %lld", (long long)this->length);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"cap\": %lld", (long long)this->cap);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_List2 rae_fromJson_rae_List2_(rae_String json) {
  rae_List2 __r = {0};
  __r.length = rae_json_extract_int(json, "length");
  __r.cap = rae_json_extract_int(json, "cap");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_JsonField_(rae_JsonField* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"key\": \"%.*s\"", (int)this->key.len, (char*)this->key.data);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"valueIdx\": %lld", (long long)this->valueIdx);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_JsonField rae_fromJson_rae_JsonField_(rae_String json) {
  rae_JsonField __r = {0};
  __r.key = rae_json_extract_string(json, "key");
  __r.valueIdx = rae_json_extract_int(json, "valueIdx");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_JsonValue_(rae_JsonValue* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"kind\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"asBool\": %s", this->asBool ? "true" : "false");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"asNumber\": %g", this->asNumber);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"asString\": \"%.*s\"", (int)this->asString.len, (char*)this->asString.data);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"rangeStart\": %lld", (long long)this->rangeStart);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"rangeLen\": %lld", (long long)this->rangeLen);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_JsonValue rae_fromJson_rae_JsonValue_(rae_String json) {
  rae_JsonValue __r = {0};
  __r.asBool = rae_json_extract_bool(json, "asBool");
  __r.asNumber = rae_json_extract_float(json, "asNumber");
  __r.asString = rae_json_extract_string(json, "asString");
  __r.rangeStart = rae_json_extract_int(json, "rangeStart");
  __r.rangeLen = rae_json_extract_int(json, "rangeLen");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_JsonDoc_(rae_JsonDoc* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"values\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"children\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"fields\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"rootIdx\": %lld", (long long)this->rootIdx);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"ok\": %s", this->ok ? "true" : "false");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"errorPos\": %lld", (long long)this->errorPos);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_JsonDoc rae_fromJson_rae_JsonDoc_(rae_String json) {
  rae_JsonDoc __r = {0};
  __r.rootIdx = rae_json_extract_int(json, "rootIdx");
  __r.ok = rae_json_extract_bool(json, "ok");
  __r.errorPos = rae_json_extract_int(json, "errorPos");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_JsonParser_(rae_JsonParser* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"source\": \"%.*s\"", (int)this->source.len, (char*)this->source.data);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"pos\": %lld", (long long)this->pos);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"ok\": %s", this->ok ? "true" : "false");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_JsonParser rae_fromJson_rae_JsonParser_(rae_String json) {
  rae_JsonParser __r = {0};
  __r.source = rae_json_extract_string(json, "source");
  __r.pos = rae_json_extract_int(json, "pos");
  __r.ok = rae_json_extract_bool(json, "ok");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_SdfGlyph_(rae_SdfGlyph* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"unicode\": %lld", (long long)this->unicode);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"advance\": %g", this->advance);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"hasBounds\": %s", this->hasBounds ? "true" : "false");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"planeLeft\": %g", this->planeLeft);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"planeBottom\": %g", this->planeBottom);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"planeRight\": %g", this->planeRight);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"planeTop\": %g", this->planeTop);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"atlasLeft\": %g", this->atlasLeft);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"atlasBottom\": %g", this->atlasBottom);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"atlasRight\": %g", this->atlasRight);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"atlasTop\": %g", this->atlasTop);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_SdfGlyph rae_fromJson_rae_SdfGlyph_(rae_String json) {
  rae_SdfGlyph __r = {0};
  __r.unicode = rae_json_extract_int(json, "unicode");
  __r.advance = rae_json_extract_float(json, "advance");
  __r.hasBounds = rae_json_extract_bool(json, "hasBounds");
  __r.planeLeft = rae_json_extract_float(json, "planeLeft");
  __r.planeBottom = rae_json_extract_float(json, "planeBottom");
  __r.planeRight = rae_json_extract_float(json, "planeRight");
  __r.planeTop = rae_json_extract_float(json, "planeTop");
  __r.atlasLeft = rae_json_extract_float(json, "atlasLeft");
  __r.atlasBottom = rae_json_extract_float(json, "atlasBottom");
  __r.atlasRight = rae_json_extract_float(json, "atlasRight");
  __r.atlasTop = rae_json_extract_float(json, "atlasTop");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_SdfFont_(rae_SdfFont* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"atlas\": %lld", (long long)this->atlas);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"atlasWidth\": %g", this->atlasWidth);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"atlasHeight\": %g", this->atlasHeight);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"pxRange\": %g", this->pxRange);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"emSize\": %g", this->emSize);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"lineHeight\": %g", this->lineHeight);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"ascender\": %g", this->ascender);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"glyphs\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_SdfFont rae_fromJson_rae_SdfFont_(rae_String json) {
  rae_SdfFont __r = {0};
  __r.atlas = rae_json_extract_int(json, "atlas");
  __r.atlasWidth = rae_json_extract_float(json, "atlasWidth");
  __r.atlasHeight = rae_json_extract_float(json, "atlasHeight");
  __r.pxRange = rae_json_extract_float(json, "pxRange");
  __r.emSize = rae_json_extract_float(json, "emSize");
  __r.lineHeight = rae_json_extract_float(json, "lineHeight");
  __r.ascender = rae_json_extract_float(json, "ascender");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_Vec3_(rae_Vec3* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"x\": %g", this->x);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"y\": %g", this->y);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"z\": %g", this->z);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Vec3 rae_fromJson_rae_Vec3_(rae_String json) {
  rae_Vec3 __r = {0};
  __r.x = rae_json_extract_float(json, "x");
  __r.y = rae_json_extract_float(json, "y");
  __r.z = rae_json_extract_float(json, "z");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_Material_(rae_Material* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"kind\": %lld", (long long)this->kind);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"albedo\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"fuzz\": %g", this->fuzz);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"ior\": %g", this->ior);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Material rae_fromJson_rae_Material_(rae_String json) {
  rae_Material __r = {0};
  __r.kind = rae_json_extract_int(json, "kind");
  __r.fuzz = rae_json_extract_float(json, "fuzz");
  __r.ior = rae_json_extract_float(json, "ior");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_Sphere_(rae_Sphere* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"center\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"radius\": %g", this->radius);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"material\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Sphere rae_fromJson_rae_Sphere_(rae_String json) {
  rae_Sphere __r = {0};
  __r.radius = rae_json_extract_float(json, "radius");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_Box_(rae_Box* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"lo\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"hi\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"material\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Box rae_fromJson_rae_Box_(rae_String json) {
  rae_Box __r = {0};
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_Triangle_(rae_Triangle* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"v0\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"v1\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"v2\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"n0\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"n1\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"n2\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"material\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Triangle rae_fromJson_rae_Triangle_(rae_String json) {
  rae_Triangle __r = {0};
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_Ray_(rae_Ray* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"origin\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"dir\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Ray rae_fromJson_rae_Ray_(rae_String json) {
  rae_Ray __r = {0};
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_Camera_(rae_Camera* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"origin\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"lowerLeft\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"horizontal\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"vertical\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"right\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"up\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"lensRadius\": %g", this->lensRadius);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_Camera rae_fromJson_rae_Camera_(rae_String json) {
  rae_Camera __r = {0};
  __r.lensRadius = rae_json_extract_float(json, "lensRadius");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_PrimRec_(rae_PrimRec* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"mnx\": %g", this->mnx);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"mny\": %g", this->mny);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"mnz\": %g", this->mnz);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"mxx\": %g", this->mxx);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"mxy\": %g", this->mxy);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"mxz\": %g", this->mxz);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"cx\": %g", this->cx);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"cy\": %g", this->cy);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"cz\": %g", this->cz);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"ref\": %lld", (long long)this->ref);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_PrimRec rae_fromJson_rae_PrimRec_(rae_String json) {
  rae_PrimRec __r = {0};
  __r.mnx = rae_json_extract_float(json, "mnx");
  __r.mny = rae_json_extract_float(json, "mny");
  __r.mnz = rae_json_extract_float(json, "mnz");
  __r.mxx = rae_json_extract_float(json, "mxx");
  __r.mxy = rae_json_extract_float(json, "mxy");
  __r.mxz = rae_json_extract_float(json, "mxz");
  __r.cx = rae_json_extract_float(json, "cx");
  __r.cy = rae_json_extract_float(json, "cy");
  __r.cz = rae_json_extract_float(json, "cz");
  __r.ref = rae_json_extract_int(json, "ref");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_GpuRT_(rae_GpuRT* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"sceneBuf\": %lld", (long long)this->sceneBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"boxBuf\": %lld", (long long)this->boxBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"triBuf\": %lld", (long long)this->triBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"bvhBuf\": %lld", (long long)this->bvhBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"refBuf\": %lld", (long long)this->refBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"accumBuf\": %lld", (long long)this->accumBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"outBuf\": %lld", (long long)this->outBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"paramsBuf\": %lld", (long long)this->paramsBuf);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"kernel\": %lld", (long long)this->kernel);
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_GpuRT rae_fromJson_rae_GpuRT_(rae_String json) {
  rae_GpuRT __r = {0};
  __r.sceneBuf = rae_json_extract_int(json, "sceneBuf");
  __r.boxBuf = rae_json_extract_int(json, "boxBuf");
  __r.triBuf = rae_json_extract_int(json, "triBuf");
  __r.bvhBuf = rae_json_extract_int(json, "bvhBuf");
  __r.refBuf = rae_json_extract_int(json, "refBuf");
  __r.accumBuf = rae_json_extract_int(json, "accumBuf");
  __r.outBuf = rae_json_extract_int(json, "outBuf");
  __r.paramsBuf = rae_json_extract_int(json, "paramsBuf");
  __r.kernel = rae_json_extract_int(json, "kernel");
  return __r;
}

RAE_UNUSED static rae_String rae_toJson_rae_SceneGpu_(rae_SceneGpu* this) {
  char __buf[4096]; int __p = 0;
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "{");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"nodes\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, ", ");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "\"refs\": ...");
  __p += snprintf(__buf + __p, sizeof(__buf) - __p, "}");
  return rae_json_build(__buf, __p);
}

RAE_UNUSED static rae_SceneGpu rae_fromJson_rae_SceneGpu_(rae_String json) {
  rae_SceneGpu __r = {0};
  return __r;
}

RAE_UNUSED static rae_String rae_to_str_rae_List2Int_(const rae_List2Int* this);
RAE_UNUSED static rae_String rae_to_str_rae_List2_(const rae_List2* this);
RAE_UNUSED static rae_String rae_to_str_rae_JsonField_(const rae_JsonField* this);
RAE_UNUSED static rae_String rae_to_str_rae_JsonValue_(const rae_JsonValue* this);
RAE_UNUSED static rae_String rae_to_str_rae_JsonDoc_(const rae_JsonDoc* this);
RAE_UNUSED static rae_String rae_to_str_rae_JsonParser_(const rae_JsonParser* this);
RAE_UNUSED static rae_String rae_to_str_rae_SdfGlyph_(const rae_SdfGlyph* this);
RAE_UNUSED static rae_String rae_to_str_rae_SdfFont_(const rae_SdfFont* this);
RAE_UNUSED static rae_String rae_to_str_rae_Vec3_(const rae_Vec3* this);
RAE_UNUSED static rae_String rae_to_str_rae_Material_(const rae_Material* this);
RAE_UNUSED static rae_String rae_to_str_rae_Sphere_(const rae_Sphere* this);
RAE_UNUSED static rae_String rae_to_str_rae_Box_(const rae_Box* this);
RAE_UNUSED static rae_String rae_to_str_rae_Triangle_(const rae_Triangle* this);
RAE_UNUSED static rae_String rae_to_str_rae_Ray_(const rae_Ray* this);
RAE_UNUSED static rae_String rae_to_str_rae_Camera_(const rae_Camera* this);
RAE_UNUSED static rae_String rae_to_str_rae_PrimRec_(const rae_PrimRec* this);
RAE_UNUSED static rae_String rae_to_str_rae_GpuRT_(const rae_GpuRT* this);
RAE_UNUSED static rae_String rae_to_str_rae_SceneGpu_(const rae_SceneGpu* this);

RAE_UNUSED static rae_String rae_to_str_rae_List2Int_(const rae_List2Int* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<Buffer>", 8});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->length));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->cap));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_List2_(const rae_List2* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<Buffer>", 8});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->length));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->cap));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_JsonField_(const rae_JsonField* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->key));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->valueIdx));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_JsonValue_(const rae_JsonValue* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->kind));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->asBool));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->asNumber));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->asString));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->rangeStart));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->rangeLen));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_JsonDoc_(const rae_JsonDoc* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<List>", 6});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<List>", 6});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<List>", 6});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->rootIdx));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->ok));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->errorPos));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_JsonParser_(const rae_JsonParser* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->source));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->pos));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->ok));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_SdfGlyph_(const rae_SdfGlyph* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->unicode));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->advance));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->hasBounds));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->planeLeft));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->planeBottom));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->planeRight));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->planeTop));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->atlasLeft));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->atlasBottom));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->atlasRight));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->atlasTop));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_SdfFont_(const rae_SdfFont* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->atlas));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->atlasWidth));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->atlasHeight));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->pxRange));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->emSize));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->lineHeight));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->ascender));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<List>", 6});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_Vec3_(const rae_Vec3* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->x));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->y));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->z));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_Material_(const rae_Material* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->kind));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->albedo));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->fuzz));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->ior));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_Sphere_(const rae_Sphere* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->center));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->radius));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Material_(&this->material));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_Box_(const rae_Box* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->lo));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->hi));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Material_(&this->material));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_Triangle_(const rae_Triangle* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->v0));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->v1));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->v2));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->n0));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->n1));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->n2));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Material_(&this->material));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_Ray_(const rae_Ray* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->origin));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->dir));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_Camera_(const rae_Camera* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->origin));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->lowerLeft));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->horizontal));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->vertical));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->right));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_to_str_rae_Vec3_(&this->up));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->lensRadius));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_PrimRec_(const rae_PrimRec* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->mnx));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->mny));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->mnz));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->mxx));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->mxy));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->mxz));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->cx));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->cy));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->cz));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->ref));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_GpuRT_(const rae_GpuRT* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->sceneBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->boxBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->triBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->bvhBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->refBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->accumBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->outBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->paramsBuf));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->kernel));
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static rae_String rae_to_str_rae_SceneGpu_(const rae_SceneGpu* this) {
  rae_String __out = (rae_String){(uint8_t*)"{ ", 2};
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<List>", 6});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)", ", 2});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)"<List>", 6});
  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)" }", 2});
  return __out;
}

RAE_UNUSED static void rae_drop_struct_rae_JsonField(rae_JsonField* this);
RAE_UNUSED static void rae_drop_struct_rae_JsonField_alias(rae_JsonField* this);
RAE_UNUSED static void rae_drop_struct_rae_JsonValue(rae_JsonValue* this);
RAE_UNUSED static void rae_drop_struct_rae_JsonValue_alias(rae_JsonValue* this);
RAE_UNUSED static void rae_drop_struct_rae_JsonDoc(rae_JsonDoc* this);
RAE_UNUSED static void rae_drop_struct_rae_JsonDoc_alias(rae_JsonDoc* this);
RAE_UNUSED static void rae_drop_struct_rae_JsonParser(rae_JsonParser* this);
RAE_UNUSED static void rae_drop_struct_rae_JsonParser_alias(rae_JsonParser* this);
RAE_UNUSED static void rae_drop_struct_rae_SdfFont(rae_SdfFont* this);
RAE_UNUSED static void rae_drop_struct_rae_SdfFont_alias(rae_SdfFont* this);
RAE_UNUSED static void rae_drop_struct_rae_SceneGpu(rae_SceneGpu* this);
RAE_UNUSED static void rae_drop_struct_rae_SceneGpu_alias(rae_SceneGpu* this);
RAE_UNUSED static void rae_drop_rae_JsonValue_rae_List_rae_JsonValue_(rae_List_rae_JsonValue* this);
RAE_UNUSED static void rae_drop_int64_t_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static void rae_drop_rae_JsonField_rae_List_rae_JsonField_(rae_List_rae_JsonField* this);
RAE_UNUSED static void rae_drop_rae_SdfGlyph_rae_List_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this);
RAE_UNUSED static void rae_drop_double_rae_List_double_(rae_List_double* this);
RAE_UNUSED static void rae_drop_double_rae_List_double_(rae_List_double* this);

RAE_UNUSED static void rae_drop_struct_rae_JsonField(rae_JsonField* this) {
  rae_string_drop(&this->key);
}

RAE_UNUSED static void rae_drop_struct_rae_JsonField_alias(rae_JsonField* this) {
}

RAE_UNUSED static void rae_drop_struct_rae_JsonValue(rae_JsonValue* this) {
  rae_string_drop(&this->asString);
}

RAE_UNUSED static void rae_drop_struct_rae_JsonValue_alias(rae_JsonValue* this) {
}

RAE_UNUSED static void rae_drop_struct_rae_JsonDoc(rae_JsonDoc* this) {
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&this->fields);
  rae_drop_int64_t_rae_List_int64_t_(&this->children);
  rae_drop_rae_JsonValue_rae_List_rae_JsonValue_(&this->values);
}

RAE_UNUSED static void rae_drop_struct_rae_JsonDoc_alias(rae_JsonDoc* this) {
  rae_drop_int64_t_rae_List_int64_t_(&this->children);
}

RAE_UNUSED static void rae_drop_struct_rae_JsonParser(rae_JsonParser* this) {
  rae_string_drop(&this->source);
}

RAE_UNUSED static void rae_drop_struct_rae_JsonParser_alias(rae_JsonParser* this) {
}

RAE_UNUSED static void rae_drop_struct_rae_SdfFont(rae_SdfFont* this) {
  rae_drop_rae_SdfGlyph_rae_List_rae_SdfGlyph_(&this->glyphs);
}

RAE_UNUSED static void rae_drop_struct_rae_SdfFont_alias(rae_SdfFont* this) {
  rae_drop_rae_SdfGlyph_rae_List_rae_SdfGlyph_(&this->glyphs);
}

RAE_UNUSED static void rae_drop_struct_rae_SceneGpu(rae_SceneGpu* this) {
  rae_drop_double_rae_List_double_(&this->refs);
  rae_drop_double_rae_List_double_(&this->nodes);
}

RAE_UNUSED static void rae_drop_struct_rae_SceneGpu_alias(rae_SceneGpu* this) {
  rae_drop_double_rae_List_double_(&this->refs);
  rae_drop_double_rae_List_double_(&this->nodes);
}

RAE_UNUSED static void rae_deep_copy_rae_JsonField(rae_JsonField* dst, const rae_JsonField* src);
RAE_UNUSED static void rae_deep_copy_rae_JsonValue(rae_JsonValue* dst, const rae_JsonValue* src);
RAE_UNUSED static void rae_deep_copy_rae_JsonDoc(rae_JsonDoc* dst, const rae_JsonDoc* src);
RAE_UNUSED static void rae_deep_copy_rae_JsonParser(rae_JsonParser* dst, const rae_JsonParser* src);
RAE_UNUSED static void rae_deep_copy_rae_SdfFont(rae_SdfFont* dst, const rae_SdfFont* src);
RAE_UNUSED static void rae_deep_copy_rae_SceneGpu(rae_SceneGpu* dst, const rae_SceneGpu* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_String(rae_List_rae_String* dst, const rae_List_rae_String* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_JsonValue(rae_List_rae_JsonValue* dst, const rae_List_rae_JsonValue* src);
RAE_UNUSED static void rae_deep_copy_rae_List_int64_t(rae_List_int64_t* dst, const rae_List_int64_t* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_JsonField(rae_List_rae_JsonField* dst, const rae_List_rae_JsonField* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_SdfGlyph(rae_List_rae_SdfGlyph* dst, const rae_List_rae_SdfGlyph* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_Triangle(rae_List_rae_Triangle* dst, const rae_List_rae_Triangle* src);
RAE_UNUSED static void rae_deep_copy_rae_List_double(rae_List_double* dst, const rae_List_double* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_Box(rae_List_rae_Box* dst, const rae_List_rae_Box* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_Sphere(rae_List_rae_Sphere* dst, const rae_List_rae_Sphere* src);
RAE_UNUSED static void rae_deep_copy_rae_List_rae_PrimRec(rae_List_rae_PrimRec* dst, const rae_List_rae_PrimRec* src);
#define rae_deep_copy_struct_rae_JsonField rae_deep_copy_rae_JsonField
#define rae_deep_copy_struct_rae_JsonValue rae_deep_copy_rae_JsonValue
#define rae_deep_copy_struct_rae_JsonDoc rae_deep_copy_rae_JsonDoc
#define rae_deep_copy_struct_rae_JsonParser rae_deep_copy_rae_JsonParser
#define rae_deep_copy_struct_rae_SdfFont rae_deep_copy_rae_SdfFont
#define rae_deep_copy_struct_rae_SceneGpu rae_deep_copy_rae_SceneGpu

RAE_UNUSED static void rae_deep_copy_rae_JsonField(rae_JsonField* dst, const rae_JsonField* src) {
  dst->key = rae_string_copy(src->key);
  dst->valueIdx = src->valueIdx;
}

RAE_UNUSED static void rae_deep_copy_rae_JsonValue(rae_JsonValue* dst, const rae_JsonValue* src) {
  dst->kind = src->kind;
  dst->asBool = src->asBool;
  dst->asNumber = src->asNumber;
  dst->asString = rae_string_copy(src->asString);
  dst->rangeStart = src->rangeStart;
  dst->rangeLen = src->rangeLen;
}

RAE_UNUSED static void rae_deep_copy_rae_JsonDoc(rae_JsonDoc* dst, const rae_JsonDoc* src) {
  rae_deep_copy_rae_List_rae_JsonValue(&dst->values, &src->values);
  rae_deep_copy_rae_List_int64_t(&dst->children, &src->children);
  rae_deep_copy_rae_List_rae_JsonField(&dst->fields, &src->fields);
  dst->rootIdx = src->rootIdx;
  dst->ok = src->ok;
  dst->errorPos = src->errorPos;
}

RAE_UNUSED static void rae_deep_copy_rae_JsonParser(rae_JsonParser* dst, const rae_JsonParser* src) {
  dst->source = rae_string_copy(src->source);
  dst->pos = src->pos;
  dst->ok = src->ok;
}

RAE_UNUSED static void rae_deep_copy_rae_SdfFont(rae_SdfFont* dst, const rae_SdfFont* src) {
  dst->atlas = src->atlas;
  dst->atlasWidth = src->atlasWidth;
  dst->atlasHeight = src->atlasHeight;
  dst->pxRange = src->pxRange;
  dst->emSize = src->emSize;
  dst->lineHeight = src->lineHeight;
  dst->ascender = src->ascender;
  rae_deep_copy_rae_List_rae_SdfGlyph(&dst->glyphs, &src->glyphs);
}

RAE_UNUSED static void rae_deep_copy_rae_SceneGpu(rae_SceneGpu* dst, const rae_SceneGpu* src) {
  rae_deep_copy_rae_List_double(&dst->nodes, &src->nodes);
  rae_deep_copy_rae_List_double(&dst->refs, &src->refs);
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_String(rae_List_rae_String* dst, const rae_List_rae_String* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_String*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_String));
    for (int64_t __i = 0; __i < src->length; __i++) {
      dst->data[__i] = rae_string_copy(src->data[__i]);
    }
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_JsonValue(rae_List_rae_JsonValue* dst, const rae_List_rae_JsonValue* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_JsonValue*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_JsonValue));
    for (int64_t __i = 0; __i < src->length; __i++) {
      rae_deep_copy_rae_JsonValue(&dst->data[__i], &src->data[__i]);
    }
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_int64_t(rae_List_int64_t* dst, const rae_List_int64_t* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (int64_t*)rae_ext_rae_buf_alloc(src->cap, sizeof(int64_t));
    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(int64_t));
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_JsonField(rae_List_rae_JsonField* dst, const rae_List_rae_JsonField* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_JsonField*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_JsonField));
    for (int64_t __i = 0; __i < src->length; __i++) {
      rae_deep_copy_rae_JsonField(&dst->data[__i], &src->data[__i]);
    }
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_SdfGlyph(rae_List_rae_SdfGlyph* dst, const rae_List_rae_SdfGlyph* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_SdfGlyph*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_SdfGlyph));
    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(rae_SdfGlyph));
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_Triangle(rae_List_rae_Triangle* dst, const rae_List_rae_Triangle* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_Triangle*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_Triangle));
    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(rae_Triangle));
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_double(rae_List_double* dst, const rae_List_double* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (double*)rae_ext_rae_buf_alloc(src->cap, sizeof(double));
    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(double));
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_Box(rae_List_rae_Box* dst, const rae_List_rae_Box* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_Box*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_Box));
    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(rae_Box));
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_Sphere(rae_List_rae_Sphere* dst, const rae_List_rae_Sphere* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_Sphere*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_Sphere));
    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(rae_Sphere));
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static void rae_deep_copy_rae_List_rae_PrimRec(rae_List_rae_PrimRec* dst, const rae_List_rae_PrimRec* src) {
  dst->length = src->length;
  dst->cap = src->cap;
  if (src->cap > 0) {
    dst->data = (rae_PrimRec*)rae_ext_rae_buf_alloc(src->cap, sizeof(rae_PrimRec));
    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(rae_PrimRec));
  } else {
    dst->data = NULL;
  }
}

RAE_UNUSED static double pi = 3.1415926535897931;
RAE_UNUSED static double tau = 6.2831853071795862;

int64_t rae_ext_nextTick(void);
int64_t rae_ext_nowMs(void);
int64_t rae_ext_nowNs(void);
void rae_ext_rae_sleep(int64_t ms);
double rae_ext_math_sin(double x);
double rae_ext_math_cos(double x);
double rae_ext_math_tan(double x);
double rae_ext_math_asin(double x);
double rae_ext_math_acos(double x);
double rae_ext_math_atan(double x);
double rae_ext_math_atan2(double y, double x);
double rae_ext_math_sqrt(double x);
double rae_ext_math_pow(double base, double exp);
double rae_ext_math_exp(double x);
double rae_ext_math_math_log(double x);
double rae_ext_math_floor(double x);
double rae_ext_math_ceil(double x);
double rae_ext_math_round(double x);
int64_t rae_ext_sdf_text_loadAtlas(rae_View_String path, int64_t w, int64_t h);
void rae_ext_sdf_text_blitGlyph(int64_t* fb, int64_t fbW, int64_t fbH, int64_t atlas, double sx0, double sy0, double sx1, double sy1, double au0, double av0, double au1, double av1, double screenPxRange, int64_t rgb);
int64_t rae_ext_gpu_storageF32(const double* data, int64_t count);
int64_t rae_ext_gpu_allocF32(int64_t count);
int64_t rae_ext_gpu_uniformU32(const int64_t* data, int64_t count);
int64_t rae_ext_gpu_allocU32(int64_t count);
void rae_ext_gpu_writeF32(int64_t buf, const double* data, int64_t count);
void rae_ext_gpu_writeU32(int64_t buf, const int64_t* data, int64_t count);
int64_t rae_ext_gpu_kernel(rae_View_String wgsl, rae_View_String entry);
void rae_ext_gpu_run(int64_t kernel, const int64_t* bufs, int64_t bufCount, int64_t gx, int64_t gy, int64_t gz);
void rae_ext_gpu_downloadF32(int64_t buf, double* out, int64_t count);
void rae_ext_gpu_downloadU32(int64_t buf, int64_t* out, int64_t count);
void rae_ext_gpu_reset(void);
void rae_ext_sdl3_initWindow(int64_t width, int64_t height, rae_String title);
void rae_ext_sdl3_setTargetFPS(int64_t fps);
rae_Bool rae_ext_sdl3_shouldClose(void);
void rae_ext_sdl3_updatePixels(const int64_t* pixels, int64_t w, int64_t h);
void rae_ext_sdl3_present(void);
void rae_ext_sdl3_setTitle(rae_String title);
void rae_ext_sdl3_closeWindow(void);
int64_t rae_ext_sdl3_windowWidth(void);
int64_t rae_ext_sdl3_windowHeight(void);
int64_t rae_ext_sdl3_getMouseX(void);
int64_t rae_ext_sdl3_getMouseY(void);
rae_Bool rae_ext_sdl3_isMouseButtonDown(int64_t button);
rae_Bool rae_ext_sdl3_isKeyDown(int64_t key);
rae_Bool rae_ext_sdl3_isKeyPressed(int64_t key);
rae_Bool rae_ext_image_savePng(rae_View_String path, const int64_t* pixels, int64_t width, int64_t height);
void* rae_ext_image_loadPng(rae_View_String path, rae_Mod_Int64 width, rae_Mod_Int64 height);
rae_String rae_ext_filesystem_userFolder(int64_t kind);
rae_String rae_ext_filesystem_prefDir(rae_View_String org, rae_View_String app);
rae_Bool rae_ext_filesystem_makeDir(rae_View_String path);
rae_Bool rae_ext_filesystem_exists(rae_View_String path);
rae_String rae_ext_filesystem_today(void);
int64_t rae_ext_filesystem_nextIndex(rae_View_String dir, rae_View_String prefix);
RAE_UNUSED static void rae_log_RaeAny_(RaeAny value);
RAE_UNUSED static void rae_logS_RaeAny_(RaeAny value);
RAE_UNUSED static double rae_toFloat_rae_View_Int64_(int64_t this);
RAE_UNUSED static int64_t rae_toInt_rae_View_Float64_(double this);
RAE_UNUSED static rae_String rae_stringCopy_rae_View_String_(rae_View_String s);
RAE_UNUSED static rae_String rae_fromCStr_void_p_(const void* s);
RAE_UNUSED static void* rae_toCStr_rae_View_String_(rae_View_String this);
RAE_UNUSED static uint32_t rae_at_rae_View_String_rae_View_Int64_(rae_View_String this, int64_t index);
RAE_UNUSED static int64_t rae_length_rae_View_String_(rae_View_String this);
RAE_UNUSED static int64_t rae_compare_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String other);
RAE_UNUSED static rae_Bool rae_equals_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String other);
RAE_UNUSED static int64_t rae_hash_rae_View_String_(rae_View_String this);
RAE_UNUSED static rae_String rae_concat_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String other);
RAE_UNUSED static rae_String rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_(rae_View_String this, int64_t start, int64_t len);
RAE_UNUSED static rae_Bool rae_contains_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sub);
RAE_UNUSED static rae_Bool rae_startsWith_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String prefix);
RAE_UNUSED static rae_Bool rae_endsWith_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String suffix);
RAE_UNUSED static int64_t rae_indexOf_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sub);
RAE_UNUSED static int64_t rae_lastIndexOf_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sub);
RAE_UNUSED static rae_String rae_toLower_rae_View_String_(rae_View_String this);
RAE_UNUSED static rae_String rae_trim_rae_View_String_(rae_View_String this);
RAE_UNUSED static rae_List_rae_String rae_split_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sep);
RAE_UNUSED static rae_String rae_replace_rae_View_String_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String old, rae_View_String new);
RAE_UNUSED static rae_String rae_join_rae_List_rae_String_rae_View_String_(rae_List_rae_String* this, rae_View_String sep);
RAE_UNUSED static double rae_toFloat_rae_View_String_(rae_View_String this);
RAE_UNUSED static int64_t rae_toInt_rae_View_String_(rae_View_String this);
RAE_UNUSED static void rae_seed_rae_View_Int64_(int64_t n);
RAE_UNUSED static double rae_random_(void);
RAE_UNUSED static int64_t rae_random_rae_View_Int64_rae_View_Int64_(int64_t min, int64_t max);
RAE_UNUSED static int64_t rae_abs_rae_View_Int64_(int64_t n);
RAE_UNUSED static double rae_abs_rae_View_Float64_(double n);
RAE_UNUSED static int64_t rae_min_rae_View_Int64_rae_View_Int64_(int64_t a, int64_t b);
RAE_UNUSED static double rae_min_rae_View_Float64_rae_View_Float64_(double a, double b);
RAE_UNUSED static int64_t rae_max_rae_View_Int64_rae_View_Int64_(int64_t a, int64_t b);
RAE_UNUSED static double rae_max_rae_View_Float64_rae_View_Float64_(double a, double b);
RAE_UNUSED static int64_t rae_clamp_rae_View_Int64_rae_View_Int64_rae_View_Int64_(int64_t val, int64_t low, int64_t high);
RAE_UNUSED static double rae_clamp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(double val, double low, double high);
RAE_UNUSED static double rae_lerp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(double a, double b, double t);
RAE_UNUSED static double rae_randomFloat_rae_View_Float64_rae_View_Float64_(double min, double max);
RAE_UNUSED static int64_t rae_randomInt_rae_View_Int64_rae_View_Int64_(int64_t min, int64_t max);
RAE_UNUSED static rae_String rae_readLine_(void);
RAE_UNUSED static uint32_t rae_readChar_(void);
RAE_UNUSED static void rae_exit_rae_View_Int64_(int64_t code);
RAE_UNUSED static rae_String rae_getEnv_rae_View_String_(rae_View_String name);
RAE_UNUSED static RaeAny rae_readFile_rae_View_String_(rae_View_String path);
RAE_UNUSED static rae_Bool rae_writeFile_rae_View_String_rae_View_String_(rae_View_String path, rae_View_String content);
RAE_UNUSED static rae_Bool rae_rename_rae_View_String_rae_View_String_(rae_View_String oldPath, rae_View_String newPath);
RAE_UNUSED static rae_Bool rae_delete_rae_View_String_(rae_View_String path);
RAE_UNUSED static rae_Bool rae_makeDir_rae_View_String_(rae_View_String path);
RAE_UNUSED static rae_Bool rae_exists_rae_View_String_(rae_View_String path);
RAE_UNUSED static rae_Bool rae_lockFile_rae_View_String_(rae_View_String path);
RAE_UNUSED static rae_Bool rae_unlockFile_rae_View_String_(rae_View_String path);
RAE_UNUSED static double rae_fileModTime_rae_View_String_(rae_View_String path);
RAE_UNUSED static int64_t rae_processRssKb_(void);
RAE_UNUSED static void rae_encrypt_RaeAny_RaeAny_RaeAny_rae_View_Int64_RaeAny_RaeAny_(RaeAny key, RaeAny nonce, RaeAny plain, int64_t len, RaeAny mac, RaeAny cipher);
RAE_UNUSED static rae_Bool rae_decrypt_RaeAny_RaeAny_RaeAny_RaeAny_rae_View_Int64_RaeAny_(RaeAny key, RaeAny nonce, RaeAny mac, RaeAny cipher, int64_t len, RaeAny plain);
RAE_UNUSED static rae_List2Int rae_createList2Int_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_grow_rae_List2Int_(rae_List2Int* this);
RAE_UNUSED static void rae_add_rae_List2Int_rae_View_Int64_(rae_List2Int* this, int64_t value);
RAE_UNUSED static int64_t rae_get_rae_List2Int_rae_View_Int64_(rae_List2Int* this, int64_t index);
RAE_UNUSED static int64_t rae_length_rae_List2Int_(rae_List2Int* this);
RAE_UNUSED static rae_List2 rae_createList2_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_grow_rae_List2_(rae_List2* this);
RAE_UNUSED static void rae_add_rae_List2_RaeAny_(rae_List2* this, RaeAny value);
RAE_UNUSED static RaeAny rae_get_rae_List2_rae_View_Int64_(rae_List2* this, int64_t index);
RAE_UNUSED static int64_t rae_length_rae_List2_(rae_List2* this);
RAE_UNUSED static rae_Bool rae_isDigit_rae_View_Char_(uint32_t this);
RAE_UNUSED static rae_Bool rae_isWhitespace_rae_View_Char_(uint32_t this);
RAE_UNUSED static rae_Bool rae_isUpper_rae_View_Char_(uint32_t this);
RAE_UNUSED static rae_Bool rae_isLower_rae_View_Char_(uint32_t this);
RAE_UNUSED static rae_Bool rae_isLetter_rae_View_Char_(uint32_t this);
RAE_UNUSED static rae_Bool rae_isAlnum_rae_View_Char_(uint32_t this);
RAE_UNUSED static uint32_t rae_toLower_rae_View_Char_(uint32_t this);
RAE_UNUSED static uint32_t rae_toUpper_rae_View_Char_(uint32_t this);
RAE_UNUSED static rae_JsonValue rae_jsonNull_(void);
RAE_UNUSED static rae_JsonValue rae_jsonBoolValue_rae_View_Bool_(rae_Bool v);
RAE_UNUSED static rae_JsonValue rae_jsonNumberValue_rae_View_Float64_(double v);
RAE_UNUSED static rae_JsonValue rae_jsonStringValue_rae_String_(rae_String v);
RAE_UNUSED static rae_JsonValue rae_valueAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx);
RAE_UNUSED static int64_t rae_childAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx);
RAE_UNUSED static rae_JsonField rae_fieldAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx);
RAE_UNUSED static rae_JsonValue rae_jsonRoot_rae_JsonDoc_(rae_JsonDoc* doc);
RAE_UNUSED static int64_t rae_jsonInt_rae_JsonValue_rae_View_Int64_(rae_JsonValue* this, int64_t fallback);
RAE_UNUSED static double rae_jsonFloat_rae_JsonValue_rae_View_Float64_(rae_JsonValue* this, double fallback);
RAE_UNUSED static rae_String rae_jsonString_rae_JsonValue_rae_View_String_(rae_JsonValue* this, rae_View_String fallback);
RAE_UNUSED static rae_Bool rae_jsonBool_rae_JsonValue_rae_View_Bool_(rae_JsonValue* this, rae_Bool fallback);
RAE_UNUSED static int64_t rae_jsonArrayLen_rae_JsonValue_(rae_JsonValue* this);
RAE_UNUSED static rae_JsonValue rae_jsonArrayAt_rae_JsonDoc_rae_JsonValue_rae_View_Int64_(rae_JsonDoc* doc, rae_JsonValue* this, int64_t idx);
RAE_UNUSED static int64_t rae_jsonObjectLen_rae_JsonValue_(rae_JsonValue* this);
RAE_UNUSED static rae_String rae_jsonObjectKeyAt_rae_JsonDoc_rae_JsonValue_rae_View_Int64_(rae_JsonDoc* doc, rae_JsonValue* this, int64_t idx);
RAE_UNUSED static rae_JsonValue rae_jsonObjectValueAt_rae_JsonDoc_rae_JsonValue_rae_View_Int64_(rae_JsonDoc* doc, rae_JsonValue* this, int64_t idx);
RAE_UNUSED static int64_t rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(rae_JsonDoc* doc, rae_JsonValue* this, rae_View_String key);
RAE_UNUSED static rae_JsonValue rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx);
RAE_UNUSED static void rae_skipWhitespace_rae_JsonParser_(rae_JsonParser* p);
RAE_UNUSED static uint32_t rae_peek_rae_JsonParser_(rae_JsonParser* p);
RAE_UNUSED static rae_Bool rae_consume_rae_JsonParser_rae_View_Char_(rae_JsonParser* p, uint32_t c);
RAE_UNUSED static rae_Bool rae_consumeKeyword_rae_JsonParser_rae_View_String_(rae_JsonParser* p, rae_View_String kw);
RAE_UNUSED static int64_t rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(rae_List_rae_JsonValue* vals, rae_JsonValue v);
RAE_UNUSED static int64_t rae_pushChild_rae_List_int64_t_rae_View_Int64_(rae_List_int64_t* kids, int64_t valueIdx);
RAE_UNUSED static int64_t rae_pushField_rae_List_rae_JsonField_rae_String_rae_View_Int64_(rae_List_rae_JsonField* fields, rae_String key, int64_t valueIdx);
RAE_UNUSED static rae_String rae_parseString_rae_JsonParser_(rae_JsonParser* p);
RAE_UNUSED static double rae_parseNumber_rae_JsonParser_(rae_JsonParser* p);
RAE_UNUSED static int64_t rae_parseValue_rae_JsonParser_rae_List_rae_JsonValue_rae_List_int64_t_rae_List_rae_JsonField_(rae_JsonParser* p, rae_List_rae_JsonValue* vals, rae_List_int64_t* kids, rae_List_rae_JsonField* fields);
RAE_UNUSED static rae_JsonDoc rae_parseJson_rae_View_String_(rae_View_String source);
RAE_UNUSED static double rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(rae_JsonDoc* doc, rae_JsonValue* obj, rae_View_String key, double fallback);
RAE_UNUSED static rae_Bool rae_sdfReadBounds_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_(rae_JsonDoc* doc, rae_JsonValue* parent, rae_View_String subKey, rae_Mod_Float64 outLeft, rae_Mod_Float64 outBottom, rae_Mod_Float64 outRight, rae_Mod_Float64 outTop);
RAE_UNUSED static rae_SdfFont rae_loadSdfFont_rae_View_String_rae_View_String_(rae_View_String jsonPath, rae_View_String rawAtlasPath);
RAE_UNUSED static int64_t rae_sdfGlyphIndex_rae_SdfFont_rae_View_Int64_(rae_SdfFont* font, int64_t cp);
RAE_UNUSED static double rae_sdfMeasureText_rae_SdfFont_rae_View_String_rae_View_Float64_(rae_SdfFont* font, rae_View_String text, double sizePx);
RAE_UNUSED static void rae_sdfDrawText_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Int64_(int64_t* fb, int64_t fbW, int64_t fbH, rae_SdfFont* font, rae_View_String text, double x, double y, double sizePx, int64_t rgb);
RAE_UNUSED static rae_String rae_desktopDir_(void);
RAE_UNUSED static rae_String rae_picturesDir_(void);
RAE_UNUSED static rae_String rae_documentsDir_(void);
RAE_UNUSED static rae_String rae_homeDir_(void);
RAE_UNUSED static rae_String rae_join_rae_View_String_rae_View_String_(rae_View_String a, rae_View_String b);
RAE_UNUSED static rae_String rae_pad4_rae_View_Int64_(int64_t n);
RAE_UNUSED static rae_String rae_renderPath_rae_View_String_(rae_View_String stem);
RAE_UNUSED static rae_Vec3 rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(double x, double y, double z);
RAE_UNUSED static rae_Vec3 rae_vadd_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b);
RAE_UNUSED static rae_Vec3 rae_vsub_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b);
RAE_UNUSED static rae_Vec3 rae_vscale_rae_Vec3_rae_View_Float64_(rae_Vec3* a, double s);
RAE_UNUSED static rae_Vec3 rae_vmul_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b);
RAE_UNUSED static rae_Vec3 rae_vneg_rae_Vec3_(rae_Vec3* a);
RAE_UNUSED static double rae_vdot_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b);
RAE_UNUSED static rae_Vec3 rae_vcross_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b);
RAE_UNUSED static double rae_vlenSq_rae_Vec3_(rae_Vec3* a);
RAE_UNUSED static double rae_vlen_rae_Vec3_(rae_Vec3* a);
RAE_UNUSED static rae_Vec3 rae_vnorm_rae_Vec3_(rae_Vec3* a);
RAE_UNUSED static int64_t rae_faceIndex_rae_View_String_(rae_View_String token);
RAE_UNUSED static int64_t rae_faceNormalIndex_rae_View_String_(rae_View_String token);
RAE_UNUSED static rae_List_rae_Triangle rae_loadObjTriangles_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Bool_rae_Material_(rae_View_String path, double scale, double ox, double oy, double oz, rae_Bool yaw180, rae_Material* mat);
RAE_UNUSED static rae_List_rae_Box rae_buildSceneOneBoxes_(void);
RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneDiffuse_(void);
RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneLights_(void);
RAE_UNUSED static rae_List_rae_Sphere rae_buildScene_(void);
RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneOne_(void);
RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneBook_(void);
RAE_UNUSED static rae_Camera rae_buildCamera_rae_Vec3_rae_Vec3_rae_View_Float64_rae_View_Float64_rae_View_Float64_(rae_Vec3* pos, rae_Vec3* target, double vfovDeg, double aperture, double aspect);
RAE_UNUSED static rae_Ray rae_makeRay_rae_Camera_rae_View_Float64_rae_View_Float64_(rae_Camera* cam, double u, double v);
RAE_UNUSED static double rae_fmin_rae_View_Float64_rae_View_Float64_(double a, double b);
RAE_UNUSED static double rae_fmax_rae_View_Float64_rae_View_Float64_(double a, double b);
RAE_UNUSED static int64_t rae_buildNode_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(rae_List_rae_PrimRec* prims, int64_t lo, int64_t hi, rae_List_double* nodes, int64_t leafSize);
RAE_UNUSED static int64_t rae_buildNodeRight_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(rae_List_rae_PrimRec* prims, int64_t lo, int64_t m, int64_t hi, rae_List_double* nodes, int64_t leafSize);
RAE_UNUSED static rae_List_double rae_buildBvh_rae_List_rae_PrimRec_rae_View_Int64_(rae_List_rae_PrimRec* prims, int64_t leafSize);
RAE_UNUSED static int64_t rae_triBase_(void);
RAE_UNUSED static rae_List_double rae_triFloats_rae_List_rae_Triangle_(rae_List_rae_Triangle* tris);
RAE_UNUSED static rae_SceneGpu rae_buildSceneBvh_rae_List_rae_Sphere_rae_List_rae_Triangle_(rae_List_rae_Sphere* world, rae_List_rae_Triangle* tris);
RAE_UNUSED static rae_List_double rae_boxFloats_rae_List_rae_Box_(rae_List_rae_Box* boxes);
RAE_UNUSED static void rae_pushVec3_rae_List_double_rae_Vec3_(rae_List_double* buf, rae_Vec3* a);
RAE_UNUSED static rae_List_double rae_sceneFloats_rae_Camera_rae_List_rae_Sphere_(rae_Camera* cam, rae_List_rae_Sphere* world);
RAE_UNUSED static rae_GpuRT rae_buildGpuRT_rae_View_String_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_int64_t_rae_View_Int64_(rae_View_String wgsl, rae_List_double* scene, rae_List_double* boxes, rae_List_double* tris, rae_List_double* nodes, rae_List_double* refs, rae_List_int64_t* params, int64_t pixels);
RAE_UNUSED static void rae_drawLabel_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_(int64_t* fb, int64_t w, int64_t h, rae_SdfFont* font, rae_View_String text, double x, double y, double px);
RAE_UNUSED static rae_List_rae_String rae_createList_rae_String_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_String_rae_List_rae_String_(rae_List_rae_String* this);
RAE_UNUSED static void rae_add_rae_String_rae_List_rae_String_rae_String_(rae_List_rae_String* this, rae_String value);
RAE_UNUSED static RaeAny rae_get_rae_String_rae_List_rae_String_rae_View_Int64_(rae_List_rae_String* this, int64_t index);
RAE_UNUSED static void rae_add_rae_JsonValue_rae_List_rae_JsonValue_rae_JsonValue_(rae_List_rae_JsonValue* this, rae_JsonValue value);
RAE_UNUSED static void rae_add_int64_t_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value);
RAE_UNUSED static void rae_add_rae_JsonField_rae_List_rae_JsonField_rae_JsonField_(rae_List_rae_JsonField* this, rae_JsonField value);
RAE_UNUSED static rae_List_rae_JsonField rae_createList_rae_JsonField_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_JsonField_rae_List_rae_JsonField_(rae_List_rae_JsonField* this);
RAE_UNUSED static rae_List_int64_t rae_createList_int64_t_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_int64_t_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static rae_List_rae_JsonValue rae_createList_rae_JsonValue_rae_View_Int64_(int64_t cap);
RAE_UNUSED static rae_List_rae_SdfGlyph rae_createList_rae_SdfGlyph_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_SdfGlyph_rae_List_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this);
RAE_UNUSED static void rae_add_rae_SdfGlyph_rae_List_rae_SdfGlyph_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this, rae_SdfGlyph value);
RAE_UNUSED static rae_List_double rae_createList_double_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_double_rae_List_double_(rae_List_double* this);
RAE_UNUSED static void rae_add_double_rae_List_double_double_(rae_List_double* this, double value);
RAE_UNUSED static rae_List_rae_Triangle rae_createList_rae_Triangle_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_Triangle_rae_List_rae_Triangle_(rae_List_rae_Triangle* this);
RAE_UNUSED static double rae_at_double_rae_List_double_rae_View_Int64_(rae_List_double* this, int64_t index);
RAE_UNUSED static void rae_add_rae_Triangle_rae_List_rae_Triangle_rae_Triangle_(rae_List_rae_Triangle* this, rae_Triangle value);
RAE_UNUSED static rae_List_rae_Box rae_createList_rae_Box_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_Box_rae_List_rae_Box_(rae_List_rae_Box* this);
RAE_UNUSED static void rae_add_rae_Box_rae_List_rae_Box_rae_Box_(rae_List_rae_Box* this, rae_Box value);
RAE_UNUSED static rae_List_rae_Sphere rae_createList_rae_Sphere_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_Sphere_rae_List_rae_Sphere_(rae_List_rae_Sphere* this);
RAE_UNUSED static void rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(rae_List_rae_Sphere* this, rae_Sphere value);
RAE_UNUSED static void rae_set_double_rae_List_double_rae_View_Int64_double_(rae_List_double* this, int64_t index, double value);
RAE_UNUSED static void rae_set_rae_PrimRec_rae_List_rae_PrimRec_rae_View_Int64_rae_PrimRec_(rae_List_rae_PrimRec* this, int64_t index, rae_PrimRec value);
RAE_UNUSED static rae_List_rae_PrimRec rae_createList_rae_PrimRec_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_PrimRec_rae_List_rae_PrimRec_(rae_List_rae_PrimRec* this);
RAE_UNUSED static void rae_add_rae_PrimRec_rae_List_rae_PrimRec_rae_PrimRec_(rae_List_rae_PrimRec* this, rae_PrimRec value);
RAE_UNUSED static rae_Sphere rae_at_rae_Sphere_rae_List_rae_Sphere_rae_View_Int64_(rae_List_rae_Sphere* this, int64_t index);
RAE_UNUSED static void rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(rae_List_int64_t* this, int64_t index, int64_t value);
RAE_UNUSED static void rae_grow_rae_String_rae_List_rae_String_(rae_List_rae_String* this);
RAE_UNUSED static void rae_grow_rae_JsonValue_rae_List_rae_JsonValue_(rae_List_rae_JsonValue* this);
RAE_UNUSED static void rae_grow_int64_t_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static void rae_grow_rae_JsonField_rae_List_rae_JsonField_(rae_List_rae_JsonField* this);
RAE_UNUSED static void rae_grow_rae_SdfGlyph_rae_List_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this);
RAE_UNUSED static void rae_grow_double_rae_List_double_(rae_List_double* this);
RAE_UNUSED static void rae_grow_rae_Triangle_rae_List_rae_Triangle_(rae_List_rae_Triangle* this);
RAE_UNUSED static void rae_grow_rae_Box_rae_List_rae_Box_(rae_List_rae_Box* this);
RAE_UNUSED static void rae_grow_rae_Sphere_rae_List_rae_Sphere_(rae_List_rae_Sphere* this);
RAE_UNUSED static void rae_grow_rae_PrimRec_rae_List_rae_PrimRec_(rae_List_rae_PrimRec* this);
RAE_UNUSED static void rae_drop_rae_JsonValue_rae_List_rae_JsonValue_(rae_List_rae_JsonValue* this);
typedef struct { int64_t f0; RaeTask* __task; } __raespawn_args_rae_toFloat_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_toFloat_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_toFloat_rae_View_Int64_* __a = (__raespawn_args_rae_toFloat_rae_View_Int64_*)__vp;
  *(double*)__a->__task->result = rae_toFloat_rae_View_Int64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; RaeTask* __task; } __raespawn_args_rae_toInt_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_toInt_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_toInt_rae_View_Float64_* __a = (__raespawn_args_rae_toInt_rae_View_Float64_*)__vp;
  *(int64_t*)__a->__task->result = rae_toInt_rae_View_Float64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; RaeTask* __task; } __raespawn_args_rae_seed_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_seed_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_seed_rae_View_Int64_* __a = (__raespawn_args_rae_seed_rae_View_Int64_*)__vp;
  rae_seed_rae_View_Int64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_random_;
RAE_UNUSED static void* __raespawn_thunk_rae_random_(void* __vp) {
  __raespawn_args_rae_random_* __a = (__raespawn_args_rae_random_*)__vp;
  *(double*)__a->__task->result = rae_random_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; int64_t f1; RaeTask* __task; } __raespawn_args_rae_random_rae_View_Int64_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_random_rae_View_Int64_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_random_rae_View_Int64_rae_View_Int64_* __a = (__raespawn_args_rae_random_rae_View_Int64_rae_View_Int64_*)__vp;
  *(int64_t*)__a->__task->result = rae_random_rae_View_Int64_rae_View_Int64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; RaeTask* __task; } __raespawn_args_rae_abs_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_abs_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_abs_rae_View_Int64_* __a = (__raespawn_args_rae_abs_rae_View_Int64_*)__vp;
  *(int64_t*)__a->__task->result = rae_abs_rae_View_Int64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; RaeTask* __task; } __raespawn_args_rae_abs_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_abs_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_abs_rae_View_Float64_* __a = (__raespawn_args_rae_abs_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_abs_rae_View_Float64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; int64_t f1; RaeTask* __task; } __raespawn_args_rae_min_rae_View_Int64_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_min_rae_View_Int64_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_min_rae_View_Int64_rae_View_Int64_* __a = (__raespawn_args_rae_min_rae_View_Int64_rae_View_Int64_*)__vp;
  *(int64_t*)__a->__task->result = rae_min_rae_View_Int64_rae_View_Int64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; RaeTask* __task; } __raespawn_args_rae_min_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_min_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_min_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_min_rae_View_Float64_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_min_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; int64_t f1; RaeTask* __task; } __raespawn_args_rae_max_rae_View_Int64_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_max_rae_View_Int64_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_max_rae_View_Int64_rae_View_Int64_* __a = (__raespawn_args_rae_max_rae_View_Int64_rae_View_Int64_*)__vp;
  *(int64_t*)__a->__task->result = rae_max_rae_View_Int64_rae_View_Int64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; RaeTask* __task; } __raespawn_args_rae_max_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_max_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_max_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_max_rae_View_Float64_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_max_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; int64_t f1; int64_t f2; RaeTask* __task; } __raespawn_args_rae_clamp_rae_View_Int64_rae_View_Int64_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_clamp_rae_View_Int64_rae_View_Int64_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_clamp_rae_View_Int64_rae_View_Int64_rae_View_Int64_* __a = (__raespawn_args_rae_clamp_rae_View_Int64_rae_View_Int64_rae_View_Int64_*)__vp;
  *(int64_t*)__a->__task->result = rae_clamp_rae_View_Int64_rae_View_Int64_rae_View_Int64_(__a->f0, __a->f1, __a->f2);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; double f2; RaeTask* __task; } __raespawn_args_rae_clamp_rae_View_Float64_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_clamp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_clamp_rae_View_Float64_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_clamp_rae_View_Float64_rae_View_Float64_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_clamp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1, __a->f2);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; double f2; RaeTask* __task; } __raespawn_args_rae_lerp_rae_View_Float64_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_lerp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_lerp_rae_View_Float64_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_lerp_rae_View_Float64_rae_View_Float64_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_lerp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1, __a->f2);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; RaeTask* __task; } __raespawn_args_rae_randomFloat_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_randomFloat_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_randomFloat_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_randomFloat_rae_View_Float64_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_randomFloat_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; int64_t f1; RaeTask* __task; } __raespawn_args_rae_randomInt_rae_View_Int64_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_randomInt_rae_View_Int64_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_randomInt_rae_View_Int64_rae_View_Int64_* __a = (__raespawn_args_rae_randomInt_rae_View_Int64_rae_View_Int64_*)__vp;
  *(int64_t*)__a->__task->result = rae_randomInt_rae_View_Int64_rae_View_Int64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_readLine_;
RAE_UNUSED static void* __raespawn_thunk_rae_readLine_(void* __vp) {
  __raespawn_args_rae_readLine_* __a = (__raespawn_args_rae_readLine_*)__vp;
  *(rae_String*)__a->__task->result = rae_readLine_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_readChar_;
RAE_UNUSED static void* __raespawn_thunk_rae_readChar_(void* __vp) {
  __raespawn_args_rae_readChar_* __a = (__raespawn_args_rae_readChar_*)__vp;
  *(uint32_t*)__a->__task->result = rae_readChar_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; RaeTask* __task; } __raespawn_args_rae_exit_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_exit_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_exit_rae_View_Int64_* __a = (__raespawn_args_rae_exit_rae_View_Int64_*)__vp;
  rae_exit_rae_View_Int64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_processRssKb_;
RAE_UNUSED static void* __raespawn_thunk_rae_processRssKb_(void* __vp) {
  __raespawn_args_rae_processRssKb_* __a = (__raespawn_args_rae_processRssKb_*)__vp;
  *(int64_t*)__a->__task->result = rae_processRssKb_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; RaeTask* __task; } __raespawn_args_rae_createList2Int_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_createList2Int_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_createList2Int_rae_View_Int64_* __a = (__raespawn_args_rae_createList2Int_rae_View_Int64_*)__vp;
  *(rae_List2Int*)__a->__task->result = rae_createList2Int_rae_View_Int64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; RaeTask* __task; } __raespawn_args_rae_createList2_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_createList2_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_createList2_rae_View_Int64_* __a = (__raespawn_args_rae_createList2_rae_View_Int64_*)__vp;
  *(rae_List2*)__a->__task->result = rae_createList2_rae_View_Int64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_isDigit_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_isDigit_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_isDigit_rae_View_Char_* __a = (__raespawn_args_rae_isDigit_rae_View_Char_*)__vp;
  *(rae_Bool*)__a->__task->result = rae_isDigit_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_isWhitespace_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_isWhitespace_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_isWhitespace_rae_View_Char_* __a = (__raespawn_args_rae_isWhitespace_rae_View_Char_*)__vp;
  *(rae_Bool*)__a->__task->result = rae_isWhitespace_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_isUpper_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_isUpper_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_isUpper_rae_View_Char_* __a = (__raespawn_args_rae_isUpper_rae_View_Char_*)__vp;
  *(rae_Bool*)__a->__task->result = rae_isUpper_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_isLower_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_isLower_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_isLower_rae_View_Char_* __a = (__raespawn_args_rae_isLower_rae_View_Char_*)__vp;
  *(rae_Bool*)__a->__task->result = rae_isLower_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_isLetter_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_isLetter_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_isLetter_rae_View_Char_* __a = (__raespawn_args_rae_isLetter_rae_View_Char_*)__vp;
  *(rae_Bool*)__a->__task->result = rae_isLetter_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_isAlnum_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_isAlnum_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_isAlnum_rae_View_Char_* __a = (__raespawn_args_rae_isAlnum_rae_View_Char_*)__vp;
  *(rae_Bool*)__a->__task->result = rae_isAlnum_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_toLower_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_toLower_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_toLower_rae_View_Char_* __a = (__raespawn_args_rae_toLower_rae_View_Char_*)__vp;
  *(uint32_t*)__a->__task->result = rae_toLower_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { uint32_t f0; RaeTask* __task; } __raespawn_args_rae_toUpper_rae_View_Char_;
RAE_UNUSED static void* __raespawn_thunk_rae_toUpper_rae_View_Char_(void* __vp) {
  __raespawn_args_rae_toUpper_rae_View_Char_* __a = (__raespawn_args_rae_toUpper_rae_View_Char_*)__vp;
  *(uint32_t*)__a->__task->result = rae_toUpper_rae_View_Char_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_jsonNull_;
RAE_UNUSED static void* __raespawn_thunk_rae_jsonNull_(void* __vp) {
  __raespawn_args_rae_jsonNull_* __a = (__raespawn_args_rae_jsonNull_*)__vp;
  *(rae_JsonValue*)__a->__task->result = rae_jsonNull_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { rae_Bool f0; RaeTask* __task; } __raespawn_args_rae_jsonBoolValue_rae_View_Bool_;
RAE_UNUSED static void* __raespawn_thunk_rae_jsonBoolValue_rae_View_Bool_(void* __vp) {
  __raespawn_args_rae_jsonBoolValue_rae_View_Bool_* __a = (__raespawn_args_rae_jsonBoolValue_rae_View_Bool_*)__vp;
  *(rae_JsonValue*)__a->__task->result = rae_jsonBoolValue_rae_View_Bool_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; RaeTask* __task; } __raespawn_args_rae_jsonNumberValue_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_jsonNumberValue_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_jsonNumberValue_rae_View_Float64_* __a = (__raespawn_args_rae_jsonNumberValue_rae_View_Float64_*)__vp;
  *(rae_JsonValue*)__a->__task->result = rae_jsonNumberValue_rae_View_Float64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { rae_String f0; RaeTask* __task; } __raespawn_args_rae_jsonStringValue_rae_String_;
RAE_UNUSED static void* __raespawn_thunk_rae_jsonStringValue_rae_String_(void* __vp) {
  __raespawn_args_rae_jsonStringValue_rae_String_* __a = (__raespawn_args_rae_jsonStringValue_rae_String_*)__vp;
  *(rae_JsonValue*)__a->__task->result = rae_jsonStringValue_rae_String_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_desktopDir_;
RAE_UNUSED static void* __raespawn_thunk_rae_desktopDir_(void* __vp) {
  __raespawn_args_rae_desktopDir_* __a = (__raespawn_args_rae_desktopDir_*)__vp;
  *(rae_String*)__a->__task->result = rae_desktopDir_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_picturesDir_;
RAE_UNUSED static void* __raespawn_thunk_rae_picturesDir_(void* __vp) {
  __raespawn_args_rae_picturesDir_* __a = (__raespawn_args_rae_picturesDir_*)__vp;
  *(rae_String*)__a->__task->result = rae_picturesDir_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_documentsDir_;
RAE_UNUSED static void* __raespawn_thunk_rae_documentsDir_(void* __vp) {
  __raespawn_args_rae_documentsDir_* __a = (__raespawn_args_rae_documentsDir_*)__vp;
  *(rae_String*)__a->__task->result = rae_documentsDir_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_homeDir_;
RAE_UNUSED static void* __raespawn_thunk_rae_homeDir_(void* __vp) {
  __raespawn_args_rae_homeDir_* __a = (__raespawn_args_rae_homeDir_*)__vp;
  *(rae_String*)__a->__task->result = rae_homeDir_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { int64_t f0; RaeTask* __task; } __raespawn_args_rae_pad4_rae_View_Int64_;
RAE_UNUSED static void* __raespawn_thunk_rae_pad4_rae_View_Int64_(void* __vp) {
  __raespawn_args_rae_pad4_rae_View_Int64_* __a = (__raespawn_args_rae_pad4_rae_View_Int64_*)__vp;
  *(rae_String*)__a->__task->result = rae_pad4_rae_View_Int64_(__a->f0);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; double f2; RaeTask* __task; } __raespawn_args_rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_*)__vp;
  *(rae_Vec3*)__a->__task->result = rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1, __a->f2);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_buildSceneOneBoxes_;
RAE_UNUSED static void* __raespawn_thunk_rae_buildSceneOneBoxes_(void* __vp) {
  __raespawn_args_rae_buildSceneOneBoxes_* __a = (__raespawn_args_rae_buildSceneOneBoxes_*)__vp;
  *(rae_List_rae_Box*)__a->__task->result = rae_buildSceneOneBoxes_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_buildSceneDiffuse_;
RAE_UNUSED static void* __raespawn_thunk_rae_buildSceneDiffuse_(void* __vp) {
  __raespawn_args_rae_buildSceneDiffuse_* __a = (__raespawn_args_rae_buildSceneDiffuse_*)__vp;
  *(rae_List_rae_Sphere*)__a->__task->result = rae_buildSceneDiffuse_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_buildSceneLights_;
RAE_UNUSED static void* __raespawn_thunk_rae_buildSceneLights_(void* __vp) {
  __raespawn_args_rae_buildSceneLights_* __a = (__raespawn_args_rae_buildSceneLights_*)__vp;
  *(rae_List_rae_Sphere*)__a->__task->result = rae_buildSceneLights_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_buildScene_;
RAE_UNUSED static void* __raespawn_thunk_rae_buildScene_(void* __vp) {
  __raespawn_args_rae_buildScene_* __a = (__raespawn_args_rae_buildScene_*)__vp;
  *(rae_List_rae_Sphere*)__a->__task->result = rae_buildScene_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_buildSceneOne_;
RAE_UNUSED static void* __raespawn_thunk_rae_buildSceneOne_(void* __vp) {
  __raespawn_args_rae_buildSceneOne_* __a = (__raespawn_args_rae_buildSceneOne_*)__vp;
  *(rae_List_rae_Sphere*)__a->__task->result = rae_buildSceneOne_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_buildSceneBook_;
RAE_UNUSED static void* __raespawn_thunk_rae_buildSceneBook_(void* __vp) {
  __raespawn_args_rae_buildSceneBook_* __a = (__raespawn_args_rae_buildSceneBook_*)__vp;
  *(rae_List_rae_Sphere*)__a->__task->result = rae_buildSceneBook_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; RaeTask* __task; } __raespawn_args_rae_fmin_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_fmin_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_fmin_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_fmin_rae_View_Float64_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_fmin_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { double f0; double f1; RaeTask* __task; } __raespawn_args_rae_fmax_rae_View_Float64_rae_View_Float64_;
RAE_UNUSED static void* __raespawn_thunk_rae_fmax_rae_View_Float64_rae_View_Float64_(void* __vp) {
  __raespawn_args_rae_fmax_rae_View_Float64_rae_View_Float64_* __a = (__raespawn_args_rae_fmax_rae_View_Float64_rae_View_Float64_*)__vp;
  *(double*)__a->__task->result = rae_fmax_rae_View_Float64_rae_View_Float64_(__a->f0, __a->f1);
  __a->__task->done = 1; free(__a); return ((void*)0);
}
typedef struct { RaeTask* __task; } __raespawn_args_rae_triBase_;
RAE_UNUSED static void* __raespawn_thunk_rae_triBase_(void* __vp) {
  __raespawn_args_rae_triBase_* __a = (__raespawn_args_rae_triBase_*)__vp;
  *(int64_t*)__a->__task->result = rae_triBase_();
  __a->__task->done = 1; free(__a); return ((void*)0);
}
RAE_UNUSED static void rae_log_RaeAny_(RaeAny value) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_log_any(rae_any((value))); rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_logS_RaeAny_(RaeAny value) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_log_stream_any(rae_any((value))); rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_toFloat_rae_View_Int64_(int64_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = rae_ext_rae_int_to_float(this);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_toInt_rae_View_Float64_(double this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_float_to_int(this);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_stringCopy_rae_View_String_(rae_View_String s) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_string_copy(s);
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_fromCStr_void_p_(const void* s) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_str_from_cstr(s);
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void* rae_toCStr_rae_View_String_(rae_View_String this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    void* __ret_val = rae_ext_rae_str_to_cstr((*this.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static uint32_t rae_at_rae_View_String_rae_View_Int64_(rae_View_String this, int64_t index) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    uint32_t __ret_val = rae_ext_rae_str_at((*this.ptr), index);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_length_rae_View_String_(rae_View_String this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_str_len((*this.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_compare_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String other) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_str_compare((*this.ptr), (*other.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_equals_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String other) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_str_eq((*this.ptr), (*other.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_hash_rae_View_String_(rae_View_String this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_str_hash((*this.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_concat_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String other) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_str_concat((*this.ptr), (*other.ptr));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_(rae_View_String this, int64_t start, int64_t len) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_str_sub((*this.ptr), start, len);
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_contains_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sub) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_str_contains((*this.ptr), (*sub.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_startsWith_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String prefix) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_str_starts_with((*this.ptr), (*prefix.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_endsWith_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String suffix) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_str_ends_with((*this.ptr), (*suffix.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_indexOf_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sub) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_str_index_of((*this.ptr), (*sub.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_lastIndexOf_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sub) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t needleLen = rae_length_rae_View_String_(sub);
  if ((bool)(needleLen == ((int64_t)0LL))) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  int64_t n = rae_length_rae_View_String_(this);
  if ((bool)(needleLen > n)) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  int64_t result = -(((int64_t)1LL));
  int64_t pos = ((int64_t)0LL);
  for (; (bool)(pos <= n - needleLen); ) {
  rae_String tail = rae_string_pool_take(rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_(this, pos, n - pos));
  int64_t found = (__extension__ ({ rae_String __rae_pw_0 = tail; rae_indexOf_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }, sub); }));
  if ((bool)(found == -(((int64_t)1LL)))) {
  {
    int64_t __ret_val = result;
  rae_string_drop(&tail);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  result = pos + found;
  pos = result + ((int64_t)1LL);
  rae_string_drop(&tail);
  }
  {
    int64_t __ret_val = result;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_toLower_rae_View_String_(rae_View_String this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_str_to_lower((*this.ptr));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_trim_rae_View_String_(rae_View_String this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_str_trim((*this.ptr));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_String rae_split_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String sep) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_String result = rae_createList_rae_String_rae_View_Int64_(((int64_t)4LL));
  if ((bool)(rae_length_rae_View_String_(sep) == ((int64_t)0LL))) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_String_rae_List_rae_String_rae_String_(&result, (*this.ptr)); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_String __ret_val = result;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_String remaining = rae_string_copy((*this.ptr));
  for (; (bool)true; ) {
  int64_t idx = (__extension__ ({ rae_String __rae_pw_0 = remaining; rae_indexOf_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }, sep); }));
  if ((bool)(idx == -(((int64_t)1LL)))) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_String_rae_List_rae_String_rae_String_(&result, remaining); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_String __ret_val = result;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_String part = rae_string_pool_take((__extension__ ({ rae_String __rae_pw_1 = remaining; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_1 }, ((int64_t)0LL), idx); })));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_String_rae_List_rae_String_rae_String_(&result, part); rae_string_pool_flush(__rae_spm); }
  { rae_String __asg2 = (__extension__ ({ rae_String __rae_pw_3 = remaining; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_3 }, idx + rae_length_rae_View_String_(sep), (__extension__ ({ rae_String __rae_pw_4 = remaining; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_4 }); })) - idx - rae_length_rae_View_String_(sep)); })); rae_string_drop(&remaining); remaining = rae_string_pool_take(__asg2); };
  }
  {
    rae_List_rae_String __ret_val = result;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_replace_rae_View_String_rae_View_String_rae_View_String_(rae_View_String this, rae_View_String old, rae_View_String new) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(rae_length_rae_View_String_(old) == ((int64_t)0LL))) {
  {
    rae_String __ret_val = rae_string_copy((*this.ptr));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  }
  rae_List_rae_String parts = rae_split_rae_View_String_rae_View_String_(this, old);
  {
    rae_String __ret_val = rae_join_rae_List_rae_String_rae_View_String_(&parts, new);
    __ret_val = rae_string_pool_take(__ret_val);
  rae_drop_rae_String_rae_List_rae_String_(&parts);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_drop_rae_String_rae_List_rae_String_(&parts);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_join_rae_List_rae_String_rae_View_String_(rae_List_rae_String* this, rae_View_String sep) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == ((int64_t)0LL))) {
  {
    rae_String __ret_val = (rae_String){(uint8_t*)"", 0};
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  }
  rae_String result = rae_string_pool_take(rae_get_rae_String_rae_List_rae_String_rae_View_Int64_(this, ((int64_t)0LL)).as.s);
  result.is_owned = 0;
  int64_t i = ((int64_t)1LL);
  for (; (bool)(i < this->length); ) {
  { rae_String __asg0 = (__extension__ ({ rae_String __rae_pw_1 = (__extension__ ({ rae_String __rae_pw_3 = result; rae_concat_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_3 }, sep); })); rae_String __rae_pw_2 = rae_get_rae_String_rae_List_rae_String_rae_View_Int64_(this, i).as.s; rae_concat_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_1 }, (rae_View_String){ .ptr = &__rae_pw_2 }); })); rae_string_drop(&result); result = rae_string_pool_take(__asg0); };
  i = i + ((int64_t)1LL);
  }
  {
    rae_String __ret_val = result;
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_toFloat_rae_View_String_(rae_View_String this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = rae_ext_rae_str_to_f64((*this.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_toInt_rae_View_String_(rae_View_String this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_str_to_i64((*this.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_seed_rae_View_Int64_(int64_t n) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_seed(n); rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_random_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = rae_ext_rae_random();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_random_rae_View_Int64_rae_View_Int64_(int64_t min, int64_t max) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_random_int(min, max);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_abs_rae_View_Int64_(int64_t n) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(n < ((int64_t)0LL))) {
  {
    int64_t __ret_val = -(n);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = n;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_abs_rae_View_Float64_(double n) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(n < 0.0)) {
  {
    double __ret_val = -(n);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    double __ret_val = n;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_min_rae_View_Int64_rae_View_Int64_(int64_t a, int64_t b) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(a < b)) {
  {
    int64_t __ret_val = a;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = b;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_min_rae_View_Float64_rae_View_Float64_(double a, double b) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(a < b)) {
  {
    double __ret_val = a;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    double __ret_val = b;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_max_rae_View_Int64_rae_View_Int64_(int64_t a, int64_t b) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(a > b)) {
  {
    int64_t __ret_val = a;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = b;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_max_rae_View_Float64_rae_View_Float64_(double a, double b) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(a > b)) {
  {
    double __ret_val = a;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    double __ret_val = b;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_clamp_rae_View_Int64_rae_View_Int64_rae_View_Int64_(int64_t val, int64_t low, int64_t high) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(val < low)) {
  {
    int64_t __ret_val = low;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(val > high)) {
  {
    int64_t __ret_val = high;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = val;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_clamp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(double val, double low, double high) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(val < low)) {
  {
    double __ret_val = low;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(val > high)) {
  {
    double __ret_val = high;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    double __ret_val = val;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_lerp_rae_View_Float64_rae_View_Float64_rae_View_Float64_(double a, double b, double t) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = a + (b - a) * t;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_randomFloat_rae_View_Float64_rae_View_Float64_(double min, double max) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = min + rae_ext_rae_random() * (max - min);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_randomInt_rae_View_Int64_rae_View_Int64_(int64_t min, int64_t max) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_random_int(min, max);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_readLine_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_io_read_line();
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static uint32_t rae_readChar_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    uint32_t __ret_val = rae_ext_rae_io_read_char();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_exit_rae_View_Int64_(int64_t code) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_sys_exit(code); rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_getEnv_rae_View_String_(rae_View_String name) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_sys_get_env((*name.ptr));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static RaeAny rae_readFile_rae_View_String_(rae_View_String path) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    RaeAny __ret_val = rae_any((rae_ext_rae_sys_read_file((*path.ptr))));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_writeFile_rae_View_String_rae_View_String_(rae_View_String path, rae_View_String content) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_sys_write_file((*path.ptr), (*content.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_rename_rae_View_String_rae_View_String_(rae_View_String oldPath, rae_View_String newPath) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_sys_rename((*oldPath.ptr), (*newPath.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_delete_rae_View_String_(rae_View_String path) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_sys_delete((*path.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_makeDir_rae_View_String_(rae_View_String path) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_sys_make_dir((*path.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_exists_rae_View_String_(rae_View_String path) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_sys_exists((*path.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_lockFile_rae_View_String_(rae_View_String path) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_sys_lock_file((*path.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_unlockFile_rae_View_String_(rae_View_String path) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = rae_ext_rae_sys_unlock_file((*path.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_fileModTime_rae_View_String_(rae_View_String path) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = rae_ext_rae_sys_file_mtime((*path.ptr));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_processRssKb_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = rae_ext_rae_sys_rss_kb();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_encrypt_RaeAny_RaeAny_RaeAny_rae_View_Int64_RaeAny_RaeAny_(RaeAny key, RaeAny nonce, RaeAny plain, int64_t len, RaeAny mac, RaeAny cipher) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_crypto_lock(rae_any((key)), rae_any((nonce)), rae_any((plain)), len, rae_any((mac)), rae_any((cipher))); rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_decrypt_RaeAny_RaeAny_RaeAny_RaeAny_rae_View_Int64_RaeAny_(RaeAny key, RaeAny nonce, RaeAny mac, RaeAny cipher, int64_t len, RaeAny plain) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Bool __ret_val = (bool)(rae_ext_rae_crypto_unlock(rae_any((key)), rae_any((nonce)), rae_any((mac)), rae_any((cipher)), len, rae_any((plain))) == ((int64_t)0LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List2Int rae_createList2Int_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List2Int __ret_val = (rae_List2Int){ .data = rae_ext___buf_alloc(cap), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_List2Int_(rae_List2Int* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  int64_t* newData = rae_ext___buf_alloc(newCap);
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(int64_t), (char*)(this->data) + (((int64_t)0LL)) * sizeof(int64_t), (this->length) * sizeof(int64_t)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext___buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_List2Int_rae_View_Int64_(rae_List2Int* this, int64_t value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_List2Int_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(int64_t*)( (char*)(this->data) + (this->length) * sizeof(int64_t) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_get_rae_List2Int_rae_View_Int64_(rae_List2Int* this, int64_t index) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = (*(int64_t*)( (char*)(this->data) + (index) * sizeof(int64_t) ));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_length_rae_List2Int_(rae_List2Int* this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = this->length;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List2 rae_createList2_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List2 __ret_val = (rae_List2){ .data = rae_ext___buf_alloc(cap), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_List2_(rae_List2* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  void* newData = rae_ext___buf_alloc(newCap);
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(RaeAny), (char*)(this->data) + (((int64_t)0LL)) * sizeof(RaeAny), (this->length) * sizeof(RaeAny)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext___buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_List2_RaeAny_(rae_List2* this, RaeAny value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_List2_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(RaeAny*)( (char*)(this->data) + (this->length) * sizeof(RaeAny) )) = rae_any((value)); rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static RaeAny rae_get_rae_List2_rae_View_Int64_(rae_List2* this, int64_t index) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    RaeAny __ret_val = rae_any(((*(RaeAny*)( (char*)(this->data) + (index) * sizeof(RaeAny) ))));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_length_rae_List2_(rae_List2* this) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = this->length;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_isDigit_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this == (uint32_t)48U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)49U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)50U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)51U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)52U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)53U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)54U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)55U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)56U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)57U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_isWhitespace_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this == (uint32_t)32U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)9U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)10U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)13U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_isUpper_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this == (uint32_t)65U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)66U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)67U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)68U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)69U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)70U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)71U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)72U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)73U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)74U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)75U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)76U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)77U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)78U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)79U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)80U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)81U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)82U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)83U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)84U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)85U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)86U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)87U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)88U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)89U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)90U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_isLower_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this == (uint32_t)97U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)98U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)99U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)100U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)101U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)102U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)103U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)104U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)105U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)106U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)107U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)108U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)109U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)110U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)111U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)112U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)113U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)114U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)115U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)116U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)117U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)118U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)119U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)120U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)121U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)122U)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_isLetter_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if (rae_isUpper_rae_View_Char_(this)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if (rae_isLower_rae_View_Char_(this)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_isAlnum_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if (rae_isLetter_rae_View_Char_(this)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if (rae_isDigit_rae_View_Char_(this)) {
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static uint32_t rae_toLower_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this == (uint32_t)65U)) {
  {
    uint32_t __ret_val = (uint32_t)97U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)66U)) {
  {
    uint32_t __ret_val = (uint32_t)98U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)67U)) {
  {
    uint32_t __ret_val = (uint32_t)99U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)68U)) {
  {
    uint32_t __ret_val = (uint32_t)100U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)69U)) {
  {
    uint32_t __ret_val = (uint32_t)101U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)70U)) {
  {
    uint32_t __ret_val = (uint32_t)102U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)71U)) {
  {
    uint32_t __ret_val = (uint32_t)103U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)72U)) {
  {
    uint32_t __ret_val = (uint32_t)104U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)73U)) {
  {
    uint32_t __ret_val = (uint32_t)105U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)74U)) {
  {
    uint32_t __ret_val = (uint32_t)106U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)75U)) {
  {
    uint32_t __ret_val = (uint32_t)107U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)76U)) {
  {
    uint32_t __ret_val = (uint32_t)108U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)77U)) {
  {
    uint32_t __ret_val = (uint32_t)109U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)78U)) {
  {
    uint32_t __ret_val = (uint32_t)110U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)79U)) {
  {
    uint32_t __ret_val = (uint32_t)111U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)80U)) {
  {
    uint32_t __ret_val = (uint32_t)112U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)81U)) {
  {
    uint32_t __ret_val = (uint32_t)113U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)82U)) {
  {
    uint32_t __ret_val = (uint32_t)114U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)83U)) {
  {
    uint32_t __ret_val = (uint32_t)115U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)84U)) {
  {
    uint32_t __ret_val = (uint32_t)116U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)85U)) {
  {
    uint32_t __ret_val = (uint32_t)117U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)86U)) {
  {
    uint32_t __ret_val = (uint32_t)118U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)87U)) {
  {
    uint32_t __ret_val = (uint32_t)119U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)88U)) {
  {
    uint32_t __ret_val = (uint32_t)120U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)89U)) {
  {
    uint32_t __ret_val = (uint32_t)121U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)90U)) {
  {
    uint32_t __ret_val = (uint32_t)122U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    uint32_t __ret_val = this;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static uint32_t rae_toUpper_rae_View_Char_(uint32_t this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this == (uint32_t)97U)) {
  {
    uint32_t __ret_val = (uint32_t)65U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)98U)) {
  {
    uint32_t __ret_val = (uint32_t)66U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)99U)) {
  {
    uint32_t __ret_val = (uint32_t)67U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)100U)) {
  {
    uint32_t __ret_val = (uint32_t)68U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)101U)) {
  {
    uint32_t __ret_val = (uint32_t)69U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)102U)) {
  {
    uint32_t __ret_val = (uint32_t)70U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)103U)) {
  {
    uint32_t __ret_val = (uint32_t)71U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)104U)) {
  {
    uint32_t __ret_val = (uint32_t)72U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)105U)) {
  {
    uint32_t __ret_val = (uint32_t)73U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)106U)) {
  {
    uint32_t __ret_val = (uint32_t)74U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)107U)) {
  {
    uint32_t __ret_val = (uint32_t)75U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)108U)) {
  {
    uint32_t __ret_val = (uint32_t)76U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)109U)) {
  {
    uint32_t __ret_val = (uint32_t)77U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)110U)) {
  {
    uint32_t __ret_val = (uint32_t)78U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)111U)) {
  {
    uint32_t __ret_val = (uint32_t)79U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)112U)) {
  {
    uint32_t __ret_val = (uint32_t)80U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)113U)) {
  {
    uint32_t __ret_val = (uint32_t)81U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)114U)) {
  {
    uint32_t __ret_val = (uint32_t)82U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)115U)) {
  {
    uint32_t __ret_val = (uint32_t)83U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)116U)) {
  {
    uint32_t __ret_val = (uint32_t)84U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)117U)) {
  {
    uint32_t __ret_val = (uint32_t)85U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)118U)) {
  {
    uint32_t __ret_val = (uint32_t)86U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)119U)) {
  {
    uint32_t __ret_val = (uint32_t)87U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)120U)) {
  {
    uint32_t __ret_val = (uint32_t)88U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)121U)) {
  {
    uint32_t __ret_val = (uint32_t)89U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(this == (uint32_t)122U)) {
  {
    uint32_t __ret_val = (uint32_t)90U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    uint32_t __ret_val = this;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonNull_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_JsonValue __ret_val = (rae_JsonValue){ .kind = JsonKind_null, .asBool = (bool)false, .asNumber = 0.0, .asString = rae_string_copy((rae_String){(uint8_t*)"", 0}), .rangeStart = ((int64_t)0LL), .rangeLen = ((int64_t)0LL) };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonBoolValue_rae_View_Bool_(rae_Bool v) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_JsonValue __ret_val = (rae_JsonValue){ .kind = JsonKind_bool, .asBool = v, .asNumber = 0.0, .asString = rae_string_copy((rae_String){(uint8_t*)"", 0}), .rangeStart = ((int64_t)0LL), .rangeLen = ((int64_t)0LL) };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonNumberValue_rae_View_Float64_(double v) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_JsonValue __ret_val = (rae_JsonValue){ .kind = JsonKind_number, .asBool = (bool)false, .asNumber = v, .asString = rae_string_copy((rae_String){(uint8_t*)"", 0}), .rangeStart = ((int64_t)0LL), .rangeLen = ((int64_t)0LL) };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonStringValue_rae_String_(rae_String v) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_JsonValue __ret_val = (rae_JsonValue){ .kind = JsonKind_string, .asBool = (bool)false, .asNumber = 0.0, .asString = rae_string_move_or_copy(&(v)), .rangeStart = ((int64_t)0LL), .rangeLen = ((int64_t)0LL) };
  rae_string_drop(&v);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_drop(&v);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_valueAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_JsonValue v = (*(rae_JsonValue*)( (char*)(doc->values.data) + (idx) * sizeof(rae_JsonValue) ));
  {
    rae_JsonValue __ret_val = v;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_childAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t v = (*(int64_t*)( (char*)(doc->children.data) + (idx) * sizeof(int64_t) ));
  {
    int64_t __ret_val = v;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonField rae_fieldAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_JsonField v = (*(rae_JsonField*)( (char*)(doc->fields.data) + (idx) * sizeof(rae_JsonField) ));
  {
    rae_JsonField __ret_val = v;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonRoot_rae_JsonDoc_(rae_JsonDoc* doc) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(doc->ok == (bool)false)) {
  {
    rae_JsonValue __ret_val = rae_jsonNull_();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_JsonValue __ret_val = rae_valueAt_rae_JsonDoc_rae_View_Int64_(doc, doc->rootIdx);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_jsonInt_rae_JsonValue_rae_View_Int64_(rae_JsonValue* this, int64_t fallback) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind == JsonKind_number)) {
  {
    int64_t __ret_val = rae_toInt_rae_View_Float64_(this->asNumber);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = fallback;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_jsonFloat_rae_JsonValue_rae_View_Float64_(rae_JsonValue* this, double fallback) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind == JsonKind_number)) {
  {
    double __ret_val = this->asNumber;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    double __ret_val = fallback;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_jsonString_rae_JsonValue_rae_View_String_(rae_JsonValue* this, rae_View_String fallback) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind == JsonKind_string)) {
  {
    rae_String __ret_val = (__extension__ ({ rae_String __rae_pw_0 = this->asString; rae_ext_rae_string_copy((rae_View_String){ .ptr = &__rae_pw_0 }); }));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  }
  {
    rae_String __ret_val = rae_ext_rae_string_copy(fallback);
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_jsonBool_rae_JsonValue_rae_View_Bool_(rae_JsonValue* this, rae_Bool fallback) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind == JsonKind_bool)) {
  {
    rae_Bool __ret_val = this->asBool;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = fallback;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_jsonArrayLen_rae_JsonValue_(rae_JsonValue* this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind != JsonKind_array)) {
  {
    int64_t __ret_val = ((int64_t)0LL);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = this->rangeLen;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonArrayAt_rae_JsonDoc_rae_JsonValue_rae_View_Int64_(rae_JsonDoc* doc, rae_JsonValue* this, int64_t idx) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind != JsonKind_array)) {
  {
    rae_JsonValue __ret_val = rae_jsonNull_();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)((bool)(idx < ((int64_t)0LL)) || (bool)(idx >= this->rangeLen))) {
  {
    rae_JsonValue __ret_val = rae_jsonNull_();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  int64_t childIdx = rae_childAt_rae_JsonDoc_rae_View_Int64_(doc, this->rangeStart + idx);
  {
    rae_JsonValue __ret_val = rae_valueAt_rae_JsonDoc_rae_View_Int64_(doc, childIdx);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_jsonObjectLen_rae_JsonValue_(rae_JsonValue* this) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind != JsonKind_object)) {
  {
    int64_t __ret_val = ((int64_t)0LL);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = this->rangeLen;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_jsonObjectKeyAt_rae_JsonDoc_rae_JsonValue_rae_View_Int64_(rae_JsonDoc* doc, rae_JsonValue* this, int64_t idx) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind != JsonKind_object)) {
  {
    rae_String __ret_val = (rae_String){(uint8_t*)"", 0};
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  }
  if ((bool)((bool)(idx < ((int64_t)0LL)) || (bool)(idx >= this->rangeLen))) {
  {
    rae_String __ret_val = (rae_String){(uint8_t*)"", 0};
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  }
  rae_JsonField f = rae_fieldAt_rae_JsonDoc_rae_View_Int64_(doc, this->rangeStart + idx);
  {
    rae_String __ret_val = rae_string_copy(f.key);
    __ret_val = rae_string_pool_take(__ret_val);
  rae_drop_struct_rae_JsonField_alias(&f);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_drop_struct_rae_JsonField_alias(&f);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonObjectValueAt_rae_JsonDoc_rae_JsonValue_rae_View_Int64_(rae_JsonDoc* doc, rae_JsonValue* this, int64_t idx) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind != JsonKind_object)) {
  {
    rae_JsonValue __ret_val = rae_jsonNull_();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)((bool)(idx < ((int64_t)0LL)) || (bool)(idx >= this->rangeLen))) {
  {
    rae_JsonValue __ret_val = rae_jsonNull_();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_JsonField f = rae_fieldAt_rae_JsonDoc_rae_View_Int64_(doc, this->rangeStart + idx);
  {
    rae_JsonValue __ret_val = rae_valueAt_rae_JsonDoc_rae_View_Int64_(doc, f.valueIdx);
  rae_drop_struct_rae_JsonField_alias(&f);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_struct_rae_JsonField_alias(&f);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(rae_JsonDoc* doc, rae_JsonValue* this, rae_View_String key) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->kind != JsonKind_object)) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < this->rangeLen); ) {
  rae_JsonField f = rae_fieldAt_rae_JsonDoc_rae_View_Int64_(doc, this->rangeStart + i);
  if ((__extension__ ({ rae_String __rae_pw_0 = f.key; rae_equals_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }, key); }))) {
  {
    int64_t __ret_val = f.valueIdx;
  rae_drop_struct_rae_JsonField_alias(&f);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  i = i + ((int64_t)1LL);
  rae_drop_struct_rae_JsonField_alias(&f);
  }
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonValue rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(rae_JsonDoc* doc, int64_t idx) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)((bool)(idx < ((int64_t)0LL)) || (bool)(idx >= doc->values.length))) {
  {
    rae_JsonValue __ret_val = rae_jsonNull_();
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_JsonValue __ret_val = rae_valueAt_rae_JsonDoc_rae_View_Int64_(doc, idx);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_skipWhitespace_rae_JsonParser_(rae_JsonParser* p) {
  int __rae_spm_func = rae_string_pool_mark();
  for (; (bool)(p->pos < (__extension__ ({ rae_String __rae_pw_0 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); }))); ) {
  uint32_t c = (__extension__ ({ rae_String __rae_pw_1 = p->source; rae_at_rae_View_String_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_1 }, p->pos); }));
  if (rae_isWhitespace_rae_View_Char_(c)) {
  p->pos = p->pos + ((int64_t)1LL);
  } else {
  {
    rae_string_pool_flush(__rae_spm_func);
    return;
  }
  }
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static uint32_t rae_peek_rae_JsonParser_(rae_JsonParser* p) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(p->pos >= (__extension__ ({ rae_String __rae_pw_0 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); })))) {
  {
    uint32_t __ret_val = (uint32_t)0U;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    uint32_t __ret_val = (__extension__ ({ rae_String __rae_pw_1 = p->source; rae_at_rae_View_String_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_1 }, p->pos); }));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_consume_rae_JsonParser_rae_View_Char_(rae_JsonParser* p, uint32_t c) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(rae_peek_rae_JsonParser_(p) == c)) {
  p->pos = p->pos + ((int64_t)1LL);
  {
    rae_Bool __ret_val = (bool)true;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_consumeKeyword_rae_JsonParser_rae_View_String_(rae_JsonParser* p, rae_View_String kw) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t len = rae_length_rae_View_String_(kw);
  if ((bool)(p->pos + len > (__extension__ ({ rae_String __rae_pw_0 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); })))) {
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_String slice = rae_string_pool_take((__extension__ ({ rae_String __rae_pw_1 = p->source; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_1 }, p->pos, len); })));
  if ((__extension__ ({ rae_String __rae_pw_2 = slice; rae_equals_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_2 }, kw); }))) {
  p->pos = p->pos + len;
  {
    rae_Bool __ret_val = (bool)true;
  rae_string_drop(&slice);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    rae_Bool __ret_val = (bool)false;
  rae_string_drop(&slice);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_drop(&slice);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(rae_List_rae_JsonValue* vals, rae_JsonValue v) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t idx = vals->length;
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_JsonValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, v); rae_string_pool_flush(__rae_spm); }
  {
    int64_t __ret_val = idx;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_pushChild_rae_List_int64_t_rae_View_Int64_(rae_List_int64_t* kids, int64_t valueIdx) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t idx = kids->length;
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(kids, valueIdx); rae_string_pool_flush(__rae_spm); }
  {
    int64_t __ret_val = idx;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_pushField_rae_List_rae_JsonField_rae_String_rae_View_Int64_(rae_List_rae_JsonField* fields, rae_String key, int64_t valueIdx) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t idx = fields->length;
  rae_JsonField f = (rae_JsonField){ .key = rae_string_move_or_copy(&(key)), .valueIdx = valueIdx };
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_JsonField_rae_List_rae_JsonField_rae_JsonField_(fields, f); rae_string_pool_flush(__rae_spm); }
  {
    int64_t __ret_val = idx;
  rae_string_drop(&key);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_drop(&key);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_parseString_rae_JsonParser_(rae_JsonParser* p) {
  int __rae_spm_func = rae_string_pool_mark();
  if (((bool)!(rae_consume_rae_JsonParser_rae_View_Char_(p, (uint32_t)34U)))) {
  p->ok = (bool)false;
  {
    rae_String __ret_val = (rae_String){(uint8_t*)"", 0};
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  }
  int64_t start = p->pos;
  rae_String result = rae_string_pool_take((rae_String){(uint8_t*)"", 0});
  result.is_owned = 0;
  int64_t chunkStart = p->pos;
  for (; (bool)(p->pos < (__extension__ ({ rae_String __rae_pw_0 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); }))); ) {
  uint32_t c = (__extension__ ({ rae_String __rae_pw_1 = p->source; rae_at_rae_View_String_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_1 }, p->pos); }));
  if ((bool)(c == (uint32_t)34U)) {
  rae_String chunk = rae_string_pool_take((__extension__ ({ rae_String __rae_pw_2 = p->source; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_2 }, chunkStart, p->pos - chunkStart); })));
  { rae_String __asg3 = (__extension__ ({ rae_String __rae_pw_4 = result; rae_String __rae_pw_5 = chunk; rae_concat_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_4 }, (rae_View_String){ .ptr = &__rae_pw_5 }); })); rae_string_drop(&result); result = rae_string_pool_take(__asg3); };
  p->pos = p->pos + ((int64_t)1LL);
  {
    rae_String __ret_val = result;
    __ret_val = rae_string_pool_take(__ret_val);
  rae_string_drop(&chunk);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_drop(&chunk);
  }
  if ((bool)(c == (uint32_t)92U)) {
  rae_String chunk = rae_string_pool_take((__extension__ ({ rae_String __rae_pw_6 = p->source; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_6 }, chunkStart, p->pos - chunkStart); })));
  { rae_String __asg7 = (__extension__ ({ rae_String __rae_pw_8 = result; rae_String __rae_pw_9 = chunk; rae_concat_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_8 }, (rae_View_String){ .ptr = &__rae_pw_9 }); })); rae_string_drop(&result); result = rae_string_pool_take(__asg7); };
  p->pos = p->pos + ((int64_t)1LL);
  if ((bool)(p->pos >= (__extension__ ({ rae_String __rae_pw_10 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_10 }); })))) {
  p->ok = (bool)false;
  {
    rae_String __ret_val = result;
    __ret_val = rae_string_pool_take(__ret_val);
  rae_string_drop(&chunk);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  }
  uint32_t esc = (__extension__ ({ rae_String __rae_pw_11 = p->source; rae_at_rae_View_String_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_11 }, p->pos); }));
  rae_String one = rae_string_pool_take((rae_String){(uint8_t*)"", 0});
  one.is_owned = 0;
  if ((bool)(esc == (uint32_t)34U)) {
  { rae_String __asg12 = (rae_String){(uint8_t*)"\"", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg12); };
  } else {
  if ((bool)(esc == (uint32_t)92U)) {
  { rae_String __asg13 = (rae_String){(uint8_t*)"\\", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg13); };
  } else {
  if ((bool)(esc == (uint32_t)47U)) {
  { rae_String __asg14 = (rae_String){(uint8_t*)"/", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg14); };
  } else {
  if ((bool)(esc == (uint32_t)110U)) {
  { rae_String __asg15 = (rae_String){(uint8_t*)"\n", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg15); };
  } else {
  if ((bool)(esc == (uint32_t)116U)) {
  { rae_String __asg16 = (rae_String){(uint8_t*)"\t", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg16); };
  } else {
  if ((bool)(esc == (uint32_t)114U)) {
  { rae_String __asg17 = (rae_String){(uint8_t*)"\r", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg17); };
  } else {
  if ((bool)(esc == (uint32_t)98U)) {
  { rae_String __asg18 = (rae_String){(uint8_t*)"\x08", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg18); };
  } else {
  if ((bool)(esc == (uint32_t)102U)) {
  { rae_String __asg19 = (rae_String){(uint8_t*)"\x0c", 1}; rae_string_drop(&one); one = rae_string_pool_take(__asg19); };
  } else {
  { rae_String __asg20 = (__extension__ ({ rae_String __rae_pw_21 = p->source; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_21 }, p->pos, ((int64_t)1LL)); })); rae_string_drop(&one); one = rae_string_pool_take(__asg20); };
  }
  }
  }
  }
  }
  }
  }
  }
  { rae_String __asg22 = (__extension__ ({ rae_String __rae_pw_23 = result; rae_String __rae_pw_24 = one; rae_concat_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_23 }, (rae_View_String){ .ptr = &__rae_pw_24 }); })); rae_string_drop(&result); result = rae_string_pool_take(__asg22); };
  p->pos = p->pos + ((int64_t)1LL);
  chunkStart = p->pos;
  rae_string_drop(&chunk);
  } else {
  p->pos = p->pos + ((int64_t)1LL);
  }
  }
  p->ok = (bool)false;
  {
    rae_String __ret_val = result;
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_parseNumber_rae_JsonParser_(rae_JsonParser* p) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t start = p->pos;
  if ((bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)45U)) {
  p->pos = p->pos + ((int64_t)1LL);
  }
  for (; (bool)((bool)(p->pos < (__extension__ ({ rae_String __rae_pw_0 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); }))) && rae_isDigit_rae_View_Char_(rae_peek_rae_JsonParser_(p))); ) {
  p->pos = p->pos + ((int64_t)1LL);
  }
  if ((bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)46U)) {
  p->pos = p->pos + ((int64_t)1LL);
  for (; (bool)((bool)(p->pos < (__extension__ ({ rae_String __rae_pw_1 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_1 }); }))) && rae_isDigit_rae_View_Char_(rae_peek_rae_JsonParser_(p))); ) {
  p->pos = p->pos + ((int64_t)1LL);
  }
  }
  if ((bool)((bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)101U) || (bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)69U))) {
  p->pos = p->pos + ((int64_t)1LL);
  if ((bool)((bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)43U) || (bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)45U))) {
  p->pos = p->pos + ((int64_t)1LL);
  }
  for (; (bool)((bool)(p->pos < (__extension__ ({ rae_String __rae_pw_2 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_2 }); }))) && rae_isDigit_rae_View_Char_(rae_peek_rae_JsonParser_(p))); ) {
  p->pos = p->pos + ((int64_t)1LL);
  }
  }
  if ((bool)(p->pos == start)) {
  p->ok = (bool)false;
  {
    double __ret_val = 0.0;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_String lit = rae_string_pool_take((__extension__ ({ rae_String __rae_pw_3 = p->source; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_3 }, start, p->pos - start); })));
  {
    double __ret_val = (__extension__ ({ rae_String __rae_pw_4 = lit; rae_toFloat_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_4 }); }));
  rae_string_drop(&lit);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_drop(&lit);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_parseValue_rae_JsonParser_rae_List_rae_JsonValue_rae_List_int64_t_rae_List_rae_JsonField_(rae_JsonParser* p, rae_List_rae_JsonValue* vals, rae_List_int64_t* kids, rae_List_rae_JsonField* fields) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  if ((bool)(p->pos >= (__extension__ ({ rae_String __rae_pw_0 = p->source; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); })))) {
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  uint32_t c = rae_peek_rae_JsonParser_(p);
  if ((bool)(c == (uint32_t)123U)) {
  p->pos = p->pos + ((int64_t)1LL);
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  rae_List_rae_JsonField localFields = rae_createList_rae_JsonField_rae_View_Int64_(((int64_t)4LL));
  if ((bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)125U)) {
  p->pos = p->pos + ((int64_t)1LL);
  } else {
  for (; (bool)true; ) {
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  rae_String key = rae_string_pool_take(rae_parseString_rae_JsonParser_(p));
  if ((bool)(p->ok == (bool)false)) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_string_drop(&key);
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&localFields);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  if (((bool)!(rae_consume_rae_JsonParser_rae_View_Char_(p, (uint32_t)58U)))) {
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_string_drop(&key);
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&localFields);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  int64_t valIdx = rae_parseValue_rae_JsonParser_rae_List_rae_JsonValue_rae_List_int64_t_rae_List_rae_JsonField_(p, vals, kids, fields);
  if ((bool)(p->ok == (bool)false)) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_string_drop(&key);
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&localFields);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_JsonField f = (rae_JsonField){ .key = rae_string_copy(key), .valueIdx = valIdx };
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_JsonField_rae_List_rae_JsonField_rae_JsonField_(&localFields, f); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  if (rae_consume_rae_JsonParser_rae_View_Char_(p, (uint32_t)44U)) {
  } else {
  if (rae_consume_rae_JsonParser_rae_View_Char_(p, (uint32_t)125U)) {
  p->pos = p->pos;
  int64_t firstField = fields->length;
  int64_t count = localFields.length;
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < count); ) {
  rae_JsonField f2 = (*(rae_JsonField*)( (char*)(localFields.data) + (i) * sizeof(rae_JsonField) ));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_JsonField_rae_List_rae_JsonField_rae_JsonField_(fields, f2); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  }
  localFields.length = ((int64_t)0LL);
  rae_JsonValue v = (rae_JsonValue){ .kind = JsonKind_object, .asBool = (bool)false, .asNumber = 0.0, .asString = rae_string_copy((rae_String){(uint8_t*)"", 0}), .rangeStart = firstField, .rangeLen = count };
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, v);
  rae_string_drop(&key);
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&localFields);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  } else {
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_string_drop(&key);
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&localFields);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  }
  rae_string_drop(&key);
  }
  }
  int64_t firstField = fields->length;
  rae_JsonValue v = (rae_JsonValue){ .kind = JsonKind_object, .asBool = (bool)false, .asNumber = 0.0, .asString = rae_string_copy((rae_String){(uint8_t*)"", 0}), .rangeStart = firstField, .rangeLen = ((int64_t)0LL) };
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, v);
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&localFields);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_rae_JsonField_rae_List_rae_JsonField_(&localFields);
  }
  if ((bool)(c == (uint32_t)91U)) {
  p->pos = p->pos + ((int64_t)1LL);
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  rae_List_int64_t localKids = rae_createList_int64_t_rae_View_Int64_(((int64_t)4LL));
  if ((bool)(rae_peek_rae_JsonParser_(p) == (uint32_t)93U)) {
  p->pos = p->pos + ((int64_t)1LL);
  } else {
  for (; (bool)true; ) {
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  int64_t elemIdx = rae_parseValue_rae_JsonParser_rae_List_rae_JsonValue_rae_List_int64_t_rae_List_rae_JsonField_(p, vals, kids, fields);
  if ((bool)(p->ok == (bool)false)) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_drop_int64_t_rae_List_int64_t_(&localKids);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&localKids, elemIdx); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(p); rae_string_pool_flush(__rae_spm); }
  if (rae_consume_rae_JsonParser_rae_View_Char_(p, (uint32_t)44U)) {
  } else {
  if (rae_consume_rae_JsonParser_rae_View_Char_(p, (uint32_t)93U)) {
  int64_t firstChild = kids->length;
  int64_t count = localKids.length;
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < count); ) {
  int64_t kidIdx = (*(int64_t*)( (char*)(localKids.data) + (i) * sizeof(int64_t) ));
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(kids, kidIdx); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  }
  rae_JsonValue v = (rae_JsonValue){ .kind = JsonKind_array, .asBool = (bool)false, .asNumber = 0.0, .asString = rae_string_copy((rae_String){(uint8_t*)"", 0}), .rangeStart = firstChild, .rangeLen = count };
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, v);
  rae_drop_int64_t_rae_List_int64_t_(&localKids);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  } else {
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_drop_int64_t_rae_List_int64_t_(&localKids);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  }
  }
  }
  int64_t firstChild = kids->length;
  rae_JsonValue v = (rae_JsonValue){ .kind = JsonKind_array, .asBool = (bool)false, .asNumber = 0.0, .asString = rae_string_copy((rae_String){(uint8_t*)"", 0}), .rangeStart = firstChild, .rangeLen = ((int64_t)0LL) };
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, v);
  rae_drop_int64_t_rae_List_int64_t_(&localKids);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_int64_t_rae_List_int64_t_(&localKids);
  }
  if ((bool)(c == (uint32_t)34U)) {
  rae_String s = rae_string_pool_take(rae_parseString_rae_JsonParser_(p));
  if ((bool)(p->ok == (bool)false)) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_string_drop(&s);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, rae_jsonStringValue_rae_String_(s));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(c == (uint32_t)116U)) {
  if ((__extension__ ({ rae_String __rae_pw_1 = (rae_String){(uint8_t*)"true", 4}; rae_consumeKeyword_rae_JsonParser_rae_View_String_(p, (rae_View_String){ .ptr = &__rae_pw_1 }); }))) {
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, rae_jsonBoolValue_rae_View_Bool_((bool)true));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(c == (uint32_t)102U)) {
  if ((__extension__ ({ rae_String __rae_pw_2 = (rae_String){(uint8_t*)"false", 5}; rae_consumeKeyword_rae_JsonParser_rae_View_String_(p, (rae_View_String){ .ptr = &__rae_pw_2 }); }))) {
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, rae_jsonBoolValue_rae_View_Bool_((bool)false));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(c == (uint32_t)110U)) {
  if ((__extension__ ({ rae_String __rae_pw_3 = (rae_String){(uint8_t*)"null", 4}; rae_consumeKeyword_rae_JsonParser_rae_View_String_(p, (rae_View_String){ .ptr = &__rae_pw_3 }); }))) {
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, rae_jsonNull_());
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)((bool)(c == (uint32_t)45U) || rae_isDigit_rae_View_Char_(c))) {
  double n = rae_parseNumber_rae_JsonParser_(p);
  if ((bool)(p->ok == (bool)false)) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = rae_pushValue_rae_List_rae_JsonValue_rae_JsonValue_(vals, rae_jsonNumberValue_rae_View_Float64_(n));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  p->ok = (bool)false;
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_JsonDoc rae_parseJson_rae_View_String_(rae_View_String source) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_JsonDoc doc = (rae_JsonDoc){ .values = rae_createList_rae_JsonValue_rae_View_Int64_(((int64_t)8LL)), .children = rae_createList_int64_t_rae_View_Int64_(((int64_t)8LL)), .fields = rae_createList_rae_JsonField_rae_View_Int64_(((int64_t)8LL)), .rootIdx = -(((int64_t)1LL)), .ok = (bool)false, .errorPos = ((int64_t)0LL) };
  rae_JsonParser p = (rae_JsonParser){ .source = rae_string_copy((*source.ptr)), .pos = ((int64_t)0LL), .ok = (bool)true };
  int64_t rootIdx = rae_parseValue_rae_JsonParser_rae_List_rae_JsonValue_rae_List_int64_t_rae_List_rae_JsonField_(&p, &doc.values, &doc.children, &doc.fields);
  { int __rae_spm = rae_string_pool_mark(); rae_skipWhitespace_rae_JsonParser_(&p); rae_string_pool_flush(__rae_spm); }
  if ((bool)(p.ok == (bool)false)) {
  doc.ok = (bool)false;
  doc.errorPos = p.pos;
  {
    rae_JsonDoc __ret_val = doc;
  rae_drop_struct_rae_JsonParser(&p);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  if ((bool)(p.pos < rae_length_rae_View_String_(source))) {
  doc.ok = (bool)false;
  doc.errorPos = p.pos;
  {
    rae_JsonDoc __ret_val = doc;
  rae_drop_struct_rae_JsonParser(&p);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  doc.rootIdx = rootIdx;
  doc.ok = (bool)true;
  {
    rae_JsonDoc __ret_val = doc;
  rae_drop_struct_rae_JsonParser(&p);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_struct_rae_JsonParser(&p);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(rae_JsonDoc* doc, rae_JsonValue* obj, rae_View_String key, double fallback) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t idx = rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(doc, obj, key);
  if ((bool)(idx < ((int64_t)0LL))) {
  {
    double __ret_val = fallback;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_JsonValue v = rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(doc, idx);
  {
    double __ret_val = rae_jsonFloat_rae_JsonValue_rae_View_Float64_(&v, fallback);
  rae_drop_struct_rae_JsonValue_alias(&v);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_struct_rae_JsonValue_alias(&v);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Bool rae_sdfReadBounds_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_(rae_JsonDoc* doc, rae_JsonValue* parent, rae_View_String subKey, rae_Mod_Float64 outLeft, rae_Mod_Float64 outBottom, rae_Mod_Float64 outRight, rae_Mod_Float64 outTop) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t subIdx = rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(doc, parent, subKey);
  if ((bool)(subIdx < ((int64_t)0LL))) {
  {
    rae_Bool __ret_val = (bool)false;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_JsonValue sub = rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(doc, subIdx);
  *outLeft.ptr = (__extension__ ({ rae_String __rae_pw_0 = (rae_String){(uint8_t*)"left", 4}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(doc, &sub, (rae_View_String){ .ptr = &__rae_pw_0 }, 0.0); }));
  *outBottom.ptr = (__extension__ ({ rae_String __rae_pw_1 = (rae_String){(uint8_t*)"bottom", 6}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(doc, &sub, (rae_View_String){ .ptr = &__rae_pw_1 }, 0.0); }));
  *outRight.ptr = (__extension__ ({ rae_String __rae_pw_2 = (rae_String){(uint8_t*)"right", 5}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(doc, &sub, (rae_View_String){ .ptr = &__rae_pw_2 }, 0.0); }));
  *outTop.ptr = (__extension__ ({ rae_String __rae_pw_3 = (rae_String){(uint8_t*)"top", 3}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(doc, &sub, (rae_View_String){ .ptr = &__rae_pw_3 }, 0.0); }));
  {
    rae_Bool __ret_val = (bool)true;
  rae_drop_struct_rae_JsonValue_alias(&sub);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_struct_rae_JsonValue_alias(&sub);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_SdfFont rae_loadSdfFont_rae_View_String_rae_View_String_(rae_View_String jsonPath, rae_View_String rawAtlasPath) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_String source = rae_string_pool_take(rae_ext_rae_sys_read_file((*jsonPath.ptr)));
  rae_JsonDoc doc = (__extension__ ({ rae_String __rae_pw_0 = source; rae_parseJson_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); }));
  rae_JsonValue root = rae_jsonRoot_rae_JsonDoc_(&doc);
  int64_t atlasIdx = (__extension__ ({ rae_String __rae_pw_1 = (rae_String){(uint8_t*)"atlas", 5}; rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(&doc, &root, (rae_View_String){ .ptr = &__rae_pw_1 }); }));
  rae_JsonValue atlasJ = rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(&doc, atlasIdx);
  double atlasW = (__extension__ ({ rae_String __rae_pw_2 = (rae_String){(uint8_t*)"width", 5}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(&doc, &atlasJ, (rae_View_String){ .ptr = &__rae_pw_2 }, 0.0); }));
  double atlasH = (__extension__ ({ rae_String __rae_pw_3 = (rae_String){(uint8_t*)"height", 6}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(&doc, &atlasJ, (rae_View_String){ .ptr = &__rae_pw_3 }, 0.0); }));
  double pxRange = (__extension__ ({ rae_String __rae_pw_4 = (rae_String){(uint8_t*)"distanceRange", 13}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(&doc, &atlasJ, (rae_View_String){ .ptr = &__rae_pw_4 }, 4.0); }));
  int64_t metricsIdx = (__extension__ ({ rae_String __rae_pw_5 = (rae_String){(uint8_t*)"metrics", 7}; rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(&doc, &root, (rae_View_String){ .ptr = &__rae_pw_5 }); }));
  rae_JsonValue metricsJ = rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(&doc, metricsIdx);
  double emSize = (__extension__ ({ rae_String __rae_pw_6 = (rae_String){(uint8_t*)"emSize", 6}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(&doc, &metricsJ, (rae_View_String){ .ptr = &__rae_pw_6 }, 1.0); }));
  double lineHeight = (__extension__ ({ rae_String __rae_pw_7 = (rae_String){(uint8_t*)"lineHeight", 10}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(&doc, &metricsJ, (rae_View_String){ .ptr = &__rae_pw_7 }, 1.2); }));
  double ascender = (__extension__ ({ rae_String __rae_pw_8 = (rae_String){(uint8_t*)"ascender", 8}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(&doc, &metricsJ, (rae_View_String){ .ptr = &__rae_pw_8 }, 0.9); }));
  int64_t glyphsIdx = (__extension__ ({ rae_String __rae_pw_9 = (rae_String){(uint8_t*)"glyphs", 6}; rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(&doc, &root, (rae_View_String){ .ptr = &__rae_pw_9 }); }));
  rae_JsonValue glyphsArr = rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(&doc, glyphsIdx);
  int64_t n = rae_jsonArrayLen_rae_JsonValue_(&glyphsArr);
  rae_List_rae_SdfGlyph glyphs = rae_createList_rae_SdfGlyph_rae_View_Int64_(n);
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < n); ) {
  rae_JsonValue entry = rae_jsonArrayAt_rae_JsonDoc_rae_JsonValue_rae_View_Int64_(&doc, &glyphsArr, i);
  int64_t unicodeIdx = (__extension__ ({ rae_String __rae_pw_10 = (rae_String){(uint8_t*)"unicode", 7}; rae_jsonField_rae_JsonDoc_rae_JsonValue_rae_View_String_(&doc, &entry, (rae_View_String){ .ptr = &__rae_pw_10 }); }));
  rae_JsonValue unicodeV = rae_jsonValueAt_rae_JsonDoc_rae_View_Int64_(&doc, unicodeIdx);
  int64_t cp = rae_jsonInt_rae_JsonValue_rae_View_Int64_(&unicodeV, ((int64_t)0LL));
  double advance = (__extension__ ({ rae_String __rae_pw_11 = (rae_String){(uint8_t*)"advance", 7}; rae_sdfFloatField_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_View_Float64_(&doc, &entry, (rae_View_String){ .ptr = &__rae_pw_11 }, 0.0); }));
  double pl = 0.0;
  double pb = 0.0;
  double pr = 0.0;
  double pt = 0.0;
  double al = 0.0;
  double ab = 0.0;
  double ar = 0.0;
  double at = 0.0;
  rae_Bool havePlane = (__extension__ ({ rae_String __rae_pw_12 = (rae_String){(uint8_t*)"planeBounds", 11}; double __rae_pw_13 = pl; double __rae_pw_14 = pb; double __rae_pw_15 = pr; double __rae_pw_16 = pt; __auto_type __rae_callret = rae_sdfReadBounds_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_(&doc, &entry, (rae_View_String){ .ptr = &__rae_pw_12 }, (rae_Mod_Float64){ .ptr = &__rae_pw_13 }, (rae_Mod_Float64){ .ptr = &__rae_pw_14 }, (rae_Mod_Float64){ .ptr = &__rae_pw_15 }, (rae_Mod_Float64){ .ptr = &__rae_pw_16 }); pl = __rae_pw_13; pb = __rae_pw_14; pr = __rae_pw_15; pt = __rae_pw_16; __rae_callret; }));
  rae_Bool haveAtlas = (__extension__ ({ rae_String __rae_pw_17 = (rae_String){(uint8_t*)"atlasBounds", 11}; double __rae_pw_18 = al; double __rae_pw_19 = ab; double __rae_pw_20 = ar; double __rae_pw_21 = at; __auto_type __rae_callret = rae_sdfReadBounds_rae_JsonDoc_rae_JsonValue_rae_View_String_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_rae_Mod_Float64_(&doc, &entry, (rae_View_String){ .ptr = &__rae_pw_17 }, (rae_Mod_Float64){ .ptr = &__rae_pw_18 }, (rae_Mod_Float64){ .ptr = &__rae_pw_19 }, (rae_Mod_Float64){ .ptr = &__rae_pw_20 }, (rae_Mod_Float64){ .ptr = &__rae_pw_21 }); al = __rae_pw_18; ab = __rae_pw_19; ar = __rae_pw_20; at = __rae_pw_21; __rae_callret; }));
  rae_SdfGlyph g = (rae_SdfGlyph){ .unicode = cp, .advance = advance, .hasBounds = (bool)(havePlane && haveAtlas), .planeLeft = pl, .planeBottom = pb, .planeRight = pr, .planeTop = pt, .atlasLeft = al, .atlasBottom = ab, .atlasRight = ar, .atlasTop = at };
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_SdfGlyph_rae_List_rae_SdfGlyph_rae_SdfGlyph_(&glyphs, g); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  rae_drop_struct_rae_JsonValue_alias(&unicodeV);
  rae_drop_struct_rae_JsonValue_alias(&entry);
  }
  int64_t atlas = rae_ext_sdf_text_loadAtlas(rawAtlasPath, rae_toInt_rae_View_Float64_(atlasW), rae_toInt_rae_View_Float64_(atlasH));
  {
    rae_SdfFont __ret_val = (rae_SdfFont){ .atlas = atlas, .atlasWidth = atlasW, .atlasHeight = atlasH, .pxRange = pxRange, .emSize = emSize, .lineHeight = lineHeight, .ascender = ascender, .glyphs = glyphs };
  rae_drop_struct_rae_JsonValue_alias(&glyphsArr);
  rae_drop_struct_rae_JsonValue_alias(&metricsJ);
  rae_drop_struct_rae_JsonValue_alias(&atlasJ);
  rae_drop_struct_rae_JsonValue_alias(&root);
  rae_drop_struct_rae_JsonDoc(&doc);
  rae_string_drop(&source);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_struct_rae_JsonValue_alias(&glyphsArr);
  rae_drop_struct_rae_JsonValue_alias(&metricsJ);
  rae_drop_struct_rae_JsonValue_alias(&atlasJ);
  rae_drop_struct_rae_JsonValue_alias(&root);
  rae_drop_struct_rae_JsonDoc(&doc);
  rae_string_drop(&source);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_sdfGlyphIndex_rae_SdfFont_rae_View_Int64_(rae_SdfFont* font, int64_t cp) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t n = font->glyphs.length;
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < n); ) {
  rae_SdfGlyph g = (*(rae_SdfGlyph*)( (char*)(font->glyphs.data) + (i) * sizeof(rae_SdfGlyph) ));
  if ((bool)(g.unicode == cp)) {
  {
    int64_t __ret_val = i;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  i = i + ((int64_t)1LL);
  }
  {
    int64_t __ret_val = -(((int64_t)1LL));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_sdfMeasureText_rae_SdfFont_rae_View_String_rae_View_Float64_(rae_SdfFont* font, rae_View_String text, double sizePx) {
  int __rae_spm_func = rae_string_pool_mark();
  double scale = sizePx / font->emSize;
  double total = 0.0;
  int64_t n = rae_length_rae_View_String_(text);
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < n); ) {
  int64_t cp = rae_toInt_rae_View_Float64_(rae_at_rae_View_String_rae_View_Int64_(text, i));
  int64_t gi = rae_sdfGlyphIndex_rae_SdfFont_rae_View_Int64_(font, cp);
  if ((bool)(gi >= ((int64_t)0LL))) {
  rae_SdfGlyph g = (*(rae_SdfGlyph*)( (char*)(font->glyphs.data) + (gi) * sizeof(rae_SdfGlyph) ));
  total = total + g.advance * scale;
  }
  i = i + ((int64_t)1LL);
  }
  {
    double __ret_val = total;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_sdfDrawText_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Int64_(int64_t* fb, int64_t fbW, int64_t fbH, rae_SdfFont* font, rae_View_String text, double x, double y, double sizePx, int64_t rgb) {
  int __rae_spm_func = rae_string_pool_mark();
  double scale = sizePx / font->emSize;
  double ah = font->atlasHeight;
  double pen = x;
  int64_t n = rae_length_rae_View_String_(text);
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < n); ) {
  int64_t cp = rae_toInt_rae_View_Float64_(rae_at_rae_View_String_rae_View_Int64_(text, i));
  int64_t gi = rae_sdfGlyphIndex_rae_SdfFont_rae_View_Int64_(font, cp);
  if ((bool)(gi >= ((int64_t)0LL))) {
  rae_SdfGlyph g = (*(rae_SdfGlyph*)( (char*)(font->glyphs.data) + (gi) * sizeof(rae_SdfGlyph) ));
  if (g.hasBounds) {
  double sx0 = pen + g.planeLeft * scale;
  double sx1 = pen + g.planeRight * scale;
  double sy0 = y - g.planeTop * scale;
  double sy1 = y - g.planeBottom * scale;
  double au0 = g.atlasLeft;
  double au1 = g.atlasRight;
  double av0 = ah - g.atlasTop;
  double av1 = ah - g.atlasBottom;
  double atlasGlyphH = g.atlasTop - g.atlasBottom;
  double spr = 1.0;
  if ((bool)(atlasGlyphH > 0.0)) {
  spr = font->pxRange * (sy1 - sy0) / atlasGlyphH;
  }
  if ((bool)(spr < 1.0)) {
  spr = 1.0;
  }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_sdf_text_blitGlyph(fb, fbW, fbH, font->atlas, sx0, sy0, sx1, sy1, au0, av0, au1, av1, spr, rgb); rae_string_pool_flush(__rae_spm); }
  }
  pen = pen + g.advance * scale;
  }
  i = i + ((int64_t)1LL);
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_desktopDir_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_filesystem_userFolder(((int64_t)0LL));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_picturesDir_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_filesystem_userFolder(((int64_t)1LL));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_documentsDir_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_filesystem_userFolder(((int64_t)2LL));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_homeDir_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_filesystem_userFolder(((int64_t)3LL));
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_join_rae_View_String_rae_View_String_(rae_View_String a, rae_View_String b) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_String __ret_val = rae_ext_rae_str_interp(5, (rae_String){(uint8_t*)"", 0}, rae_ext_rae_str(((*a.ptr))), (rae_String){(uint8_t*)"/", 1}, rae_ext_rae_str(((*b.ptr))), (rae_String){(uint8_t*)"", 0});
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_pad4_rae_View_Int64_(int64_t n) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_String s = rae_string_pool_take(rae_ext_rae_str_interp(3, (rae_String){(uint8_t*)"", 0}, rae_ext_rae_str((n)), (rae_String){(uint8_t*)"", 0}));
  for (; (bool)((__extension__ ({ rae_String __rae_pw_0 = s; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); })) < ((int64_t)4LL)); ) {
  { rae_String __asg1 = rae_ext_rae_str_interp(3, (rae_String){(uint8_t*)"0", 1}, rae_string_borrow(s), (rae_String){(uint8_t*)"", 0}); rae_string_drop(&s); s = rae_string_pool_take(__asg1); };
  }
  {
    rae_String __ret_val = s;
    __ret_val = rae_string_pool_take(__ret_val);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_String rae_renderPath_rae_View_String_(rae_View_String stem) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_String dir = rae_string_pool_take(rae_desktopDir_());
  if ((bool)((__extension__ ({ rae_String __rae_pw_0 = dir; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }); })) == ((int64_t)0LL))) {
  { rae_String __asg1 = (__extension__ ({ rae_String __rae_pw_2 = (__extension__ ({ rae_String __rae_pw_4 = (rae_String){(uint8_t*)"Rae", 3}; rae_String __rae_pw_5 = (rae_String){(uint8_t*)"render", 6}; rae_ext_filesystem_prefDir((rae_View_String){ .ptr = &__rae_pw_4 }, (rae_View_String){ .ptr = &__rae_pw_5 }); })); rae_String __rae_pw_3 = (rae_String){(uint8_t*)"render", 6}; rae_join_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_2 }, (rae_View_String){ .ptr = &__rae_pw_3 }); })); rae_string_drop(&dir); dir = rae_string_pool_take(__asg1); };
  }
  for (; (bool)((bool)((__extension__ ({ rae_String __rae_pw_6 = dir; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_6 }); })) > ((int64_t)1LL)) && (__extension__ ({ rae_String __rae_pw_7 = dir; rae_String __rae_pw_8 = (rae_String){(uint8_t*)"/", 1}; rae_endsWith_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_7 }, (rae_View_String){ .ptr = &__rae_pw_8 }); }))); ) {
  { rae_String __asg9 = (__extension__ ({ rae_String __rae_pw_10 = dir; rae_sub_rae_View_String_rae_View_Int64_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_10 }, ((int64_t)0LL), (__extension__ ({ rae_String __rae_pw_11 = dir; rae_length_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_11 }); })) - ((int64_t)1LL)); })); rae_string_drop(&dir); dir = rae_string_pool_take(__asg9); };
  }
  rae_Bool made = (__extension__ ({ rae_String __rae_pw_12 = dir; rae_ext_filesystem_makeDir((rae_View_String){ .ptr = &__rae_pw_12 }); }));
  rae_String date = rae_string_pool_take(rae_ext_filesystem_today());
  rae_String prefix = rae_string_pool_take(rae_ext_rae_str_interp(5, (rae_String){(uint8_t*)"", 0}, rae_ext_rae_str(((*stem.ptr))), (rae_String){(uint8_t*)"_", 1}, rae_string_borrow(date), (rae_String){(uint8_t*)"_", 1}));
  int64_t n = (__extension__ ({ rae_String __rae_pw_13 = dir; rae_String __rae_pw_14 = prefix; rae_ext_filesystem_nextIndex((rae_View_String){ .ptr = &__rae_pw_13 }, (rae_View_String){ .ptr = &__rae_pw_14 }); }));
  {
    rae_String __ret_val = rae_ext_rae_str_interp(7, (rae_String){(uint8_t*)"", 0}, rae_string_borrow(dir), (rae_String){(uint8_t*)"/", 1}, rae_string_borrow(prefix), (rae_String){(uint8_t*)"", 0}, rae_ext_rae_str((rae_pad4_rae_View_Int64_(n))), (rae_String){(uint8_t*)".png", 4});
    __ret_val = rae_string_pool_take(__ret_val);
  rae_string_drop(&prefix);
  rae_string_drop(&date);
  rae_string_drop(&dir);
    rae_string_pool_flush(__rae_spm_func);
    __ret_val = rae_string_pool_register_owned(__ret_val);
    return __ret_val;
  }
  rae_string_drop(&prefix);
  rae_string_drop(&date);
  rae_string_drop(&dir);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(double x, double y, double z) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = x, .y = y, .z = z };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_vadd_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = a->x + b->x, .y = a->y + b->y, .z = a->z + b->z };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_vsub_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = a->x - b->x, .y = a->y - b->y, .z = a->z - b->z };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_vscale_rae_Vec3_rae_View_Float64_(rae_Vec3* a, double s) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = a->x * s, .y = a->y * s, .z = a->z * s };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_vmul_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = a->x * b->x, .y = a->y * b->y, .z = a->z * b->z };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_vneg_rae_Vec3_(rae_Vec3* a) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = 0.0 - a->x, .y = 0.0 - a->y, .z = 0.0 - a->z };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_vdot_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = a->x * b->x + a->y * b->y + a->z * b->z;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_vcross_rae_Vec3_rae_Vec3_(rae_Vec3* a, rae_Vec3* b) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = a->y * b->z - a->z * b->y, .y = a->z * b->x - a->x * b->z, .z = a->x * b->y - a->y * b->x };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_vlenSq_rae_Vec3_(rae_Vec3* a) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = a->x * a->x + a->y * a->y + a->z * a->z;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_vlen_rae_Vec3_(rae_Vec3* a) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = rae_ext_math_sqrt(rae_vlenSq_rae_Vec3_(a));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Vec3 rae_vnorm_rae_Vec3_(rae_Vec3* a) {
  int __rae_spm_func = rae_string_pool_mark();
  double l = rae_vlen_rae_Vec3_(a);
  {
    rae_Vec3 __ret_val = (rae_Vec3){ .x = a->x / l, .y = a->y / l, .z = a->z / l };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_faceIndex_rae_View_String_(rae_View_String token) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_String parts = (__extension__ ({ rae_String __rae_pw_0 = (rae_String){(uint8_t*)"/", 1}; rae_split_rae_View_String_rae_View_String_(token, (rae_View_String){ .ptr = &__rae_pw_0 }); }));
  rae_String s = rae_string_pool_take((*(rae_String*)( (char*)(parts.data) + (((int64_t)0LL)) * sizeof(rae_String) )));
  s.is_owned = 0;
  {
    int64_t __ret_val = (__extension__ ({ rae_String __rae_pw_1 = s; rae_toInt_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_1 }); })) - ((int64_t)1LL);
  rae_drop_rae_String_rae_List_rae_String_(&parts);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_rae_String_rae_List_rae_String_(&parts);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_faceNormalIndex_rae_View_String_(rae_View_String token) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_String parts = (__extension__ ({ rae_String __rae_pw_0 = (rae_String){(uint8_t*)"/", 1}; rae_split_rae_View_String_rae_View_String_(token, (rae_View_String){ .ptr = &__rae_pw_0 }); }));
  if ((bool)(parts.length < ((int64_t)3LL))) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_drop_rae_String_rae_List_rae_String_(&parts);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  rae_String s = rae_string_pool_take((*(rae_String*)( (char*)(parts.data) + (((int64_t)2LL)) * sizeof(rae_String) )));
  s.is_owned = 0;
  if ((__extension__ ({ rae_String __rae_pw_1 = s; rae_String __rae_pw_2 = (rae_String){(uint8_t*)"", 0}; rae_equals_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_1 }, (rae_View_String){ .ptr = &__rae_pw_2 }); }))) {
  {
    int64_t __ret_val = -(((int64_t)1LL));
  rae_drop_rae_String_rae_List_rae_String_(&parts);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    int64_t __ret_val = (__extension__ ({ rae_String __rae_pw_3 = s; rae_toInt_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_3 }); })) - ((int64_t)1LL);
  rae_drop_rae_String_rae_List_rae_String_(&parts);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_rae_String_rae_List_rae_String_(&parts);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Triangle rae_loadObjTriangles_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Bool_rae_Material_(rae_View_String path, double scale, double ox, double oy, double oz, rae_Bool yaw180, rae_Material* mat) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_String text = rae_string_pool_take(rae_ext_rae_sys_read_file((*path.ptr)));
  rae_List_rae_String lines = (__extension__ ({ rae_String __rae_pw_0 = text; rae_String __rae_pw_1 = (rae_String){(uint8_t*)"\n", 1}; rae_split_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }, (rae_View_String){ .ptr = &__rae_pw_1 }); }));
  rae_List_double vx = rae_createList_double_rae_View_Int64_(((int64_t)4096LL));
  rae_List_double vy = rae_createList_double_rae_View_Int64_(((int64_t)4096LL));
  rae_List_double vz = rae_createList_double_rae_View_Int64_(((int64_t)4096LL));
  double flip = 1.0;
  if (yaw180) {
  flip = -(1.0);
  }
  int64_t li = ((int64_t)0LL);
  for (; (bool)(li < lines.length); ) {
  rae_String line = rae_string_pool_take((*(rae_String*)( (char*)(lines.data) + (li) * sizeof(rae_String) )));
  line.is_owned = 0;
  rae_List_rae_String tok = (__extension__ ({ rae_String __rae_pw_2 = line; rae_String __rae_pw_3 = (rae_String){(uint8_t*)" ", 1}; rae_split_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_2 }, (rae_View_String){ .ptr = &__rae_pw_3 }); }));
  if ((bool)(tok.length >= ((int64_t)4LL))) {
  rae_String t0 = rae_string_pool_take((*(rae_String*)( (char*)(tok.data) + (((int64_t)0LL)) * sizeof(rae_String) )));
  t0.is_owned = 0;
  if ((__extension__ ({ rae_String __rae_pw_4 = t0; rae_String __rae_pw_5 = (rae_String){(uint8_t*)"v", 1}; rae_equals_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_4 }, (rae_View_String){ .ptr = &__rae_pw_5 }); }))) {
  double px = (__extension__ ({ rae_String __rae_pw_6 = (*(rae_String*)( (char*)(tok.data) + (((int64_t)1LL)) * sizeof(rae_String) )); rae_toFloat_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_6 }); })) * flip;
  double py = (__extension__ ({ rae_String __rae_pw_7 = (*(rae_String*)( (char*)(tok.data) + (((int64_t)2LL)) * sizeof(rae_String) )); rae_toFloat_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_7 }); }));
  double pz = (__extension__ ({ rae_String __rae_pw_8 = (*(rae_String*)( (char*)(tok.data) + (((int64_t)3LL)) * sizeof(rae_String) )); rae_toFloat_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_8 }); })) * flip;
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&vx, px * scale + ox); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&vy, pz * scale + oy); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&vz, py * scale + oz); rae_string_pool_flush(__rae_spm); }
  }
  }
  li = li + ((int64_t)1LL);
  rae_drop_rae_String_rae_List_rae_String_(&tok);
  }
  rae_List_double nx = rae_createList_double_rae_View_Int64_(((int64_t)4096LL));
  rae_List_double ny = rae_createList_double_rae_View_Int64_(((int64_t)4096LL));
  rae_List_double nz = rae_createList_double_rae_View_Int64_(((int64_t)4096LL));
  li = ((int64_t)0LL);
  for (; (bool)(li < lines.length); ) {
  rae_String line = rae_string_pool_take((*(rae_String*)( (char*)(lines.data) + (li) * sizeof(rae_String) )));
  line.is_owned = 0;
  rae_List_rae_String tok = (__extension__ ({ rae_String __rae_pw_9 = line; rae_String __rae_pw_10 = (rae_String){(uint8_t*)" ", 1}; rae_split_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_9 }, (rae_View_String){ .ptr = &__rae_pw_10 }); }));
  if ((bool)(tok.length >= ((int64_t)4LL))) {
  rae_String t0 = rae_string_pool_take((*(rae_String*)( (char*)(tok.data) + (((int64_t)0LL)) * sizeof(rae_String) )));
  t0.is_owned = 0;
  if ((__extension__ ({ rae_String __rae_pw_11 = t0; rae_String __rae_pw_12 = (rae_String){(uint8_t*)"vn", 2}; rae_equals_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_11 }, (rae_View_String){ .ptr = &__rae_pw_12 }); }))) {
  double mnx = (__extension__ ({ rae_String __rae_pw_13 = (*(rae_String*)( (char*)(tok.data) + (((int64_t)1LL)) * sizeof(rae_String) )); rae_toFloat_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_13 }); })) * flip;
  double mny = (__extension__ ({ rae_String __rae_pw_14 = (*(rae_String*)( (char*)(tok.data) + (((int64_t)2LL)) * sizeof(rae_String) )); rae_toFloat_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_14 }); }));
  double mnz = (__extension__ ({ rae_String __rae_pw_15 = (*(rae_String*)( (char*)(tok.data) + (((int64_t)3LL)) * sizeof(rae_String) )); rae_toFloat_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_15 }); })) * flip;
  double rx = mnx;
  double ry = mnz;
  double rz = mny;
  double ln = rae_ext_math_sqrt(rx * rx + ry * ry + rz * rz);
  if ((bool)(ln < 0.000001)) {
  ln = 1.0;
  }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&nx, rx / ln); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&ny, ry / ln); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&nz, rz / ln); rae_string_pool_flush(__rae_spm); }
  }
  }
  li = li + ((int64_t)1LL);
  rae_drop_rae_String_rae_List_rae_String_(&tok);
  }
  rae_Bool haveNormals = (bool)(nx.length > ((int64_t)0LL));
  rae_List_rae_Triangle tris = rae_createList_rae_Triangle_rae_View_Int64_(((int64_t)8192LL));
  li = ((int64_t)0LL);
  for (; (bool)(li < lines.length); ) {
  rae_String line = rae_string_pool_take((*(rae_String*)( (char*)(lines.data) + (li) * sizeof(rae_String) )));
  line.is_owned = 0;
  rae_List_rae_String tok = (__extension__ ({ rae_String __rae_pw_16 = line; rae_String __rae_pw_17 = (rae_String){(uint8_t*)" ", 1}; rae_split_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_16 }, (rae_View_String){ .ptr = &__rae_pw_17 }); }));
  if ((bool)(tok.length >= ((int64_t)4LL))) {
  rae_String t0 = rae_string_pool_take((*(rae_String*)( (char*)(tok.data) + (((int64_t)0LL)) * sizeof(rae_String) )));
  t0.is_owned = 0;
  if ((__extension__ ({ rae_String __rae_pw_18 = t0; rae_String __rae_pw_19 = (rae_String){(uint8_t*)"f", 1}; rae_equals_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_18 }, (rae_View_String){ .ptr = &__rae_pw_19 }); }))) {
  rae_String tk1 = rae_string_pool_take((*(rae_String*)( (char*)(tok.data) + (((int64_t)1LL)) * sizeof(rae_String) )));
  tk1.is_owned = 0;
  rae_String tk2 = rae_string_pool_take((*(rae_String*)( (char*)(tok.data) + (((int64_t)2LL)) * sizeof(rae_String) )));
  tk2.is_owned = 0;
  rae_String tk3 = rae_string_pool_take((*(rae_String*)( (char*)(tok.data) + (((int64_t)3LL)) * sizeof(rae_String) )));
  tk3.is_owned = 0;
  int64_t i0 = (__extension__ ({ rae_String __rae_pw_20 = tk1; rae_faceIndex_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_20 }); }));
  int64_t i1 = (__extension__ ({ rae_String __rae_pw_21 = tk2; rae_faceIndex_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_21 }); }));
  int64_t i2 = (__extension__ ({ rae_String __rae_pw_22 = tk3; rae_faceIndex_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_22 }); }));
  rae_Vec3 p0 = (rae_Vec3){ .x = rae_at_double_rae_List_double_rae_View_Int64_(&vx, i0), .y = rae_at_double_rae_List_double_rae_View_Int64_(&vy, i0), .z = rae_at_double_rae_List_double_rae_View_Int64_(&vz, i0) };
  rae_Vec3 p1 = (rae_Vec3){ .x = rae_at_double_rae_List_double_rae_View_Int64_(&vx, i1), .y = rae_at_double_rae_List_double_rae_View_Int64_(&vy, i1), .z = rae_at_double_rae_List_double_rae_View_Int64_(&vz, i1) };
  rae_Vec3 p2 = (rae_Vec3){ .x = rae_at_double_rae_List_double_rae_View_Int64_(&vx, i2), .y = rae_at_double_rae_List_double_rae_View_Int64_(&vy, i2), .z = rae_at_double_rae_List_double_rae_View_Int64_(&vz, i2) };
  rae_Vec3 nn0 = (rae_Vec3){ .x = 0.0, .y = 0.0, .z = 1.0 };
  rae_Vec3 nn1 = (rae_Vec3){ .x = 0.0, .y = 0.0, .z = 1.0 };
  rae_Vec3 nn2 = (rae_Vec3){ .x = 0.0, .y = 0.0, .z = 1.0 };
  int64_t ni0 = (__extension__ ({ rae_String __rae_pw_23 = tk1; rae_faceNormalIndex_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_23 }); }));
  int64_t ni1 = (__extension__ ({ rae_String __rae_pw_24 = tk2; rae_faceNormalIndex_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_24 }); }));
  int64_t ni2 = (__extension__ ({ rae_String __rae_pw_25 = tk3; rae_faceNormalIndex_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_25 }); }));
  if ((bool)((bool)((bool)(haveNormals && (bool)(ni0 >= ((int64_t)0LL))) && (bool)(ni1 >= ((int64_t)0LL))) && (bool)(ni2 >= ((int64_t)0LL)))) {
  nn0 = (rae_Vec3)(rae_Vec3){ .x = rae_at_double_rae_List_double_rae_View_Int64_(&nx, ni0), .y = rae_at_double_rae_List_double_rae_View_Int64_(&ny, ni0), .z = rae_at_double_rae_List_double_rae_View_Int64_(&nz, ni0) };
  nn1 = (rae_Vec3)(rae_Vec3){ .x = rae_at_double_rae_List_double_rae_View_Int64_(&nx, ni1), .y = rae_at_double_rae_List_double_rae_View_Int64_(&ny, ni1), .z = rae_at_double_rae_List_double_rae_View_Int64_(&nz, ni1) };
  nn2 = (rae_Vec3)(rae_Vec3){ .x = rae_at_double_rae_List_double_rae_View_Int64_(&nx, ni2), .y = rae_at_double_rae_List_double_rae_View_Int64_(&ny, ni2), .z = rae_at_double_rae_List_double_rae_View_Int64_(&nz, ni2) };
  } else {
  double ex = p1.x - p0.x;
  double ey = p1.y - p0.y;
  double ez = p1.z - p0.z;
  double fx = p2.x - p0.x;
  double fy = p2.y - p0.y;
  double fz = p2.z - p0.z;
  double gx = ey * fz - ez * fy;
  double gy = ez * fx - ex * fz;
  double gz = ex * fy - ey * fx;
  double gl = rae_ext_math_sqrt(gx * gx + gy * gy + gz * gz);
  if ((bool)(gl < 0.000001)) {
  gl = 1.0;
  }
  rae_Vec3 g = (rae_Vec3){ .x = gx / gl, .y = gy / gl, .z = gz / gl };
  nn0 = g;
  nn1 = g;
  nn2 = g;
  }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Triangle_rae_List_rae_Triangle_rae_Triangle_(&tris, (rae_Triangle){ .v0 = p0, .v1 = p1, .v2 = p2, .n0 = nn0, .n1 = nn1, .n2 = nn2, .material = (*mat) }); rae_string_pool_flush(__rae_spm); }
  }
  }
  li = li + ((int64_t)1LL);
  rae_drop_rae_String_rae_List_rae_String_(&tok);
  }
  {
    rae_List_rae_Triangle __ret_val = tris;
  rae_drop_double_rae_List_double_(&nz);
  rae_drop_double_rae_List_double_(&ny);
  rae_drop_double_rae_List_double_(&nx);
  rae_drop_double_rae_List_double_(&vz);
  rae_drop_double_rae_List_double_(&vy);
  rae_drop_double_rae_List_double_(&vx);
  rae_drop_rae_String_rae_List_rae_String_(&lines);
  rae_string_drop(&text);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_double_rae_List_double_(&nz);
  rae_drop_double_rae_List_double_(&ny);
  rae_drop_double_rae_List_double_(&nx);
  rae_drop_double_rae_List_double_(&vz);
  rae_drop_double_rae_List_double_(&vy);
  rae_drop_double_rae_List_double_(&vx);
  rae_drop_rae_String_rae_List_rae_String_(&lines);
  rae_string_drop(&text);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Box rae_buildSceneOneBoxes_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_Box b = rae_createList_rae_Box_rae_View_Int64_(((int64_t)1LL));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Box_rae_List_rae_Box_rae_Box_(&b, (rae_Box){ .lo = (rae_Vec3){ .x = -(0.5), .y = -(0.5), .z = -(0.5) }, .hi = (rae_Vec3){ .x = 0.5, .y = 0.5, .z = 0.5 }, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.3, .z = 0.3 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_Box __ret_val = b;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneDiffuse_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_Sphere s = rae_createList_rae_Sphere_rae_View_Int64_(((int64_t)4LL));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 1.0, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.3, .z = 0.3 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(1.05), .y = 1.05, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.3, .y = 0.4, .z = 0.8 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 1.05, .y = 1.05, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.3, .y = 0.7, .z = 0.3 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 1.0, .z = -(100.5) }, .radius = 100.0, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.6, .y = 0.6, .z = 0.6 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_Sphere __ret_val = s;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneLights_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_Sphere s = rae_createList_rae_Sphere_rae_View_Int64_(((int64_t)8LL));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 1.0, .z = -(100.5) }, .radius = 100.0, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.45, .y = 0.6, .z = 0.7 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 1.0, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.3, .z = 0.3 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 1.1, .y = 1.2, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)1LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.6, .z = 0.2 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(1.6), .y = 1.7, .z = 0.1 }, .radius = 0.4, .material = (rae_Material){ .kind = ((int64_t)1LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.4, .z = 0.8 }, .fuzz = 0.3, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(1.0), .y = 0.8, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)2LL), .albedo = (rae_Vec3){ .x = 1.0, .y = 1.0, .z = 1.0 }, .fuzz = 0.0, .ior = 1.5 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.4, .y = -(0.1), .z = -(0.25) }, .radius = 0.25, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.1, .y = 0.2, .z = 0.8 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 1.4, .y = 1.2, .z = 3.0 }, .radius = 1.2, .material = (rae_Material){ .kind = ((int64_t)3LL), .albedo = (rae_Vec3){ .x = 4.0, .y = 3.6, .z = 3.0 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(0.7), .y = 0.2, .z = 0.05 }, .radius = 0.12, .material = (rae_Material){ .kind = ((int64_t)3LL), .albedo = (rae_Vec3){ .x = 16.0, .y = 16.0, .z = 16.0 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_Sphere __ret_val = s;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Sphere rae_buildScene_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_Sphere s = rae_createList_rae_Sphere_rae_View_Int64_(((int64_t)4LL));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 1.0, .z = -(100.5) }, .radius = 100.0, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.8, .z = 0.0 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 1.0, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.1, .y = 0.2, .z = 0.5 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(1.0), .y = 1.0, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)2LL), .albedo = (rae_Vec3){ .x = 1.0, .y = 1.0, .z = 1.0 }, .fuzz = 0.0, .ior = 1.5 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 1.0, .y = 1.0, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)1LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.6, .z = 0.2 }, .fuzz = 0.05, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_Sphere __ret_val = s;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneOne_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_Sphere s = rae_createList_rae_Sphere_rae_View_Int64_(((int64_t)8LL));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = -(1.0), .z = 6.0 }, .radius = 2.0, .material = (rae_Material){ .kind = ((int64_t)3LL), .albedo = (rae_Vec3){ .x = 4.0, .y = 4.0, .z = 4.0 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 3.85, .y = -(0.15), .z = 2.3 }, .radius = 0.2, .material = (rae_Material){ .kind = ((int64_t)3LL), .albedo = (rae_Vec3){ .x = 16.0, .y = 16.0, .z = 16.0 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = -(2.0), .z = 0.3 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.3, .z = 0.3 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = -(1.0), .z = -(100.5) }, .radius = 100.0, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.0, .y = 0.7, .z = 0.8 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 1.0, .y = 0.0, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)1LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.6, .z = 0.2 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(1.5), .y = 0.5, .z = 0.65 }, .radius = 0.4, .material = (rae_Material){ .kind = ((int64_t)1LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.4, .z = 0.8 }, .fuzz = 0.3, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(1.0), .y = 1.0, .z = 0.0 }, .radius = 0.5, .material = (rae_Material){ .kind = ((int64_t)2LL), .albedo = (rae_Vec3){ .x = 1.0, .y = 1.0, .z = 1.0 }, .fuzz = 0.0, .ior = 1.5 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(3.15), .y = -(5.0), .z = 0.1 }, .radius = 0.6, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.05, .y = 0.2, .z = 0.8 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_Sphere __ret_val = s;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Sphere rae_buildSceneBook_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_seed_rae_View_Int64_(((int64_t)1LL)); rae_string_pool_flush(__rae_spm); }
  rae_List_rae_Sphere s = rae_createList_rae_Sphere_rae_View_Int64_(((int64_t)512LL));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 0.0, .z = -(1000.0) }, .radius = 1000.0, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.5, .y = 0.5, .z = 0.5 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  int64_t a = -(((int64_t)11LL));
  for (; (bool)(a < ((int64_t)11LL)); ) {
  int64_t b = -(((int64_t)11LL));
  for (; (bool)(b < ((int64_t)11LL)); ) {
  double cx = rae_toFloat_rae_View_Int64_(a) + 0.9 * rae_random_();
  double cy = rae_toFloat_rae_View_Int64_(b) + 0.9 * rae_random_();
  double dx = cx - 4.0;
  if ((bool)(dx * dx + cy * cy > 0.81)) {
  double chooseMat = rae_random_();
  if ((bool)(chooseMat < 0.8)) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = cx, .y = cy, .z = 0.2 }, .radius = 0.2, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = rae_random_() * rae_random_(), .y = rae_random_() * rae_random_(), .z = rae_random_() * rae_random_() }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  } else {
  if ((bool)(chooseMat < 0.95)) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = cx, .y = cy, .z = 0.2 }, .radius = 0.2, .material = (rae_Material){ .kind = ((int64_t)1LL), .albedo = (rae_Vec3){ .x = 0.5 * (1.0 + rae_random_()), .y = 0.5 * (1.0 + rae_random_()), .z = 0.5 * (1.0 + rae_random_()) }, .fuzz = 0.5 * rae_random_(), .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  } else {
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = cx, .y = cy, .z = 0.2 }, .radius = 0.2, .material = (rae_Material){ .kind = ((int64_t)2LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.5, .z = 0.3 }, .fuzz = 0.0, .ior = 1.5 } }); rae_string_pool_flush(__rae_spm); }
  }
  }
  }
  b = b + ((int64_t)1LL);
  }
  a = a + ((int64_t)1LL);
  }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 0.0, .y = 0.0, .z = 1.0 }, .radius = 1.0, .material = (rae_Material){ .kind = ((int64_t)2LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.5, .z = 0.3 }, .fuzz = 0.0, .ior = 1.5 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = -(4.0), .y = 0.0, .z = 1.0 }, .radius = 1.0, .material = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.0, .y = 0.2, .z = 0.9 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(&s, (rae_Sphere){ .center = (rae_Vec3){ .x = 4.0, .y = 0.0, .z = 1.0 }, .radius = 1.0, .material = (rae_Material){ .kind = ((int64_t)1LL), .albedo = (rae_Vec3){ .x = 0.7, .y = 0.6, .z = 0.5 }, .fuzz = 0.0, .ior = 0.0 } }); rae_string_pool_flush(__rae_spm); }
  {
    rae_List_rae_Sphere __ret_val = s;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Camera rae_buildCamera_rae_Vec3_rae_Vec3_rae_View_Float64_rae_View_Float64_rae_View_Float64_(rae_Vec3* pos, rae_Vec3* target, double vfovDeg, double aperture, double aspect) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_Vec3 worldUp = rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(0.0, 0.0, 1.0);
  rae_Vec3 fwd = rae_vnorm_rae_Vec3_(((rae_Vec3[1]){ rae_vsub_rae_Vec3_rae_Vec3_(target, pos) }));
  rae_Vec3 right = rae_vnorm_rae_Vec3_(((rae_Vec3[1]){ rae_vcross_rae_Vec3_rae_Vec3_(&fwd, &worldUp) }));
  rae_Vec3 up = rae_vcross_rae_Vec3_rae_Vec3_(&right, &fwd);
  double focus = 1.0;
  double theta = vfovDeg * pi / 180.0;
  double halfH = rae_ext_math_tan(theta / 2.0);
  double vpH = 2.0 * halfH * focus;
  double vpW = aspect * vpH;
  rae_Vec3 horiz = rae_vscale_rae_Vec3_rae_View_Float64_(&right, vpW);
  rae_Vec3 vert = rae_vscale_rae_Vec3_rae_View_Float64_(&up, vpH);
  rae_Vec3 center = rae_vadd_rae_Vec3_rae_Vec3_(pos, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&fwd, focus) }));
  rae_Vec3 ll = rae_vsub_rae_Vec3_rae_Vec3_(((rae_Vec3[1]){ rae_vsub_rae_Vec3_rae_Vec3_(&center, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&horiz, 0.5) })) }), ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&vert, 0.5) }));
  {
    rae_Camera __ret_val = (rae_Camera){ .origin = (*pos), .lowerLeft = ll, .horizontal = horiz, .vertical = vert, .right = right, .up = up, .lensRadius = aperture * 0.5 };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Ray rae_makeRay_rae_Camera_rae_View_Float64_rae_View_Float64_(rae_Camera* cam, double u, double v) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_Vec3 ro = cam->origin;
  if ((bool)(cam->lensRadius > 0.0)) {
  double a = rae_random_() * tau;
  double r = cam->lensRadius * rae_ext_math_sqrt(rae_random_());
  double lx = r * rae_ext_math_cos(a);
  double ly = r * rae_ext_math_sin(a);
  ro = rae_vadd_rae_Vec3_rae_Vec3_(&cam->origin, ((rae_Vec3[1]){ rae_vadd_rae_Vec3_rae_Vec3_(((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&cam->right, lx) }), ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&cam->up, ly) })) }));
  }
  rae_Vec3 target = rae_vadd_rae_Vec3_rae_Vec3_(&cam->lowerLeft, ((rae_Vec3[1]){ rae_vadd_rae_Vec3_rae_Vec3_(((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&cam->horizontal, u) }), ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&cam->vertical, v) })) }));
  {
    rae_Ray __ret_val = (rae_Ray){ .origin = ro, .dir = rae_vsub_rae_Vec3_rae_Vec3_(&target, &ro) };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_fmin_rae_View_Float64_rae_View_Float64_(double a, double b) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(a < b)) {
  {
    double __ret_val = a;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    double __ret_val = b;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_fmax_rae_View_Float64_rae_View_Float64_(double a, double b) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(a > b)) {
  {
    double __ret_val = a;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    double __ret_val = b;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_buildNode_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(rae_List_rae_PrimRec* prims, int64_t lo, int64_t hi, rae_List_double* nodes, int64_t leafSize) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t myIdx = nodes->length / ((int64_t)8LL);
  double mnx = 1000000000.0;
  double mny = 1000000000.0;
  double mnz = 1000000000.0;
  double mxx = -(1000000000.0);
  double mxy = -(1000000000.0);
  double mxz = -(1000000000.0);
  double cbnx = 1000000000.0;
  double cbny = 1000000000.0;
  double cbnz = 1000000000.0;
  double cbxx = -(1000000000.0);
  double cbxy = -(1000000000.0);
  double cbxz = -(1000000000.0);
  int64_t i = lo;
  for (; (bool)(i < hi); ) {
  rae_PrimRec p = (*(rae_PrimRec*)( (char*)(prims->data) + (i) * sizeof(rae_PrimRec) ));
  mnx = rae_fmin_rae_View_Float64_rae_View_Float64_(mnx, p.mnx);
  mny = rae_fmin_rae_View_Float64_rae_View_Float64_(mny, p.mny);
  mnz = rae_fmin_rae_View_Float64_rae_View_Float64_(mnz, p.mnz);
  mxx = rae_fmax_rae_View_Float64_rae_View_Float64_(mxx, p.mxx);
  mxy = rae_fmax_rae_View_Float64_rae_View_Float64_(mxy, p.mxy);
  mxz = rae_fmax_rae_View_Float64_rae_View_Float64_(mxz, p.mxz);
  cbnx = rae_fmin_rae_View_Float64_rae_View_Float64_(cbnx, p.cx);
  cbny = rae_fmin_rae_View_Float64_rae_View_Float64_(cbny, p.cy);
  cbnz = rae_fmin_rae_View_Float64_rae_View_Float64_(cbnz, p.cz);
  cbxx = rae_fmax_rae_View_Float64_rae_View_Float64_(cbxx, p.cx);
  cbxy = rae_fmax_rae_View_Float64_rae_View_Float64_(cbxy, p.cy);
  cbxz = rae_fmax_rae_View_Float64_rae_View_Float64_(cbxz, p.cz);
  i = i + ((int64_t)1LL);
  }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, mnx); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, mny); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, mnz); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, mxx); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, mxy); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, mxz); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, 0.0); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(nodes, 0.0); rae_string_pool_flush(__rae_spm); }
  int64_t count = hi - lo;
  if ((bool)(count <= leafSize)) {
  { int __rae_spm = rae_string_pool_mark(); rae_set_double_rae_List_double_rae_View_Int64_double_(nodes, myIdx * ((int64_t)8LL) + ((int64_t)6LL), rae_toFloat_rae_View_Int64_(lo)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_double_rae_List_double_rae_View_Int64_double_(nodes, myIdx * ((int64_t)8LL) + ((int64_t)7LL), rae_toFloat_rae_View_Int64_(count)); rae_string_pool_flush(__rae_spm); }
  {
    int64_t __ret_val = myIdx;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  double ex = cbxx - cbnx;
  double ey = cbxy - cbny;
  double ez = cbxz - cbnz;
  int64_t axis = ((int64_t)0LL);
  if ((bool)(ey > ex)) {
  axis = ((int64_t)1LL);
  }
  if ((bool)(ez > rae_fmax_rae_View_Float64_rae_View_Float64_(ex, ey))) {
  axis = ((int64_t)2LL);
  }
  double mid = 0.5 * (cbnx + cbxx);
  if ((bool)(axis == ((int64_t)1LL))) {
  mid = 0.5 * (cbny + cbxy);
  }
  if ((bool)(axis == ((int64_t)2LL))) {
  mid = 0.5 * (cbnz + cbxz);
  }
  int64_t m = lo;
  int64_t j = lo;
  for (; (bool)(j < hi); ) {
  rae_PrimRec pj = (*(rae_PrimRec*)( (char*)(prims->data) + (j) * sizeof(rae_PrimRec) ));
  double cj = pj.cx;
  if ((bool)(axis == ((int64_t)1LL))) {
  cj = pj.cy;
  }
  if ((bool)(axis == ((int64_t)2LL))) {
  cj = pj.cz;
  }
  if ((bool)(cj < mid)) {
  rae_PrimRec pm = (*(rae_PrimRec*)( (char*)(prims->data) + (m) * sizeof(rae_PrimRec) ));
  { int __rae_spm = rae_string_pool_mark(); rae_set_rae_PrimRec_rae_List_rae_PrimRec_rae_View_Int64_rae_PrimRec_(prims, m, pj); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_rae_PrimRec_rae_List_rae_PrimRec_rae_View_Int64_rae_PrimRec_(prims, j, pm); rae_string_pool_flush(__rae_spm); }
  m = m + ((int64_t)1LL);
  }
  j = j + ((int64_t)1LL);
  }
  if ((bool)(m == lo)) {
  m = (lo + hi) / ((int64_t)2LL);
  }
  if ((bool)(m == hi)) {
  m = (lo + hi) / ((int64_t)2LL);
  }
  int64_t right = rae_buildNodeRight_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(prims, lo, m, hi, nodes, leafSize);
  { int __rae_spm = rae_string_pool_mark(); rae_set_double_rae_List_double_rae_View_Int64_double_(nodes, myIdx * ((int64_t)8LL) + ((int64_t)6LL), rae_toFloat_rae_View_Int64_(right)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_double_rae_List_double_rae_View_Int64_double_(nodes, myIdx * ((int64_t)8LL) + ((int64_t)7LL), 0.0); rae_string_pool_flush(__rae_spm); }
  {
    int64_t __ret_val = myIdx;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_buildNodeRight_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(rae_List_rae_PrimRec* prims, int64_t lo, int64_t m, int64_t hi, rae_List_double* nodes, int64_t leafSize) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t dummyLeft = rae_buildNode_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(prims, lo, m, nodes, leafSize);
  {
    int64_t __ret_val = rae_buildNode_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(prims, m, hi, nodes, leafSize);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_double rae_buildBvh_rae_List_rae_PrimRec_rae_View_Int64_(rae_List_rae_PrimRec* prims, int64_t leafSize) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_double nodes = rae_createList_double_rae_View_Int64_(prims->length * ((int64_t)16LL) + ((int64_t)8LL));
  if ((bool)(prims->length > ((int64_t)0LL))) {
  int64_t r = rae_buildNode_rae_List_rae_PrimRec_rae_View_Int64_rae_View_Int64_rae_List_double_rae_View_Int64_(prims, ((int64_t)0LL), prims->length, &nodes, leafSize);
  }
  {
    rae_List_double __ret_val = nodes;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static int64_t rae_triBase_(void) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    int64_t __ret_val = ((int64_t)1000000LL);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_double rae_triFloats_rae_List_rae_Triangle_(rae_List_rae_Triangle* tris) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_double f = rae_createList_double_rae_View_Int64_(((int64_t)16LL));
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < tris->length); ) {
  rae_Triangle t = (*(rae_Triangle*)( (char*)(tris->data) + (i) * sizeof(rae_Triangle) ));
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v0.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v0.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v0.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v1.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v1.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v1.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v2.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v2.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.v2.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n0.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n0.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n0.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n1.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n1.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n1.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n2.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n2.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.n2.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.material.albedo.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.material.albedo.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.material.albedo.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, rae_toFloat_rae_View_Int64_(t.material.kind)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.material.fuzz); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, t.material.ior); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  }
  if ((bool)(tris->length == ((int64_t)0LL))) {
  int64_t z = ((int64_t)0LL);
  for (; (bool)(z < ((int64_t)24LL)); ) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, 0.0); rae_string_pool_flush(__rae_spm); }
  z = z + ((int64_t)1LL);
  }
  }
  {
    rae_List_double __ret_val = f;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_SceneGpu rae_buildSceneBvh_rae_List_rae_Sphere_rae_List_rae_Triangle_(rae_List_rae_Sphere* world, rae_List_rae_Triangle* tris) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_rae_PrimRec prims = rae_createList_rae_PrimRec_rae_View_Int64_(world->length + tris->length + ((int64_t)1LL));
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < world->length); ) {
  rae_Sphere s = (*(rae_Sphere*)( (char*)(world->data) + (i) * sizeof(rae_Sphere) ));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_PrimRec_rae_List_rae_PrimRec_rae_PrimRec_(&prims, (rae_PrimRec){ .mnx = s.center.x - s.radius, .mny = s.center.y - s.radius, .mnz = s.center.z - s.radius, .mxx = s.center.x + s.radius, .mxy = s.center.y + s.radius, .mxz = s.center.z + s.radius, .cx = s.center.x, .cy = s.center.y, .cz = s.center.z, .ref = i }); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  }
  int64_t k = ((int64_t)0LL);
  for (; (bool)(k < tris->length); ) {
  rae_Triangle t = (*(rae_Triangle*)( (char*)(tris->data) + (k) * sizeof(rae_Triangle) ));
  { int __rae_spm = rae_string_pool_mark(); rae_add_rae_PrimRec_rae_List_rae_PrimRec_rae_PrimRec_(&prims, (rae_PrimRec){ .mnx = rae_fmin_rae_View_Float64_rae_View_Float64_(t.v0.x, rae_fmin_rae_View_Float64_rae_View_Float64_(t.v1.x, t.v2.x)), .mny = rae_fmin_rae_View_Float64_rae_View_Float64_(t.v0.y, rae_fmin_rae_View_Float64_rae_View_Float64_(t.v1.y, t.v2.y)), .mnz = rae_fmin_rae_View_Float64_rae_View_Float64_(t.v0.z, rae_fmin_rae_View_Float64_rae_View_Float64_(t.v1.z, t.v2.z)), .mxx = rae_fmax_rae_View_Float64_rae_View_Float64_(t.v0.x, rae_fmax_rae_View_Float64_rae_View_Float64_(t.v1.x, t.v2.x)), .mxy = rae_fmax_rae_View_Float64_rae_View_Float64_(t.v0.y, rae_fmax_rae_View_Float64_rae_View_Float64_(t.v1.y, t.v2.y)), .mxz = rae_fmax_rae_View_Float64_rae_View_Float64_(t.v0.z, rae_fmax_rae_View_Float64_rae_View_Float64_(t.v1.z, t.v2.z)), .cx = (t.v0.x + t.v1.x + t.v2.x) / 3.0, .cy = (t.v0.y + t.v1.y + t.v2.y) / 3.0, .cz = (t.v0.z + t.v1.z + t.v2.z) / 3.0, .ref = rae_triBase_() + k }); rae_string_pool_flush(__rae_spm); }
  k = k + ((int64_t)1LL);
  }
  rae_List_double nodes = rae_buildBvh_rae_List_rae_PrimRec_rae_View_Int64_(&prims, ((int64_t)4LL));
  rae_List_double refs = rae_createList_double_rae_View_Int64_(prims.length + ((int64_t)1LL));
  i = ((int64_t)0LL);
  for (; (bool)(i < prims.length); ) {
  rae_PrimRec p = (*(rae_PrimRec*)( (char*)(prims.data) + (i) * sizeof(rae_PrimRec) ));
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&refs, rae_toFloat_rae_View_Int64_(p.ref)); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  }
  if ((bool)(refs.length == ((int64_t)0LL))) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&refs, 0.0); rae_string_pool_flush(__rae_spm); }
  }
  if ((bool)(nodes.length == ((int64_t)0LL))) {
  int64_t z = ((int64_t)0LL);
  for (; (bool)(z < ((int64_t)8LL)); ) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&nodes, 0.0); rae_string_pool_flush(__rae_spm); }
  z = z + ((int64_t)1LL);
  }
  }
  {
    rae_SceneGpu __ret_val = (rae_SceneGpu){ .nodes = (__extension__ ({ rae_List_double __fdc0; rae_deep_copy_rae_List_double(&__fdc0, &(nodes)); __fdc0; })), .refs = (__extension__ ({ rae_List_double __fdc1; rae_deep_copy_rae_List_double(&__fdc1, &(refs)); __fdc1; })) };
  rae_drop_double_rae_List_double_(&refs);
  rae_drop_double_rae_List_double_(&nodes);
  rae_drop_rae_PrimRec_rae_List_rae_PrimRec_(&prims);
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_drop_double_rae_List_double_(&refs);
  rae_drop_double_rae_List_double_(&nodes);
  rae_drop_rae_PrimRec_rae_List_rae_PrimRec_(&prims);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_double rae_boxFloats_rae_List_rae_Box_(rae_List_rae_Box* boxes) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_double f = rae_createList_double_rae_View_Int64_(((int64_t)16LL));
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < boxes->length); ) {
  rae_Box bx = (*(rae_Box*)( (char*)(boxes->data) + (i) * sizeof(rae_Box) ));
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.lo.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.lo.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.lo.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.hi.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.hi.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.hi.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.material.albedo.x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.material.albedo.y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.material.albedo.z); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, rae_toFloat_rae_View_Int64_(bx.material.kind)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.material.fuzz); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, bx.material.ior); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  }
  if ((bool)(boxes->length == ((int64_t)0LL))) {
  int64_t k = ((int64_t)0LL);
  for (; (bool)(k < ((int64_t)12LL)); ) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&f, 0.0); rae_string_pool_flush(__rae_spm); }
  k = k + ((int64_t)1LL);
  }
  }
  {
    rae_List_double __ret_val = f;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_pushVec3_rae_List_double_rae_Vec3_(rae_List_double* buf, rae_Vec3* a) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(buf, a->x); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(buf, a->y); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(buf, a->z); rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_double rae_sceneFloats_rae_Camera_rae_List_rae_Sphere_(rae_Camera* cam, rae_List_rae_Sphere* world) {
  int __rae_spm_func = rae_string_pool_mark();
  rae_List_double s = rae_createList_double_rae_View_Int64_(((int64_t)64LL));
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &cam->origin); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &cam->lowerLeft); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &cam->horizontal); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &cam->vertical); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &cam->right); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &cam->up); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&s, cam->lensRadius); rae_string_pool_flush(__rae_spm); }
  int64_t i = ((int64_t)0LL);
  for (; (bool)(i < world->length); ) {
  rae_Sphere sp = rae_at_rae_Sphere_rae_List_rae_Sphere_rae_View_Int64_(world, i);
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &sp.center); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&s, sp.radius); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_pushVec3_rae_List_double_rae_Vec3_(&s, &sp.material.albedo); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&s, rae_toFloat_rae_View_Int64_(sp.material.kind)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&s, sp.material.fuzz); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_double_rae_List_double_double_(&s, sp.material.ior); rae_string_pool_flush(__rae_spm); }
  i = i + ((int64_t)1LL);
  }
  {
    rae_List_double __ret_val = s;
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_GpuRT rae_buildGpuRT_rae_View_String_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_int64_t_rae_View_Int64_(rae_View_String wgsl, rae_List_double* scene, rae_List_double* boxes, rae_List_double* tris, rae_List_double* nodes, rae_List_double* refs, rae_List_int64_t* params, int64_t pixels) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_gpu_reset(); rae_string_pool_flush(__rae_spm); }
  {
    rae_GpuRT __ret_val = (rae_GpuRT){ .sceneBuf = rae_ext_gpu_storageF32(scene->data, scene->length), .boxBuf = rae_ext_gpu_storageF32(boxes->data, boxes->length), .triBuf = rae_ext_gpu_storageF32(tris->data, tris->length), .bvhBuf = rae_ext_gpu_storageF32(nodes->data, nodes->length), .refBuf = rae_ext_gpu_storageF32(refs->data, refs->length), .accumBuf = rae_ext_gpu_allocF32(pixels * ((int64_t)4LL)), .outBuf = rae_ext_gpu_allocU32(pixels), .paramsBuf = rae_ext_gpu_uniformU32(params->data, ((int64_t)12LL)), .kernel = (__extension__ ({ rae_String __rae_pw_0 = (rae_String){(uint8_t*)"main", 4}; rae_ext_gpu_kernel(wgsl, (rae_View_String){ .ptr = &__rae_pw_0 }); })) };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drawLabel_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_(int64_t* fb, int64_t w, int64_t h, rae_SdfFont* font, rae_View_String text, double x, double y, double px) {
  int __rae_spm_func = rae_string_pool_mark();
  double sh = px * 0.06 + 1.0;
  { int __rae_spm = rae_string_pool_mark(); rae_sdfDrawText_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Int64_(fb, w, h, font, text, x + sh, y + sh, px, ((int64_t)0LL)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_sdfDrawText_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Int64_(fb, w, h, font, text, x, y, px, ((int64_t)16777215LL)); rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_String rae_createList_rae_String_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_String_rae_List_rae_String_(rae_List_rae_String* this);
RAE_UNUSED static void rae_add_rae_String_rae_List_rae_String_rae_String_(rae_List_rae_String* this, rae_String value);
RAE_UNUSED static RaeAny rae_get_rae_String_rae_List_rae_String_rae_View_Int64_(rae_List_rae_String* this, int64_t index);
RAE_UNUSED static void rae_add_rae_JsonValue_rae_List_rae_JsonValue_rae_JsonValue_(rae_List_rae_JsonValue* this, rae_JsonValue value);
RAE_UNUSED static void rae_add_int64_t_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value);
RAE_UNUSED static void rae_add_rae_JsonField_rae_List_rae_JsonField_rae_JsonField_(rae_List_rae_JsonField* this, rae_JsonField value);
RAE_UNUSED static rae_List_rae_JsonField rae_createList_rae_JsonField_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_JsonField_rae_List_rae_JsonField_(rae_List_rae_JsonField* this);
RAE_UNUSED static rae_List_int64_t rae_createList_int64_t_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_int64_t_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static rae_List_rae_JsonValue rae_createList_rae_JsonValue_rae_View_Int64_(int64_t cap);
RAE_UNUSED static rae_List_rae_SdfGlyph rae_createList_rae_SdfGlyph_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_SdfGlyph_rae_List_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this);
RAE_UNUSED static void rae_add_rae_SdfGlyph_rae_List_rae_SdfGlyph_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this, rae_SdfGlyph value);
RAE_UNUSED static rae_List_double rae_createList_double_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_double_rae_List_double_(rae_List_double* this);
RAE_UNUSED static void rae_add_double_rae_List_double_double_(rae_List_double* this, double value);
RAE_UNUSED static rae_List_rae_Triangle rae_createList_rae_Triangle_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_Triangle_rae_List_rae_Triangle_(rae_List_rae_Triangle* this);
RAE_UNUSED static double rae_at_double_rae_List_double_rae_View_Int64_(rae_List_double* this, int64_t index);
RAE_UNUSED static void rae_add_rae_Triangle_rae_List_rae_Triangle_rae_Triangle_(rae_List_rae_Triangle* this, rae_Triangle value);
RAE_UNUSED static rae_List_rae_Box rae_createList_rae_Box_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_Box_rae_List_rae_Box_(rae_List_rae_Box* this);
RAE_UNUSED static void rae_add_rae_Box_rae_List_rae_Box_rae_Box_(rae_List_rae_Box* this, rae_Box value);
RAE_UNUSED static rae_List_rae_Sphere rae_createList_rae_Sphere_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_Sphere_rae_List_rae_Sphere_(rae_List_rae_Sphere* this);
RAE_UNUSED static void rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(rae_List_rae_Sphere* this, rae_Sphere value);
RAE_UNUSED static void rae_set_double_rae_List_double_rae_View_Int64_double_(rae_List_double* this, int64_t index, double value);
RAE_UNUSED static void rae_set_rae_PrimRec_rae_List_rae_PrimRec_rae_View_Int64_rae_PrimRec_(rae_List_rae_PrimRec* this, int64_t index, rae_PrimRec value);
RAE_UNUSED static rae_List_rae_PrimRec rae_createList_rae_PrimRec_rae_View_Int64_(int64_t cap);
RAE_UNUSED static void rae_drop_rae_PrimRec_rae_List_rae_PrimRec_(rae_List_rae_PrimRec* this);
RAE_UNUSED static void rae_add_rae_PrimRec_rae_List_rae_PrimRec_rae_PrimRec_(rae_List_rae_PrimRec* this, rae_PrimRec value);
RAE_UNUSED static rae_Sphere rae_at_rae_Sphere_rae_List_rae_Sphere_rae_View_Int64_(rae_List_rae_Sphere* this, int64_t index);
RAE_UNUSED static void rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(rae_List_int64_t* this, int64_t index, int64_t value);
RAE_UNUSED static void rae_grow_rae_String_rae_List_rae_String_(rae_List_rae_String* this);
RAE_UNUSED static void rae_grow_rae_JsonValue_rae_List_rae_JsonValue_(rae_List_rae_JsonValue* this);
RAE_UNUSED static void rae_grow_int64_t_rae_List_int64_t_(rae_List_int64_t* this);
RAE_UNUSED static void rae_grow_rae_JsonField_rae_List_rae_JsonField_(rae_List_rae_JsonField* this);
RAE_UNUSED static void rae_grow_rae_SdfGlyph_rae_List_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this);
RAE_UNUSED static void rae_grow_double_rae_List_double_(rae_List_double* this);
RAE_UNUSED static void rae_grow_rae_Triangle_rae_List_rae_Triangle_(rae_List_rae_Triangle* this);
RAE_UNUSED static void rae_grow_rae_Box_rae_List_rae_Box_(rae_List_rae_Box* this);
RAE_UNUSED static void rae_grow_rae_Sphere_rae_List_rae_Sphere_(rae_List_rae_Sphere* this);
RAE_UNUSED static void rae_grow_rae_PrimRec_rae_List_rae_PrimRec_(rae_List_rae_PrimRec* this);
RAE_UNUSED static void rae_drop_rae_JsonValue_rae_List_rae_JsonValue_(rae_List_rae_JsonValue* this);
RAE_UNUSED static rae_List_rae_String rae_createList_rae_String_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_String __ret_val = (rae_List_rae_String){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_String)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_String_rae_List_rae_String_(rae_List_rae_String* this) {
  for (int64_t __i = 0; __i < this->length; __i++) {
    rae_String* __elem = (rae_String*)((char*)this->data + __i * sizeof(rae_String));
    rae_ext_rae_str_free(*__elem);
  }
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_String_rae_List_rae_String_rae_String_(rae_List_rae_String* this, rae_String value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_String_rae_List_rae_String_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_String*)( (char*)(this->data) + (this->length) * sizeof(rae_String) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static RaeAny rae_get_rae_String_rae_List_rae_String_rae_View_Int64_(rae_List_rae_String* this, int64_t index) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)((bool)(index < ((int64_t)0LL)) || (bool)(index >= this->length))) {
  {
    RaeAny __ret_val = rae_any((rae_any_none()));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  }
  {
    RaeAny __ret_val = rae_any(((*(rae_String*)( (char*)(this->data) + (index) * sizeof(rae_String) ))));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_JsonValue_rae_List_rae_JsonValue_rae_JsonValue_(rae_List_rae_JsonValue* this, rae_JsonValue value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_JsonValue_rae_List_rae_JsonValue_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_JsonValue*)( (char*)(this->data) + (this->length) * sizeof(rae_JsonValue) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_int64_t_rae_List_int64_t_int64_t_(rae_List_int64_t* this, int64_t value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_int64_t_rae_List_int64_t_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(int64_t*)( (char*)(this->data) + (this->length) * sizeof(int64_t) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_JsonField_rae_List_rae_JsonField_rae_JsonField_(rae_List_rae_JsonField* this, rae_JsonField value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_JsonField_rae_List_rae_JsonField_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_JsonField*)( (char*)(this->data) + (this->length) * sizeof(rae_JsonField) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_JsonField rae_createList_rae_JsonField_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_JsonField __ret_val = (rae_List_rae_JsonField){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_JsonField)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_JsonField_rae_List_rae_JsonField_(rae_List_rae_JsonField* this) {
  for (int64_t __i = 0; __i < this->length; __i++) {
    rae_JsonField* __elem = (rae_JsonField*)((char*)this->data + __i * sizeof(rae_JsonField));
    rae_drop_struct_rae_JsonField(__elem);
  }
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_int64_t rae_createList_int64_t_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_int64_t __ret_val = (rae_List_int64_t){ .data = rae_ext_rae_buf_alloc(cap, sizeof(int64_t)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_int64_t_rae_List_int64_t_(rae_List_int64_t* this) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_JsonValue rae_createList_rae_JsonValue_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_JsonValue __ret_val = (rae_List_rae_JsonValue){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_JsonValue)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_SdfGlyph rae_createList_rae_SdfGlyph_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_SdfGlyph __ret_val = (rae_List_rae_SdfGlyph){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_SdfGlyph)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_SdfGlyph_rae_List_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_SdfGlyph_rae_List_rae_SdfGlyph_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this, rae_SdfGlyph value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_SdfGlyph_rae_List_rae_SdfGlyph_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_SdfGlyph*)( (char*)(this->data) + (this->length) * sizeof(rae_SdfGlyph) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_double rae_createList_double_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_double __ret_val = (rae_List_double){ .data = rae_ext_rae_buf_alloc(cap, sizeof(double)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_double_rae_List_double_(rae_List_double* this) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_double_rae_List_double_double_(rae_List_double* this, double value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_double_rae_List_double_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(double*)( (char*)(this->data) + (this->length) * sizeof(double) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Triangle rae_createList_rae_Triangle_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_Triangle __ret_val = (rae_List_rae_Triangle){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_Triangle)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_Triangle_rae_List_rae_Triangle_(rae_List_rae_Triangle* this) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static double rae_at_double_rae_List_double_rae_View_Int64_(rae_List_double* this, int64_t index) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    double __ret_val = (*(double*)( (char*)(this->data) + (index) * sizeof(double) ));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_Triangle_rae_List_rae_Triangle_rae_Triangle_(rae_List_rae_Triangle* this, rae_Triangle value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_Triangle_rae_List_rae_Triangle_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_Triangle*)( (char*)(this->data) + (this->length) * sizeof(rae_Triangle) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Box rae_createList_rae_Box_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_Box __ret_val = (rae_List_rae_Box){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_Box)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_Box_rae_List_rae_Box_(rae_List_rae_Box* this) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_Box_rae_List_rae_Box_rae_Box_(rae_List_rae_Box* this, rae_Box value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_Box_rae_List_rae_Box_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_Box*)( (char*)(this->data) + (this->length) * sizeof(rae_Box) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_Sphere rae_createList_rae_Sphere_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_Sphere __ret_val = (rae_List_rae_Sphere){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_Sphere)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_Sphere_rae_List_rae_Sphere_(rae_List_rae_Sphere* this) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_Sphere_rae_List_rae_Sphere_rae_Sphere_(rae_List_rae_Sphere* this, rae_Sphere value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_Sphere_rae_List_rae_Sphere_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_Sphere*)( (char*)(this->data) + (this->length) * sizeof(rae_Sphere) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_set_double_rae_List_double_rae_View_Int64_double_(rae_List_double* this, int64_t index, double value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)((bool)(index < ((int64_t)0LL)) || (bool)(index >= this->length))) {
  {
    rae_string_pool_flush(__rae_spm_func);
    return;
  }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(double*)( (char*)(this->data) + (index) * sizeof(double) )) = value; rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_set_rae_PrimRec_rae_List_rae_PrimRec_rae_View_Int64_rae_PrimRec_(rae_List_rae_PrimRec* this, int64_t index, rae_PrimRec value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)((bool)(index < ((int64_t)0LL)) || (bool)(index >= this->length))) {
  {
    rae_string_pool_flush(__rae_spm_func);
    return;
  }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_PrimRec*)( (char*)(this->data) + (index) * sizeof(rae_PrimRec) )) = value; rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_List_rae_PrimRec rae_createList_rae_PrimRec_rae_View_Int64_(int64_t cap) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_List_rae_PrimRec __ret_val = (rae_List_rae_PrimRec){ .data = rae_ext_rae_buf_alloc(cap, sizeof(rae_PrimRec)), .length = ((int64_t)0LL), .cap = cap };
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_PrimRec_rae_List_rae_PrimRec_(rae_List_rae_PrimRec* this) {
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_add_rae_PrimRec_rae_List_rae_PrimRec_rae_PrimRec_(rae_List_rae_PrimRec* this, rae_PrimRec value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)(this->length == this->cap)) {
  { int __rae_spm = rae_string_pool_mark(); rae_grow_rae_PrimRec_rae_List_rae_PrimRec_(this); rae_string_pool_flush(__rae_spm); }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(rae_PrimRec*)( (char*)(this->data) + (this->length) * sizeof(rae_PrimRec) )) = value; rae_string_pool_flush(__rae_spm); }
  this->length = this->length + ((int64_t)1LL);
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static rae_Sphere rae_at_rae_Sphere_rae_List_rae_Sphere_rae_View_Int64_(rae_List_rae_Sphere* this, int64_t index) {
  int __rae_spm_func = rae_string_pool_mark();
  {
    rae_Sphere __ret_val = (*(rae_Sphere*)( (char*)(this->data) + (index) * sizeof(rae_Sphere) ));
    rae_string_pool_flush(__rae_spm_func);
    return __ret_val;
  }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(rae_List_int64_t* this, int64_t index, int64_t value) {
  int __rae_spm_func = rae_string_pool_mark();
  if ((bool)((bool)(index < ((int64_t)0LL)) || (bool)(index >= this->length))) {
  {
    rae_string_pool_flush(__rae_spm_func);
    return;
  }
  }
  { int __rae_spm = rae_string_pool_mark(); (*(int64_t*)( (char*)(this->data) + (index) * sizeof(int64_t) )) = value; rae_string_pool_flush(__rae_spm); }
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_String_rae_List_rae_String_(rae_List_rae_String* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_String* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_String));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_String), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_String), (this->length) * sizeof(rae_String)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_JsonValue_rae_List_rae_JsonValue_(rae_List_rae_JsonValue* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_JsonValue* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_JsonValue));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_JsonValue), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_JsonValue), (this->length) * sizeof(rae_JsonValue)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_int64_t_rae_List_int64_t_(rae_List_int64_t* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  int64_t* newData = rae_ext_rae_buf_alloc(newCap, sizeof(int64_t));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(int64_t), (char*)(this->data) + (((int64_t)0LL)) * sizeof(int64_t), (this->length) * sizeof(int64_t)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_JsonField_rae_List_rae_JsonField_(rae_List_rae_JsonField* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_JsonField* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_JsonField));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_JsonField), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_JsonField), (this->length) * sizeof(rae_JsonField)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_SdfGlyph_rae_List_rae_SdfGlyph_(rae_List_rae_SdfGlyph* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_SdfGlyph* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_SdfGlyph));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_SdfGlyph), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_SdfGlyph), (this->length) * sizeof(rae_SdfGlyph)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_double_rae_List_double_(rae_List_double* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  double* newData = rae_ext_rae_buf_alloc(newCap, sizeof(double));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(double), (char*)(this->data) + (((int64_t)0LL)) * sizeof(double), (this->length) * sizeof(double)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_Triangle_rae_List_rae_Triangle_(rae_List_rae_Triangle* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_Triangle* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_Triangle));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_Triangle), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_Triangle), (this->length) * sizeof(rae_Triangle)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_Box_rae_List_rae_Box_(rae_List_rae_Box* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_Box* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_Box));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_Box), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_Box), (this->length) * sizeof(rae_Box)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_Sphere_rae_List_rae_Sphere_(rae_List_rae_Sphere* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_Sphere* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_Sphere));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_Sphere), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_Sphere), (this->length) * sizeof(rae_Sphere)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_grow_rae_PrimRec_rae_List_rae_PrimRec_(rae_List_rae_PrimRec* this) {
  int __rae_spm_func = rae_string_pool_mark();
  int64_t newCap = this->cap * ((int64_t)2LL);
  if ((bool)(newCap == ((int64_t)0LL))) {
  newCap = ((int64_t)4LL);
  }
  rae_PrimRec* newData = rae_ext_rae_buf_alloc(newCap, sizeof(rae_PrimRec));
  { int __rae_spm = rae_string_pool_mark(); memmove((char*)(newData) + (((int64_t)0LL)) * sizeof(rae_PrimRec), (char*)(this->data) + (((int64_t)0LL)) * sizeof(rae_PrimRec), (this->length) * sizeof(rae_PrimRec)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->data = newData;
  this->cap = newCap;
  rae_string_pool_flush(__rae_spm_func);
}

RAE_UNUSED static void rae_drop_rae_JsonValue_rae_List_rae_JsonValue_(rae_List_rae_JsonValue* this) {
  for (int64_t __i = 0; __i < this->length; __i++) {
    rae_JsonValue* __elem = (rae_JsonValue*)((char*)this->data + __i * sizeof(rae_JsonValue));
    rae_drop_struct_rae_JsonValue(__elem);
  }
  int __rae_spm_func = rae_string_pool_mark();
  { int __rae_spm = rae_string_pool_mark(); rae_ext_rae_buf_free(this->data); rae_string_pool_flush(__rae_spm); }
  this->length = ((int64_t)0LL);
  this->cap = ((int64_t)0LL);
  rae_string_pool_flush(__rae_spm_func);
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  int __rae_spm_func = rae_string_pool_mark();
  int64_t maxDepth = ((int64_t)8LL);
  int64_t sceneId = ((int64_t)1LL);
  rae_List_rae_Sphere world = rae_buildSceneOne_();
  rae_List_rae_Box boxes = rae_buildSceneOneBoxes_();
  rae_List_rae_Triangle tris = rae_createList_rae_Triangle_rae_View_Int64_(((int64_t)0LL));
  int64_t skyMode = ((int64_t)1LL);
  rae_Vec3 camPos = rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(0.0, -(1.6), 0.6);
  double yaw = pi / 2.0;
  double pitch = -(0.15);
  double moveStep = 0.08;
  double rotStep = 0.03;
  double mouseSens = 0.005;
  rae_Vec3 worldUp = rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(0.0, 0.0, 1.0);
  rae_Vec3 fwd = rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(rae_ext_math_cos(pitch) * rae_ext_math_cos(yaw), rae_ext_math_cos(pitch) * rae_ext_math_sin(yaw), rae_ext_math_sin(pitch));
  rae_String dir = rae_string_pool_take((rae_String){(uint8_t*)"examples/53_raytracer_webgpu_text/", 34});
  dir.is_owned = 0;
  rae_String wgsl = rae_string_pool_take(rae_ext_rae_sys_read_file(rae_ext_rae_str_interp(3, (rae_String){(uint8_t*)"", 0}, rae_string_borrow(dir), (rae_String){(uint8_t*)"accum.wgsl", 10})));
  rae_SdfFont font = (__extension__ ({ rae_String __rae_pw_0 = rae_ext_rae_str_interp(3, (rae_String){(uint8_t*)"", 0}, rae_string_borrow(dir), (rae_String){(uint8_t*)"assets/Roboto-Regular.mtsdf.json", 32}); rae_String __rae_pw_1 = rae_ext_rae_str_interp(3, (rae_String){(uint8_t*)"", 0}, rae_string_borrow(dir), (rae_String){(uint8_t*)"assets/Roboto-Regular.mtsdf.raw", 31}); rae_loadSdfFont_rae_View_String_rae_View_String_((rae_View_String){ .ptr = &__rae_pw_0 }, (rae_View_String){ .ptr = &__rae_pw_1 }); }));
  { int __rae_spm = rae_string_pool_mark(); rae_ext_sdl3_initWindow(((int64_t)1280LL), ((int64_t)720LL), (rae_String){(uint8_t*)"Rae raytracer \xe2\x80\x94 interactive GPU + MTSDF text", 46}); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_sdl3_setTargetFPS(((int64_t)0LL)); rae_string_pool_flush(__rae_spm); }
  double aspect = 16.0 / 9.0;
  rae_Camera cam = rae_buildCamera_rae_Vec3_rae_Vec3_rae_View_Float64_rae_View_Float64_rae_View_Float64_(&camPos, ((rae_Vec3[1]){ rae_vadd_rae_Vec3_rae_Vec3_(&camPos, &fwd) }), 50.0, 0.0, aspect);
  rae_Bool hiRes = (bool)true;
  int64_t width = ((int64_t)1920LL);
  int64_t height = ((int64_t)1080LL);
  rae_List_int64_t params = rae_createList_int64_t_rae_View_Int64_(((int64_t)12LL));
  int64_t pinit = ((int64_t)0LL);
  for (; (bool)(pinit < ((int64_t)12LL)); ) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&params, ((int64_t)0LL)); rae_string_pool_flush(__rae_spm); }
  pinit = pinit + ((int64_t)1LL);
  }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)0LL), width); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)1LL), height); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)3LL), maxDepth); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)4LL), world.length); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)7LL), skyMode); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)8LL), boxes.length); rae_string_pool_flush(__rae_spm); }
  rae_SceneGpu sceneGpu = rae_buildSceneBvh_rae_List_rae_Sphere_rae_List_rae_Triangle_(&world, &tris);
  rae_GpuRT rt = (__extension__ ({ rae_String __rae_pw_2 = wgsl; rae_buildGpuRT_rae_View_String_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_int64_t_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_2 }, ((rae_List_double[1]){ rae_sceneFloats_rae_Camera_rae_List_rae_Sphere_(&cam, &world) }), ((rae_List_double[1]){ rae_boxFloats_rae_List_rae_Box_(&boxes) }), ((rae_List_double[1]){ rae_triFloats_rae_List_rae_Triangle_(&tris) }), &sceneGpu.nodes, &sceneGpu.refs, &params, width * height); }));
  rae_List_int64_t fb = rae_createList_int64_t_rae_View_Int64_(width * height);
  int64_t fbi = ((int64_t)0LL);
  for (; (bool)(fbi < width * height); ) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&fb, ((int64_t)0LL)); rae_string_pool_flush(__rae_spm); }
  fbi = fbi + ((int64_t)1LL);
  }
  int64_t sampleIndex = ((int64_t)0LL);
  int64_t frame = ((int64_t)0LL);
  int64_t prevMouseX = rae_ext_sdl3_getMouseX();
  int64_t prevMouseY = rae_ext_sdl3_getMouseY();
  int64_t lastMs = rae_ext_nowMs();
  double fps = 0.0;
  int64_t spp = ((int64_t)1LL);
  rae_String helper = rae_string_pool_take((rae_String){(uint8_t*)"WASD/E-Q move   arrows/drag look   1-6 scene   P quality   F2 save PNG", 70});
  helper.is_owned = 0;
  for (; ((bool)!(rae_ext_sdl3_shouldClose())); ) {
  rae_Bool moved = (bool)false;
  if (rae_ext_sdl3_isKeyPressed(((int64_t)291LL))) {
  saveRequested = (bool)true;
  }
  int64_t newScene = sceneId;
  if (rae_ext_sdl3_isKeyPressed(((int64_t)49LL))) {
  newScene = ((int64_t)1LL);
  }
  if (rae_ext_sdl3_isKeyPressed(((int64_t)50LL))) {
  newScene = ((int64_t)2LL);
  }
  if (rae_ext_sdl3_isKeyPressed(((int64_t)51LL))) {
  newScene = ((int64_t)3LL);
  }
  if (rae_ext_sdl3_isKeyPressed(((int64_t)52LL))) {
  newScene = ((int64_t)4LL);
  }
  if (rae_ext_sdl3_isKeyPressed(((int64_t)53LL))) {
  newScene = ((int64_t)5LL);
  }
  if (rae_ext_sdl3_isKeyPressed(((int64_t)54LL))) {
  newScene = ((int64_t)6LL);
  }
  if ((bool)((bool)(newScene > sceneId) || (bool)(newScene < sceneId))) {
  sceneId = newScene;
  boxes = rae_createList_rae_Box_rae_View_Int64_(((int64_t)0LL));
  tris = rae_createList_rae_Triangle_rae_View_Int64_(((int64_t)0LL));
  if ((bool)(sceneId == ((int64_t)1LL))) {
  world = rae_buildSceneOne_();
  boxes = rae_buildSceneOneBoxes_();
  skyMode = ((int64_t)1LL);
  } else {
  if ((bool)(sceneId == ((int64_t)2LL))) {
  world = rae_buildSceneBook_();
  skyMode = ((int64_t)0LL);
  } else {
  if ((bool)(sceneId == ((int64_t)3LL))) {
  world = rae_buildSceneOne_();
  skyMode = ((int64_t)1LL);
  rae_Material bunnyMat = (rae_Material){ .kind = ((int64_t)0LL), .albedo = (rae_Vec3){ .x = 0.8, .y = 0.3, .z = 0.3 }, .fuzz = 0.0, .ior = 0.0 };
  tris = (__extension__ ({ rae_String __rae_pw_3 = rae_ext_rae_str_interp(3, (rae_String){(uint8_t*)"", 0}, rae_string_borrow(dir), (rae_String){(uint8_t*)"assets/bunny.obj", 16}); rae_loadObjTriangles_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Float64_rae_View_Bool_rae_Material_((rae_View_String){ .ptr = &__rae_pw_3 }, 4.0, 0.0, 3.6, 0.9, (bool)true, &bunnyMat); }));
  } else {
  if ((bool)(sceneId == ((int64_t)4LL))) {
  world = rae_buildSceneDiffuse_();
  skyMode = ((int64_t)0LL);
  } else {
  if ((bool)(sceneId == ((int64_t)5LL))) {
  world = rae_buildScene_();
  skyMode = ((int64_t)0LL);
  } else {
  world = rae_buildSceneLights_();
  skyMode = ((int64_t)1LL);
  }
  }
  }
  }
  }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)4LL), world.length); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)7LL), skyMode); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)8LL), boxes.length); rae_string_pool_flush(__rae_spm); }
  sceneGpu = rae_buildSceneBvh_rae_List_rae_Sphere_rae_List_rae_Triangle_(&world, &tris);
  rt = (__extension__ ({ rae_String __rae_pw_4 = wgsl; rae_buildGpuRT_rae_View_String_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_int64_t_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_4 }, ((rae_List_double[1]){ rae_sceneFloats_rae_Camera_rae_List_rae_Sphere_(&cam, &world) }), ((rae_List_double[1]){ rae_boxFloats_rae_List_rae_Box_(&boxes) }), ((rae_List_double[1]){ rae_triFloats_rae_List_rae_Triangle_(&tris) }), &sceneGpu.nodes, &sceneGpu.refs, &params, width * height); }));
  sampleIndex = ((int64_t)0LL);
  spp = ((int64_t)1LL);
  }
  if (rae_ext_sdl3_isKeyPressed(((int64_t)80LL))) {
  hiRes = ((bool)!(hiRes));
  if (hiRes) {
  width = ((int64_t)1920LL);
  height = ((int64_t)1080LL);
  } else {
  width = ((int64_t)960LL);
  height = ((int64_t)540LL);
  }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)0LL), width); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)1LL), height); rae_string_pool_flush(__rae_spm); }
  rt = (__extension__ ({ rae_String __rae_pw_5 = wgsl; rae_buildGpuRT_rae_View_String_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_double_rae_List_int64_t_rae_View_Int64_((rae_View_String){ .ptr = &__rae_pw_5 }, ((rae_List_double[1]){ rae_sceneFloats_rae_Camera_rae_List_rae_Sphere_(&cam, &world) }), ((rae_List_double[1]){ rae_boxFloats_rae_List_rae_Box_(&boxes) }), ((rae_List_double[1]){ rae_triFloats_rae_List_rae_Triangle_(&tris) }), &sceneGpu.nodes, &sceneGpu.refs, &params, width * height); }));
  fb = rae_createList_int64_t_rae_View_Int64_(width * height);
  fbi = ((int64_t)0LL);
  for (; (bool)(fbi < width * height); ) {
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&fb, ((int64_t)0LL)); rae_string_pool_flush(__rae_spm); }
  fbi = fbi + ((int64_t)1LL);
  }
  sampleIndex = ((int64_t)0LL);
  spp = ((int64_t)1LL);
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)87LL))) {
  camPos = rae_vadd_rae_Vec3_rae_Vec3_(&camPos, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&fwd, moveStep) }));
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)83LL))) {
  camPos = rae_vsub_rae_Vec3_rae_Vec3_(&camPos, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&fwd, moveStep) }));
  moved = (bool)true;
  }
  rae_Vec3 rgt = rae_vnorm_rae_Vec3_(((rae_Vec3[1]){ rae_vcross_rae_Vec3_rae_Vec3_(&fwd, &worldUp) }));
  if (rae_ext_sdl3_isKeyDown(((int64_t)68LL))) {
  camPos = rae_vadd_rae_Vec3_rae_Vec3_(&camPos, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&rgt, moveStep) }));
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)65LL))) {
  camPos = rae_vsub_rae_Vec3_rae_Vec3_(&camPos, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&rgt, moveStep) }));
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)69LL))) {
  camPos = rae_vadd_rae_Vec3_rae_Vec3_(&camPos, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&worldUp, moveStep) }));
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)81LL))) {
  camPos = rae_vsub_rae_Vec3_rae_Vec3_(&camPos, ((rae_Vec3[1]){ rae_vscale_rae_Vec3_rae_View_Float64_(&worldUp, moveStep) }));
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)262LL))) {
  yaw = yaw - rotStep;
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)263LL))) {
  yaw = yaw + rotStep;
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)265LL))) {
  pitch = pitch + rotStep;
  moved = (bool)true;
  }
  if (rae_ext_sdl3_isKeyDown(((int64_t)264LL))) {
  pitch = pitch - rotStep;
  moved = (bool)true;
  }
  int64_t mx = rae_ext_sdl3_getMouseX();
  int64_t my = rae_ext_sdl3_getMouseY();
  if (rae_ext_sdl3_isMouseButtonDown(((int64_t)0LL))) {
  int64_t dx = mx - prevMouseX;
  int64_t dy = my - prevMouseY;
  if ((bool)((bool)(dx > ((int64_t)0LL)) || (bool)(dx < ((int64_t)0LL)))) {
  yaw = yaw - rae_toFloat_rae_View_Int64_(dx) * mouseSens;
  moved = (bool)true;
  }
  if ((bool)((bool)(dy > ((int64_t)0LL)) || (bool)(dy < ((int64_t)0LL)))) {
  pitch = pitch - rae_toFloat_rae_View_Int64_(dy) * mouseSens;
  moved = (bool)true;
  }
  }
  prevMouseX = mx;
  prevMouseY = my;
  if (moved) {
  if ((bool)(pitch > 1.5)) {
  pitch = 1.5;
  }
  if ((bool)(pitch < -(1.5))) {
  pitch = -(1.5);
  }
  fwd = rae_create_rae_View_Float64_rae_View_Float64_rae_View_Float64_(rae_ext_math_cos(pitch) * rae_ext_math_cos(yaw), rae_ext_math_cos(pitch) * rae_ext_math_sin(yaw), rae_ext_math_sin(pitch));
  cam = rae_buildCamera_rae_Vec3_rae_Vec3_rae_View_Float64_rae_View_Float64_rae_View_Float64_(&camPos, ((rae_Vec3[1]){ rae_vadd_rae_Vec3_rae_Vec3_(&camPos, &fwd) }), 50.0, 0.0, aspect);
  { int __rae_spm = rae_string_pool_mark(); rae_ext_gpu_writeF32(rt.sceneBuf, rae_sceneFloats_rae_Camera_rae_List_rae_Sphere_(&cam, &world).data, ((int64_t)19LL) + world.length * ((int64_t)10LL)); rae_string_pool_flush(__rae_spm); }
  sampleIndex = ((int64_t)0LL);
  }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)2LL), sampleIndex); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)5LL), frame); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_set_int64_t_rae_List_int64_t_rae_View_Int64_int64_t_(&params, ((int64_t)6LL), spp); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_gpu_writeU32(rt.paramsBuf, params.data, ((int64_t)8LL)); rae_string_pool_flush(__rae_spm); }
  rae_List_int64_t bufs = rae_createList_int64_t_rae_View_Int64_(((int64_t)8LL));
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.paramsBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.sceneBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.accumBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.outBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.boxBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.bvhBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.refBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_add_int64_t_rae_List_int64_t_int64_t_(&bufs, rt.triBuf); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_gpu_run(rt.kernel, bufs.data, ((int64_t)8LL), (width + ((int64_t)7LL)) / ((int64_t)8LL), (height + ((int64_t)7LL)) / ((int64_t)8LL), ((int64_t)1LL)); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_gpu_downloadU32(rt.outBuf, fb.data, width * height); rae_string_pool_flush(__rae_spm); }
  int64_t now = rae_ext_nowMs();
  int64_t dt = now - lastMs;
  lastMs = now;
  if ((bool)(dt > ((int64_t)0LL))) {
  double inst = 1000.0 / rae_toFloat_rae_View_Int64_(dt);
  fps = fps * 0.9 + inst * 0.1;
  }
  if ((bool)(fps > 70.0)) {
  spp = spp + ((int64_t)1LL);
  } else {
  if ((bool)((bool)(fps < 50.0) && (bool)(spp > ((int64_t)1LL)))) {
  spp = spp - ((int64_t)1LL);
  }
  }
  double textPx = rae_toFloat_rae_View_Int64_(height) * 0.030;
  double baseY = textPx + 8.0;
  { int __rae_spm = rae_string_pool_mark(); (__extension__ ({ rae_String __rae_pw_6 = helper; rae_drawLabel_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_(fb.data, width, height, &font, (rae_View_String){ .ptr = &__rae_pw_6 }, 12.0, baseY, textPx); })); rae_string_pool_flush(__rae_spm); }
  int64_t totalSamples = sampleIndex + spp;
  rae_String fpsStr = rae_string_pool_take(rae_ext_rae_str_interp(7, (rae_String){(uint8_t*)"", 0}, rae_ext_rae_str((rae_toInt_rae_View_Float64_(fps))), (rae_String){(uint8_t*)" FPS   ", 7}, rae_ext_rae_str((spp)), (rae_String){(uint8_t*)" spp/f   ", 9}, rae_ext_rae_str((totalSamples)), (rae_String){(uint8_t*)" samples", 8}));
  double fpsW = (__extension__ ({ rae_String __rae_pw_7 = fpsStr; rae_sdfMeasureText_rae_SdfFont_rae_View_String_rae_View_Float64_(&font, (rae_View_String){ .ptr = &__rae_pw_7 }, textPx); }));
  { int __rae_spm = rae_string_pool_mark(); (__extension__ ({ rae_String __rae_pw_8 = fpsStr; rae_drawLabel_rae_Buffer_int64_t_rae_View_Int64_rae_View_Int64_rae_SdfFont_rae_View_String_rae_View_Float64_rae_View_Float64_rae_View_Float64_(fb.data, width, height, &font, (rae_View_String){ .ptr = &__rae_pw_8 }, rae_toFloat_rae_View_Int64_(width) - fpsW - 12.0, baseY, textPx); })); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_sdl3_updatePixels(fb.data, width, height); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_sdl3_present(); rae_string_pool_flush(__rae_spm); }
  sampleIndex = sampleIndex + spp;
  frame = frame + ((int64_t)1LL);
  rae_string_drop(&fpsStr);
  rae_drop_int64_t_rae_List_int64_t_(&bufs);
  }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_gpu_reset(); rae_string_pool_flush(__rae_spm); }
  { int __rae_spm = rae_string_pool_mark(); rae_ext_sdl3_closeWindow(); rae_string_pool_flush(__rae_spm); }
  rae_drop_int64_t_rae_List_int64_t_(&fb);
  rae_drop_struct_rae_SceneGpu(&sceneGpu);
  rae_drop_int64_t_rae_List_int64_t_(&params);
  rae_drop_struct_rae_SdfFont(&font);
  rae_string_drop(&wgsl);
  rae_drop_rae_Triangle_rae_List_rae_Triangle_(&tris);
  rae_drop_rae_Box_rae_List_rae_Box_(&boxes);
  rae_drop_rae_Sphere_rae_List_rae_Sphere_(&world);
  rae_string_pool_flush(__rae_spm_func);
  return 0;
}

