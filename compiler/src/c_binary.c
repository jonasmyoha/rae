/* The derived binary encoding (docs/wasm-interfaces.md §2, §3).
 *
 * Every struct whose fields can all cross gets, when a program calls
 * `value.toBinary()` or `T.fromBinary(bytes:)` on it:
 *
 *   rae_binPut_<T>_(out, value)          append the encoding
 *   rae_binGet_<T>_(in, value, depth)    read it, bounds-checked; 0 = malformed
 *   rae_binFree_<T>_(value)              free what a failed read built
 *   rae_toBinary_<T>_ / rae_fromBinary_<T>_   the two calls the program makes
 *
 * The format: little-endian, no padding, no header, fields in declaration
 * order. Integers in their own width (Int 8 bytes), Float f32, Float64 f64,
 * Bool 1 byte (0 or 1 only), an enum its member index as a UInt32, a String a
 * UInt32 byte count and UTF-8 (checked), a List a UInt32 count and each
 * element, an opt one byte (0 none, 1 value) and the value.
 *
 * The reader trusts nothing: every read is checked against the bytes left, a
 * count is checked against the smallest encoding of its elements before
 * anything is allocated, and the input must be used up exactly. A value that
 * does not decode frees what was built and answers none.
 *
 * The byte helpers are generated C, like the JSON writer's: they are the
 * compiler's output for one program, not runtime C. */
#include <stdlib.h>
#include <string.h>

#include "c_backend_internal.h"
#include "c_binary.h"
#include "mangler.h"
#include "sema.h"

typedef enum { BK_BAD, BK_INT, BK_FLOAT32, BK_FLOAT64, BK_BOOL, BK_STRING, BK_ENUM, BK_LIST, BK_OPT, BK_STRUCT } BinKind;

typedef struct {
  BinKind kind;
  int bytes;                  /* an integer's width */
  const AstDecl* decl;        /* an enum, or a struct (its template when generic) */
  const AstTypeRef* inner;    /* a List's element, an opt's payload */
  const char* bad;            /* why it cannot cross (BK_BAD) */
} BinInfo;

static const AstDecl* bin_find_decl(CompilerContext* ctx, Str name, AstDeclKind kind) {
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != kind) continue;
    if (kind == AST_DECL_TYPE && d->as.type_decl.specialization_args) continue;
    Str declared = kind == AST_DECL_TYPE ? d->as.type_decl.name : d->as.enum_decl.name;
    if (str_eq(declared, name)) return d;
  }
  return NULL;
}

static const AstTypeRef* bin_without_opt(CompilerContext* ctx, const AstTypeRef* type) {
  AstTypeRef* copy = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
  *copy = *type;
  copy->is_opt = false;
  copy->next = NULL;
  return copy;
}

static BinInfo bin_classify(CompilerContext* ctx, const AstTypeRef* type) {
  BinInfo info = {0};
  if (!type) { info.bad = "has no type"; return info; }
  if (type->is_view || type->is_mod) { info.bad = "is a view/mod alias"; return info; }
  if (type->is_opt) {
    info.kind = BK_OPT;
    info.inner = bin_without_opt(ctx, type);
    return info;
  }
  Str base = get_base_type_name(type);
  static const struct { const char* name; int bytes; } ints[] = {
    { "Int", 8 }, { "Int64", 8 }, { "UInt64", 8 }, { "Int32", 4 }, { "UInt32", 4 },
    { "Int16", 2 }, { "UInt16", 2 }, { "Int8", 1 }, { "UInt8", 1 },
  };
  for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
    if (str_eq_cstr(base, ints[i].name)) { info.kind = BK_INT; info.bytes = ints[i].bytes; return info; }
  }
  if (str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) { info.kind = BK_FLOAT32; return info; }
  if (str_eq_cstr(base, "Float64")) { info.kind = BK_FLOAT64; return info; }
  if (str_eq_cstr(base, "Bool")) { info.kind = BK_BOOL; return info; }
  if (str_eq_cstr(base, "String")) { info.kind = BK_STRING; return info; }
  if (str_eq_cstr(base, "List")) {
    if (!type->generic_args) { info.bad = "is a List without an element type"; return info; }
    info.kind = BK_LIST;
    info.inner = type->generic_args;
    return info;
  }
  static const char* const never[] = {
    "Ptr", "Buffer", "Task", "Channel", "Any", "Char", "Array", "StringMap", "IntMap", "ComponentTable",
  };
  for (size_t i = 0; i < sizeof never / sizeof never[0]; i++) {
    if (str_eq_cstr(base, never[i])) { info.bad = "cannot cross"; return info; }
  }
  /* The declaration sema resolved, when it did: two modules may each declare
   * a type of this name. Else the first declaration of the name. */
  const TypeInfo* resolved = type->resolved_type;
  while (resolved && resolved->kind == TYPE_REF) resolved = resolved->as.ref.base;
  const AstDecl* type_decl = NULL;
  if (resolved && (resolved->kind == TYPE_STRUCT || resolved->kind == TYPE_GENERIC_INST)
      && resolved->as.structure.decl && resolved->as.structure.decl->kind == AST_DECL_TYPE)
    type_decl = resolved->as.structure.decl;
  if (type_decl && type_decl->as.type_decl.specialization_args) type_decl = NULL;
  const AstDecl* enum_decl = type_decl ? NULL : bin_find_decl(ctx, base, AST_DECL_ENUM);
  if (enum_decl) { info.kind = BK_ENUM; info.decl = enum_decl; return info; }
  if (!type_decl) type_decl = bin_find_decl(ctx, base, AST_DECL_TYPE);
  if (!type_decl) { info.bad = "is not a type the encoding knows"; return info; }
  if (has_property(type_decl->as.type_decl.properties, "c_struct")) { info.bad = "is a C struct (its layout is C's)"; return info; }
  if (type_decl->as.type_decl.generic_params && !type->generic_args) { info.bad = "is a generic type without its arguments"; return info; }
  info.kind = BK_STRUCT;
  info.decl = type_decl;
  return info;
}

/* A struct's fields, a generic instance's with its arguments substituted. */
static const AstTypeField* bin_fields(CompilerContext* ctx, const AstTypeRef* type, const AstDecl* decl) {
  const AstTypeDecl* td = &decl->as.type_decl;
  if (!td->generic_params) return td->fields;
  AstTypeField* head = NULL;
  AstTypeField* tail = NULL;
  for (const AstTypeField* f = td->fields; f; f = f->next) {
    AstTypeField* copy = arena_alloc(ctx->ast_arena, sizeof(AstTypeField));
    *copy = *f;
    copy->next = NULL;
    copy->type = substitute_type_ref(ctx, td->generic_params, type->generic_args, f->type);
    if (!head) head = tail = copy; else { tail->next = copy; tail = copy; }
  }
  return head;
}

/* ---- crossability ---------------------------------------------------------- */

typedef struct { const AstDecl* decls[64]; int count; } BinStack;

static bool bin_stack_has(const BinStack* stack, const AstDecl* decl) {
  for (int i = 0; i < stack->count; i++) if (stack->decls[i] == decl) return true;
  return false;
}

static void bin_type_spelling(const AstTypeRef* type, char* buf, size_t cap) {
  Str base = get_base_type_name(type);
  snprintf(buf, cap, "%s%s%.*s%s", type && type->is_view ? "view " : "", type && type->is_mod ? "mod " : "",
           (int)base.len, base.data ? base.data : "", type && type->generic_args ? "(...)" : "");
}

static bool bin_check(CompilerContext* ctx, const AstTypeRef* type, const char* path, BinStack* stack,
                      char* message, size_t cap) {
  BinInfo info = bin_classify(ctx, type);
  if (info.kind == BK_BAD) {
    char spelled[160];
    bin_type_spelling(type, spelled, sizeof spelled);
    snprintf(message, cap,
             "`%s` is %s, which %s: toBinary / fromBinary carry numbers, Bool, String, enums, "
             "Lists, opt values and structs of them (docs/wasm-interfaces.md §2)",
             path, spelled, info.bad);
    return false;
  }
  if (info.kind == BK_LIST || info.kind == BK_OPT) {
    char inner_path[512];
    snprintf(inner_path, sizeof inner_path, "%s%s", path, info.kind == BK_LIST ? "[]" : "");
    return bin_check(ctx, info.inner, inner_path, stack, message, cap);
  }
  if (info.kind != BK_STRUCT) return true;
  if (bin_stack_has(stack, info.decl) || stack->count >= 64) return true;
  stack->decls[stack->count++] = info.decl;
  bool ok = true;
  for (const AstTypeField* f = bin_fields(ctx, type, info.decl); f && ok; f = f->next) {
    char field_path[512];
    snprintf(field_path, sizeof field_path, "%s.%.*s", path, (int)f->name.len, f->name.data);
    ok = bin_check(ctx, f->type, field_path, stack, message, cap);
  }
  stack->count--;
  return ok;
}

bool binary_type_crossable(CompilerContext* ctx, const AstTypeRef* type, char* message, size_t cap) {
  BinStack stack = {0};
  Str base = get_base_type_name(type);
  char root[160];
  snprintf(root, sizeof root, "%.*s", (int)base.len, base.data ? base.data : "?");
  return bin_check(ctx, type, root, &stack, message, cap);
}

/* ---- the canonical description --------------------------------------------- */

typedef struct { char* data; size_t len; size_t cap; } BinText;

static void text_put(BinText* text, const char* bytes, size_t count) {
  if (text->len + count + 1 > text->cap) {
    size_t cap = text->cap ? text->cap * 2 : 128;
    while (cap < text->len + count + 1) cap *= 2;
    text->data = realloc(text->data, cap);
    text->cap = cap;
  }
  memcpy(text->data + text->len, bytes, count);
  text->len += count;
  text->data[text->len] = 0;
}

static void text_str(BinText* text, Str s) { text_put(text, s.data ? s.data : "", s.len); }
static void text_cstr(BinText* text, const char* s) { text_put(text, s, strlen(s)); }

static void bin_describe(CompilerContext* ctx, const AstTypeRef* type, BinText* text, BinStack* stack) {
  BinInfo info = bin_classify(ctx, type);
  switch (info.kind) {
    case BK_INT: {
      /* Int and Int64 are one type. */
      Str base = get_base_type_name(type);
      if (str_eq_cstr(base, "Int64")) text_cstr(text, "Int"); else text_str(text, base);
      return;
    }
    case BK_FLOAT32: text_cstr(text, "Float"); return;
    case BK_FLOAT64: text_cstr(text, "Float64"); return;
    case BK_BOOL: text_cstr(text, "Bool"); return;
    case BK_STRING: text_cstr(text, "String"); return;
    case BK_OPT: text_cstr(text, "opt "); bin_describe(ctx, info.inner, text, stack); return;
    case BK_LIST: text_cstr(text, "List("); bin_describe(ctx, info.inner, text, stack); text_cstr(text, ")"); return;
    case BK_ENUM: {
      text_str(text, type_written_name(info.decl->as.enum_decl.name));
      text_cstr(text, "[");
      for (const AstEnumMember* m = info.decl->as.enum_decl.members; m; m = m->next) {
        text_str(text, m->name);
        if (m->next) text_cstr(text, ",");
      }
      text_cstr(text, "]");
      return;
    }
    case BK_STRUCT: {
      /* The name as written: a second module's `Frame` is `Frame` too. */
      text_str(text, type_written_name(info.decl->as.type_decl.name));
      if (type->generic_args) {
        text_cstr(text, "(");
        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
          bin_describe(ctx, a, text, stack);
          if (a->next) text_cstr(text, ",");
        }
        text_cstr(text, ")");
      }
      /* A type inside itself is written by name: the description is finite. */
      if (bin_stack_has(stack, info.decl) || stack->count >= 64) return;
      stack->decls[stack->count++] = info.decl;
      text_cstr(text, "{");
      for (const AstTypeField* f = bin_fields(ctx, type, info.decl); f; f = f->next) {
        text_str(text, f->name);
        text_cstr(text, ":");
        bin_describe(ctx, f->type, text, stack);
        if (f->next) text_cstr(text, ",");
      }
      text_cstr(text, "}");
      stack->count--;
      return;
    }
    default: text_cstr(text, "?"); return;
  }
}

const char* binary_schema_text(CompilerContext* ctx, const AstTypeRef* type) {
  BinText text = {0};
  BinStack stack = {0};
  bin_describe(ctx, type, &text, &stack);
  char* out = arena_alloc(ctx->ast_arena, text.len + 1);
  memcpy(out, text.data ? text.data : "", text.len);
  out[text.len] = 0;
  free(text.data);
  return out;
}

int64_t binary_schema_hash(const char* text) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
    hash ^= *p;
    hash *= 1099511628211ull;
  }
  return (int64_t)hash;
}

/* ---- the types a program encodes ------------------------------------------- */

const AstTypeRef* binary_bytes_type(CompilerContext* ctx) {
  AstTypeRef* byte_type = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
  memset(byte_type, 0, sizeof *byte_type);
  byte_type->parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
  memset(byte_type->parts, 0, sizeof *byte_type->parts);
  byte_type->parts->text = str_from_cstr("UInt8");
  AstTypeRef* list_type = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
  memset(list_type, 0, sizeof *list_type);
  list_type->parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
  memset(list_type->parts, 0, sizeof *list_type->parts);
  list_type->parts->text = str_from_cstr("List");
  list_type->generic_args = byte_type;
  return list_type;
}

const AstTypeRef* binary_opt_type(CompilerContext* ctx, const AstTypeRef* type) {
  AstTypeRef* copy = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
  *copy = *type;
  copy->is_opt = true;
  copy->is_view = false;
  copy->is_mod = false;
  copy->next = NULL;
  return copy;
}

const AstTypeRef* c_binary_call_type(CFuncContext* ctx, const AstExpr* expr, bool* is_from) {
  if (!expr || expr->kind != AST_EXPR_METHOD_CALL || !expr->as.method_call.object) return NULL;
  Str method = expr->as.method_call.method_name;
  CompilerContext* cc = ctx->compiler_ctx;
  const AstTypeRef* type = NULL;
  if (str_eq_cstr(method, "toBinary") && !expr->as.method_call.args) {
    type = infer_expr_type_ref(ctx, expr->as.method_call.object);
    *is_from = false;
  } else if (str_eq_cstr(method, "fromBinary") && expr->as.method_call.object->kind == AST_EXPR_IDENT) {
    AstTypeRef* named = arena_alloc(cc->ast_arena, sizeof(AstTypeRef));
    memset(named, 0, sizeof *named);
    named->parts = arena_alloc(cc->ast_arena, sizeof(AstIdentifierPart));
    memset(named->parts, 0, sizeof *named->parts);
    named->parts->text = expr->as.method_call.object->as.ident;
    type = named;
    *is_from = true;
  } else {
    return NULL;
  }
  if (!type) return NULL;
  if (ctx->generic_params && ctx->generic_args) {
    const AstTypeRef* substituted = substitute_type_ref(cc, ctx->generic_params, ctx->generic_args, type);
    if (substituted) type = substituted;
  }
  AstTypeRef* plain = arena_alloc(cc->ast_arena, sizeof(AstTypeRef));
  *plain = *type;
  plain->is_view = false;
  plain->is_mod = false;
  plain->next = NULL;
  if (bin_classify(cc, plain).kind != BK_STRUCT) return NULL;
  return plain;
}

void binary_register_type(CompilerContext* ctx, const AstTypeRef* type, bool is_from) {
  if (!type) return;
  const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, type);
  if (!mangled) return;
  unsigned char use = is_from ? 2 : 1;
  for (size_t i = 0; i < ctx->binary_type_count; i++) {
    if (strcmp(rae_mangle_type_specialized(ctx, NULL, NULL, ctx->binary_types[i]), mangled) == 0) {
      ctx->binary_type_uses[i] |= use;
      return;
    }
  }
  if (ctx->binary_type_count >= ctx->binary_type_cap) {
    size_t cap = ctx->binary_type_cap ? ctx->binary_type_cap * 2 : 8;
    ctx->binary_types = realloc((void*)ctx->binary_types, cap * sizeof(*ctx->binary_types));
    ctx->binary_type_uses = realloc(ctx->binary_type_uses, cap);
    ctx->binary_type_cap = cap;
  }
  ctx->binary_type_uses[ctx->binary_type_count] = use;
  ctx->binary_types[ctx->binary_type_count++] = type;
}

/* Every struct reached from the registered types, each once. */
typedef struct { const AstTypeRef** types; const char** mangled; size_t count; size_t cap; } BinStructs;

static void bin_collect(CompilerContext* ctx, const AstTypeRef* type, BinStructs* all) {
  BinInfo info = bin_classify(ctx, type);
  if (info.kind == BK_LIST || info.kind == BK_OPT) { bin_collect(ctx, info.inner, all); return; }
  if (info.kind != BK_STRUCT) return;
  const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, type);
  for (size_t i = 0; i < all->count; i++) if (strcmp(all->mangled[i], mangled) == 0) return;
  if (all->count >= all->cap) {
    all->cap = all->cap ? all->cap * 2 : 16;
    all->types = realloc((void*)all->types, all->cap * sizeof(*all->types));
    all->mangled = realloc((void*)all->mangled, all->cap * sizeof(*all->mangled));
  }
  all->types[all->count] = type;
  all->mangled[all->count] = mangled;
  all->count++;
  for (const AstTypeField* f = bin_fields(ctx, type, info.decl); f; f = f->next) bin_collect(ctx, f->type, all);
}

/* The fewest bytes a value of `type` encodes to: a count is checked against
 * it before a List is allocated. */
static int64_t bin_min_bytes(CompilerContext* ctx, const AstTypeRef* type, int depth) {
  BinInfo info = bin_classify(ctx, type);
  switch (info.kind) {
    case BK_INT: return info.bytes;
    case BK_FLOAT32: return 4;
    case BK_FLOAT64: return 8;
    case BK_BOOL: return 1;
    case BK_ENUM: return 4;
    case BK_STRING: return 4;
    case BK_LIST: return 4;
    case BK_OPT: return 1;
    case BK_STRUCT: {
      if (depth > 16) return 0;
      int64_t total = 0;
      for (const AstTypeField* f = bin_fields(ctx, type, info.decl); f; f = f->next)
        total += bin_min_bytes(ctx, f->type, depth + 1);
      return total;
    }
    default: return 0;
  }
}

static bool bin_has_heap(CompilerContext* ctx, const AstTypeRef* type, int depth) {
  BinInfo info = bin_classify(ctx, type);
  if (info.kind == BK_STRING || info.kind == BK_LIST) return true;
  if (info.kind == BK_OPT) return bin_has_heap(ctx, info.inner, depth);
  if (info.kind != BK_STRUCT || depth > 16) return false;
  for (const AstTypeField* f = bin_fields(ctx, type, info.decl); f; f = f->next)
    if (bin_has_heap(ctx, f->type, depth + 1)) return true;
  return false;
}

static int bin_enum_count(const AstDecl* decl) {
  int count = 0;
  for (const AstEnumMember* m = decl->as.enum_decl.members; m; m = m->next) count++;
  return count;
}

/* ---- the generated C -------------------------------------------------------- */

typedef struct { CompilerContext* ctx; FILE* out; int temp; } BinGen;

static void indent(FILE* out, int level) { for (int i = 0; i < level; i++) fputs("  ", out); }

static void gen_put(BinGen* gen, const AstTypeRef* type, const char* value, int level) {
  BinInfo info = bin_classify(gen->ctx, type);
  FILE* out = gen->out;
  indent(out, level);
  switch (info.kind) {
    case BK_INT: fprintf(out, "rae_bin_put(out, (uint64_t)(%s), %d);\n", value, info.bytes); return;
    case BK_BOOL: fprintf(out, "rae_bin_put(out, (%s) ? 1u : 0u, 1);\n", value); return;
    case BK_ENUM: fprintf(out, "rae_bin_put(out, (uint64_t)(int64_t)(%s), 4);\n", value); return;
    case BK_FLOAT32: fprintf(out, "rae_bin_put_f32(out, (float)(%s));\n", value); return;
    case BK_FLOAT64: fprintf(out, "rae_bin_put_f64(out, (double)(%s));\n", value); return;
    case BK_STRING: fprintf(out, "rae_bin_put_string(out, %s);\n", value); return;
    case BK_OPT: {
      fprintf(out, "rae_bin_put(out, (%s).has ? 1u : 0u, 1);\n", value);
      indent(out, level);
      fprintf(out, "if ((%s).has) {\n", value);
      char inner[512];
      snprintf(inner, sizeof inner, "(%s).value", value);
      gen_put(gen, info.inner, inner, level + 1);
      indent(out, level);
      fprintf(out, "}\n");
      return;
    }
    case BK_LIST: {
      int k = gen->temp++;
      fprintf(out, "{ const __typeof__(%s)* list%d = &(%s);\n", value, k, value);
      indent(out, level + 1);
      fprintf(out, "if ((uint64_t)list%d->length > 0xFFFFFFFFu) out->failed = 1;\n", k);
      indent(out, level + 1);
      fprintf(out, "rae_bin_put(out, (uint64_t)list%d->length, 4);\n", k);
      indent(out, level + 1);
      fprintf(out, "for (int64_t index%d = 0; index%d < list%d->length; index%d++) {\n", k, k, k, k);
      char element[512];
      snprintf(element, sizeof element, "list%d->data[index%d]", k, k);
      gen_put(gen, info.inner, element, level + 2);
      indent(out, level + 1);
      fprintf(out, "}\n");
      indent(out, level);
      fprintf(out, "}\n");
      return;
    }
    case BK_STRUCT:
      fprintf(out, "rae_binPut_%s_(out, &(%s));\n", rae_mangle_type_specialized(gen->ctx, NULL, NULL, type), value);
      return;
    default: fprintf(out, "out->failed = 1;\n"); return;
  }
}

static void gen_get(BinGen* gen, const AstTypeRef* type, const char* target, int level) {
  BinInfo info = bin_classify(gen->ctx, type);
  FILE* out = gen->out;
  indent(out, level);
  switch (info.kind) {
    case BK_INT:
      fprintf(out, "{ uint64_t raw; if (!rae_bin_get(in, %d, &raw)) return 0; %s = (__typeof__(%s))raw; }\n",
              info.bytes, target, target);
      return;
    case BK_BOOL:
      fprintf(out, "{ uint64_t raw; if (!rae_bin_get(in, 1, &raw) || raw > 1) return 0; %s = raw ? 1 : 0; }\n", target);
      return;
    case BK_ENUM:
      fprintf(out, "{ uint64_t raw; if (!rae_bin_get(in, 4, &raw) || raw >= %du) return 0; %s = (__typeof__(%s))raw; }\n",
              bin_enum_count(info.decl), target, target);
      return;
    case BK_FLOAT32:
      fprintf(out, "{ float raw; if (!rae_bin_get_f32(in, &raw)) return 0; %s = (__typeof__(%s))raw; }\n", target, target);
      return;
    case BK_FLOAT64:
      fprintf(out, "{ double raw; if (!rae_bin_get_f64(in, &raw)) return 0; %s = (__typeof__(%s))raw; }\n", target, target);
      return;
    case BK_STRING: fprintf(out, "if (!rae_bin_get_string(in, &(%s))) return 0;\n", target); return;
    case BK_OPT: {
      fprintf(out, "{ uint64_t raw; if (!rae_bin_get(in, 1, &raw) || raw > 1) return 0;\n");
      indent(out, level + 1);
      /* `has` first: a payload that fails half way is then freed with it. */
      fprintf(out, "if (raw) { (%s).has = 1;\n", target);
      char inner[512];
      snprintf(inner, sizeof inner, "(%s).value", target);
      gen_get(gen, info.inner, inner, level + 2);
      indent(out, level + 1);
      fprintf(out, "}\n");
      indent(out, level);
      fprintf(out, "}\n");
      return;
    }
    case BK_LIST: {
      int k = gen->temp++;
      int64_t min_bytes = bin_min_bytes(gen->ctx, info.inner, 0);
      fprintf(out, "{ __typeof__(%s)* list%d = &(%s); int64_t count%d = 0;\n", target, k, target, k);
      indent(out, level + 1);
      fprintf(out, "if (!rae_bin_count(in, %lld, &count%d)) return 0;\n", (long long)min_bytes, k);
      indent(out, level + 1);
      fprintf(out, "if (count%d > 0) { list%d->data = rae_ext_rae_buf_alloc(count%d, sizeof(*list%d->data)); "
                   "if (!list%d->data) return 0; list%d->cap = count%d; }\n", k, k, k, k, k, k, k);
      indent(out, level + 1);
      /* The length counts the element being read, so a failure frees it too
       * (the slot is zeroed, and zero is every type's empty value). */
      fprintf(out, "for (int64_t index%d = 0; index%d < count%d; index%d++) { list%d->length = index%d + 1;\n",
              k, k, k, k, k, k);
      char element[512];
      snprintf(element, sizeof element, "list%d->data[index%d]", k, k);
      gen_get(gen, info.inner, element, level + 2);
      indent(out, level + 1);
      fprintf(out, "}\n");
      indent(out, level);
      fprintf(out, "}\n");
      return;
    }
    case BK_STRUCT:
      fprintf(out, "if (!rae_binGet_%s_(in, &(%s), depth + 1)) return 0;\n",
              rae_mangle_type_specialized(gen->ctx, NULL, NULL, type), target);
      return;
    default: fprintf(out, "return 0;\n"); return;
  }
}

static void gen_free(BinGen* gen, const AstTypeRef* type, const char* target, int level) {
  if (!bin_has_heap(gen->ctx, type, 0)) return;
  BinInfo info = bin_classify(gen->ctx, type);
  FILE* out = gen->out;
  indent(out, level);
  switch (info.kind) {
    case BK_STRING: fprintf(out, "rae_string_drop(&(%s));\n", target); return;
    case BK_OPT: {
      fprintf(out, "if ((%s).has) {\n", target);
      char inner[512];
      snprintf(inner, sizeof inner, "(%s).value", target);
      gen_free(gen, info.inner, inner, level + 1);
      indent(out, level);
      fprintf(out, "}\n");
      return;
    }
    case BK_LIST: {
      int k = gen->temp++;
      fprintf(out, "{ __typeof__(%s)* list%d = &(%s);\n", target, k, target);
      if (bin_has_heap(gen->ctx, info.inner, 0)) {
        indent(out, level + 1);
        fprintf(out, "for (int64_t index%d = 0; index%d < list%d->length; index%d++) {\n", k, k, k, k);
        char element[512];
        snprintf(element, sizeof element, "list%d->data[index%d]", k, k);
        gen_free(gen, info.inner, element, level + 2);
        indent(out, level + 1);
        fprintf(out, "}\n");
      }
      indent(out, level + 1);
      fprintf(out, "rae_ext_rae_buf_free(list%d->data); list%d->data = NULL; list%d->length = 0; list%d->cap = 0;\n",
              k, k, k, k);
      indent(out, level);
      fprintf(out, "}\n");
      return;
    }
    case BK_STRUCT:
      fprintf(out, "rae_binFree_%s_(&(%s));\n", rae_mangle_type_specialized(gen->ctx, NULL, NULL, type), target);
      return;
    default: fprintf(out, "\n"); return;
  }
}

static void emit_byte_helpers(FILE* out) {
  fputs(
    "/* ---- the derived binary encoding (docs/wasm-interfaces.md §2) ---- */\n"
    "typedef struct { uint8_t* data; int64_t len; int64_t cap; int failed; } rae_BinOut;\n"
    "typedef struct { const uint8_t* data; int64_t len; int64_t pos; } rae_BinIn;\n"
    "RAE_UNUSED static int rae_bin_reserve(rae_BinOut* out, int64_t count) {\n"
    "  if (out->failed) return 0;\n"
    "  if (out->len + count <= out->cap) return 1;\n"
    "  int64_t cap = out->cap > 0 ? out->cap * 2 : 64;\n"
    "  while (cap < out->len + count) cap *= 2;\n"
    "  uint8_t* grown = (uint8_t*)rae_ext_rae_buf_resize(out->data, cap, 1);\n"
    "  if (!grown) { out->failed = 1; return 0; }\n"
    "  out->data = grown; out->cap = cap; return 1;\n"
    "}\n"
    "RAE_UNUSED static void rae_bin_put(rae_BinOut* out, uint64_t value, int count) {\n"
    "  if (!rae_bin_reserve(out, count)) return;\n"
    "  for (int i = 0; i < count; i++) out->data[out->len++] = (uint8_t)(value >> (8 * i));\n"
    "}\n"
    "RAE_UNUSED static void rae_bin_put_f32(rae_BinOut* out, float value) { uint32_t bits; memcpy(&bits, &value, 4); rae_bin_put(out, bits, 4); }\n"
    "RAE_UNUSED static void rae_bin_put_f64(rae_BinOut* out, double value) { uint64_t bits; memcpy(&bits, &value, 8); rae_bin_put(out, bits, 8); }\n"
    "RAE_UNUSED static void rae_bin_put_string(rae_BinOut* out, rae_String text) {\n"
    "  if ((uint64_t)text.len > 0xFFFFFFFFu) { out->failed = 1; return; }\n"
    "  rae_bin_put(out, (uint64_t)text.len, 4);\n"
    "  if (text.len > 0 && rae_bin_reserve(out, text.len)) { memcpy(out->data + out->len, text.data, (size_t)text.len); out->len += text.len; }\n"
    "}\n"
    "RAE_UNUSED static int rae_bin_get(rae_BinIn* in, int count, uint64_t* value) {\n"
    "  if (in->len - in->pos < count) return 0;\n"
    "  uint64_t result = 0;\n"
    "  for (int i = 0; i < count; i++) result |= (uint64_t)in->data[in->pos + i] << (8 * i);\n"
    "  in->pos += count; *value = result; return 1;\n"
    "}\n"
    "RAE_UNUSED static int rae_bin_get_f32(rae_BinIn* in, float* value) { uint64_t bits; if (!rae_bin_get(in, 4, &bits)) return 0; uint32_t narrow = (uint32_t)bits; memcpy(value, &narrow, 4); return 1; }\n"
    "RAE_UNUSED static int rae_bin_get_f64(rae_BinIn* in, double* value) { uint64_t bits; if (!rae_bin_get(in, 8, &bits)) return 0; memcpy(value, &bits, 8); return 1; }\n"
    "/* A count of elements that each take at least `smallest` bytes: refused\n"
    " * before anything is allocated when the bytes left cannot hold them. */\n"
    "RAE_UNUSED static int rae_bin_count(rae_BinIn* in, int64_t smallest, int64_t* count) {\n"
    "  uint64_t raw; if (!rae_bin_get(in, 4, &raw)) return 0;\n"
    "  int64_t left = in->len - in->pos;\n"
    "  if (smallest > 0 ? (int64_t)raw > left / smallest : raw > 65536u) return 0;\n"
    "  *count = (int64_t)raw; return 1;\n"
    "}\n"
    "/* Well-formed UTF-8: no overlong form, no surrogate, nothing past U+10FFFF. */\n"
    "RAE_UNUSED static int rae_bin_utf8(const uint8_t* text, int64_t count) {\n"
    "  int64_t i = 0;\n"
    "  while (i < count) {\n"
    "    uint8_t lead = text[i];\n"
    "    if (lead < 0x80) { i++; continue; }\n"
    "    int extra; uint32_t point;\n"
    "    if (lead >= 0xC2 && lead <= 0xDF) { extra = 1; point = lead & 0x1F; }\n"
    "    else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; point = lead & 0x0F; }\n"
    "    else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; point = lead & 0x07; }\n"
    "    else return 0;\n"
    "    if (count - i - 1 < extra) return 0;\n"
    "    for (int k = 1; k <= extra; k++) {\n"
    "      uint8_t next = text[i + k];\n"
    "      if ((next & 0xC0) != 0x80) return 0;\n"
    "      point = (point << 6) | (next & 0x3F);\n"
    "    }\n"
    "    if ((extra == 2 && point < 0x800) || (extra == 3 && point < 0x10000)) return 0;\n"
    "    if (point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return 0;\n"
    "    i += extra + 1;\n"
    "  }\n"
    "  return 1;\n"
    "}\n"
    "RAE_UNUSED static int rae_bin_get_string(rae_BinIn* in, rae_String* text) {\n"
    "  uint64_t raw; if (!rae_bin_get(in, 4, &raw)) return 0;\n"
    "  if ((int64_t)raw > in->len - in->pos) return 0;\n"
    "  int64_t count = (int64_t)raw;\n"
    "  if (!rae_bin_utf8(in->data + in->pos, count)) return 0;\n"
    "  if (count > 0) *text = rae_string_pool_take(rae_ext_rae_str_from_bytes((uint8_t*)in->data, in->pos, count));\n"
    "  in->pos += count; return 1;\n"
    "}\n"
    "#define RAE_BIN_MAX_DEPTH 200\n",
    out);
}

void binary_emit_helpers(CompilerContext* ctx, FILE* out) {
  if (ctx->binary_type_count == 0) return;
  BinStructs all = {0};
  for (size_t i = 0; i < ctx->binary_type_count; i++) bin_collect(ctx, ctx->binary_types[i], &all);
  if (all.count == 0) return;
  emit_byte_helpers(out);
  for (size_t i = 0; i < all.count; i++) {
    const char* m = all.mangled[i];
    fprintf(out, "RAE_UNUSED static void rae_binPut_%s_(rae_BinOut* out, const %s* value);\n", m, m);
    fprintf(out, "RAE_UNUSED static int rae_binGet_%s_(rae_BinIn* in, %s* value, int depth);\n", m, m);
    fprintf(out, "RAE_UNUSED static void rae_binFree_%s_(%s* value);\n", m, m);
  }
  BinGen gen = { ctx, out, 0 };
  for (size_t i = 0; i < all.count; i++) {
    const char* m = all.mangled[i];
    BinInfo info = bin_classify(ctx, all.types[i]);
    const AstTypeField* fields = bin_fields(ctx, all.types[i], info.decl);
    fprintf(out, "RAE_UNUSED static void rae_binPut_%s_(rae_BinOut* out, const %s* value) {\n", m, m);
    for (const AstTypeField* f = fields; f; f = f->next) {
      char access[256];
      snprintf(access, sizeof access, "value->%.*s", (int)f->name.len, f->name.data);
      gen_put(&gen, f->type, access, 1);
    }
    fprintf(out, "  (void)out; (void)value;\n}\n");
    fprintf(out, "RAE_UNUSED static int rae_binGet_%s_(rae_BinIn* in, %s* value, int depth) {\n", m, m);
    fprintf(out, "  (void)in; (void)value; if (depth > RAE_BIN_MAX_DEPTH) return 0;\n");
    for (const AstTypeField* f = fields; f; f = f->next) {
      char access[256];
      snprintf(access, sizeof access, "value->%.*s", (int)f->name.len, f->name.data);
      gen_get(&gen, f->type, access, 1);
    }
    fprintf(out, "  return 1;\n}\n");
    fprintf(out, "RAE_UNUSED static void rae_binFree_%s_(%s* value) {\n  (void)value;\n", m, m);
    for (const AstTypeField* f = fields; f; f = f->next) {
      char access[256];
      snprintf(access, sizeof access, "value->%.*s", (int)f->name.len, f->name.data);
      gen_free(&gen, f->type, access, 1);
    }
    fprintf(out, "}\n");
  }
  /* The two calls a program makes, for the types it makes them on. */
  AstTypeRef bytes_type = {0};
  AstIdentifierPart list_part = { .text = { .data = "List", .len = 4 } };
  AstIdentifierPart byte_part = { .text = { .data = "UInt8", .len = 5 } };
  AstTypeRef byte_type = {0};
  byte_type.parts = &byte_part;
  bytes_type.parts = &list_part;
  bytes_type.generic_args = &byte_type;
  const char* bytes_c = rae_mangle_type_specialized(ctx, NULL, NULL, &bytes_type);
  for (size_t i = 0; i < ctx->binary_type_count; i++) {
    const AstTypeRef* type = ctx->binary_types[i];
    if (bin_classify(ctx, type).kind != BK_STRUCT) continue;
    const char* m = rae_mangle_type_specialized(ctx, NULL, NULL, type);
    AstTypeRef opt_type = *type;
    opt_type.is_opt = true;
    const char* opt_c = rae_mangle_type_specialized(ctx, NULL, NULL, &opt_type);
    unsigned char uses = ctx->binary_type_uses[i];
    if (uses & 1) {
    fprintf(out, "RAE_UNUSED static %s rae_toBinary_%s_(const %s* value) {\n", bytes_c, m, m);
    fprintf(out, "  rae_BinOut out = {0}; rae_binPut_%s_(&out, value);\n", m);
    fprintf(out, "  %s result; memset(&result, 0, sizeof result);\n", bytes_c);
    fprintf(out, "  if (out.failed) { rae_ext_rae_buf_free(out.data); return result; }\n");
    fprintf(out, "  result.data = out.data; result.length = out.len; result.cap = out.cap; return result;\n}\n");
    /* A temporary receiver (`make().toBinary()`): encoded, then freed. */
    fprintf(out, "RAE_UNUSED static %s rae_toBinaryOwned_%s_(%s value) {\n", bytes_c, m, m);
    fprintf(out, "  %s result = rae_toBinary_%s_(&value); rae_binFree_%s_(&value); return result;\n}\n", bytes_c, m, m);
    }
    if (!(uses & 2)) continue;
    fprintf(out, "RAE_UNUSED static %s rae_fromBinary_%s_(const %s* bytes) {\n", opt_c, m, bytes_c);
    fprintf(out, "  %s result; memset(&result, 0, sizeof result);\n", opt_c);
    fprintf(out, "  rae_BinIn in = { bytes->data, bytes->length, 0 };\n");
    fprintf(out, "  if (!rae_binGet_%s_(&in, &result.value, 0) || in.pos != in.len) {\n", m);
    fprintf(out, "    rae_binFree_%s_(&result.value); memset(&result, 0, sizeof result); return result;\n  }\n", m);
    fprintf(out, "  result.has = 1; return result;\n}\n");
    /* Temporary bytes (`T.fromBinary(bytes: value.toBinary())`): freed after. */
    fprintf(out, "RAE_UNUSED static %s rae_fromBinaryOwned_%s_(%s bytes) {\n", opt_c, m, bytes_c);
    fprintf(out, "  %s result = rae_fromBinary_%s_(&bytes); rae_ext_rae_buf_free(bytes.data); return result;\n}\n",
            opt_c, m);
  }
  free((void*)all.types);
  free((void*)all.mangled);
}
