/* The generated text and value comparison of a StringMap(V) / IntMap(V), next to
 * those of a List(T) and a struct (c_backend.c). Where the entries lie depends on
 * the order they were set in and on the capacity, so both work on the entries
 * ordered by key: two equal maps have the same text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c_backend_internal.h"
#include "mangler.h"
#include "sema.h"

/* Is `type` a StringMap(V) or IntMap(V) of lib/collections (a value, or a view /
 * mod of one; not an optional)? A program's own generic type of that name is not. */
bool c_is_map_type(CompilerContext* ctx, const AstTypeRef* type) {
  if (!type || !type->generic_args || type->is_opt) return false;
  Str base = get_base_type_name(type);
  bool by_string = str_eq_cstr(base, "StringMap");
  if (!by_string && !str_eq_cstr(base, "IntMap")) return false;
  const char* own_module = by_string ? "collections/StringMap" : "collections/IntMap";
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_TYPE || d->as.type_decl.specialization_args) continue;
    if (!str_eq(d->as.type_decl.name, base)) continue;
    return d->module_name && strcmp(d->module_name, own_module) == 0;
  }
  return false;
}

/* Does `type` have its own generated text and comparison (`rae_to_str_<T>_`,
 * `rae_eq_<T>_`): a List(T), a map, an Array, or an instance of a generic struct? */
bool c_has_generic_value_helpers(CompilerContext* ctx, const AstTypeRef* type) {
  if (!type) return false;
  return (str_eq_cstr(get_base_type_name(type), "List") && type->generic_args)
      || c_is_map_type(ctx, type) || c_is_array_type(type) || generic_struct_template(ctx, type);
}

/* The i-th registered type's C name when it is a map that no earlier one names. */
static const char* map_helper_name(CompilerContext* ctx, size_t i) {
  const AstTypeRef* type = ctx->generic_types[i];
  if (!type || !type->parts || type->is_view || type->is_mod || !c_is_map_type(ctx, type)) return NULL;
  const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)type);
  for (size_t k = 0; mangled && k < i; k++) {
    const AstTypeRef* before = ctx->generic_types[k];
    if (!before || !before->parts || before->is_view || before->is_mod || !c_is_map_type(ctx, before)) continue;
    const char* name = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)before);
    if (name && strcmp(name, mangled) == 0) return NULL;
  }
  return mangled;
}

void emit_map_helper_declarations(CompilerContext* ctx, FILE* out) {
  for (size_t i = 0; i < ctx->generic_type_count; i++) {
    const char* mangled = map_helper_name(ctx, i);
    if (!mangled) continue;
    fprintf(out, "RAE_UNUSED static rae_String rae_to_str_%s_(const %s* this);\n", mangled, mangled);
    fprintf(out, "RAE_UNUSED static rae_Bool rae_eq_%s_(const %s* a, const %s* b);\n", mangled, mangled, mangled);
  }
}

/* The order of two entries by key: a String by its bytes, an Int by its value. */
static void emit_key_orders(FILE* out) {
  fprintf(out,
      "RAE_UNUSED static int rae_map_key_order_string(const rae_String* a, const rae_String* b) {\n"
      "  int64_t shorter = a->len < b->len ? (int64_t)a->len : (int64_t)b->len;\n"
      "  int order = shorter > 0 ? memcmp(a->data, b->data, (size_t)shorter) : 0;\n"
      "  return order != 0 ? order : (a->len > b->len) - (a->len < b->len);\n"
      "}\n"
      "RAE_UNUSED static int rae_map_key_order_int(const int64_t* a, const int64_t* b) {\n"
      "  return (*a > *b) - (*a < *b);\n"
      "}\n\n");
}

void emit_map_helper_definitions(CompilerContext* ctx, const AstModule* module, FILE* out) {
  bool any = false;
  for (size_t i = 0; i < ctx->generic_type_count; i++) {
    const char* mangled = map_helper_name(ctx, i);
    if (!mangled) continue;
    if (!any) emit_key_orders(out);
    any = true;
    const AstTypeRef* type = ctx->generic_types[i];
    bool by_string = str_eq_cstr(get_base_type_name(type), "StringMap");
    AstTypeRef value = *type->generic_args;
    value.next = NULL;
    AstTypeRef key = {.parts = &(AstIdentifierPart){.text = str_from_cstr(by_string ? "String" : "Int")}};

    /* the entries of `map` ordered by key: an array of pointers, to be freed */
    fprintf(out, "typedef __typeof__(((%s*)0)->data) rae_map_entry_%s_;\n", mangled, mangled);
    fprintf(out, "RAE_UNUSED static int rae_map_order_%s_(const void* a, const void* b) {\n", mangled);
    fprintf(out, "  return rae_map_key_order_%s(&(*(const rae_map_entry_%s_*)a)->k, "
                 "&(*(const rae_map_entry_%s_*)b)->k);\n}\n",
            by_string ? "string" : "int", mangled, mangled);
    fprintf(out, "RAE_UNUSED static rae_map_entry_%s_* rae_map_entries_%s_(const %s* map) {\n", mangled, mangled, mangled);
    fprintf(out, "  rae_map_entry_%s_* entries = malloc(sizeof(*entries) * (size_t)(map->length + 1));\n"
                 "  if (!entries) abort();\n"
                 "  int64_t count = 0;\n"
                 "  for (int64_t i = 0; i < map->cap && count < map->length; i++)\n"
                 "    if (map->data[i].occupied) entries[count++] = &map->data[i];\n"
                 "  qsort(entries, (size_t)count, sizeof(*entries), rae_map_order_%s_);\n"
                 "  return entries;\n}\n", mangled, mangled);

    /* `{ "a": 1, "b": 2 }`, an empty one `{ }` */
    fprintf(out, "RAE_UNUSED static rae_String rae_to_str_%s_(const %s* this) {\n", mangled, mangled);
    fprintf(out, "  rae_String __out = (rae_String){(uint8_t*)\"{\", 1};\n"
                 "  rae_map_entry_%s_* __entries = rae_map_entries_%s_(this);\n"
                 "  for (int64_t __i = 0; __i < this->length; __i++) {\n"
                 "    __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)\", \" + (__i == 0), 2 - (__i == 0)});\n"
                 "    __out = rae_ext_rae_str_concat(__out, ", mangled, mangled);
    rae_value_to_str_expr(ctx, module, &key, "__entries[__i]->k", true, out);
    fprintf(out, ");\n    __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)\": \", 2});\n"
                 "    __out = rae_ext_rae_str_concat(__out, ");
    rae_value_to_str_expr(ctx, module, &value, "__entries[__i]->value", true, out);
    fprintf(out, ");\n  }\n  free(__entries);\n"
                 "  return rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)\" }\", 2});\n}\n\n");

    /* the same keys with equal values */
    fprintf(out, "RAE_UNUSED static rae_Bool rae_eq_%s_(const %s* a, const %s* b) {\n", mangled, mangled, mangled);
    fprintf(out, "  if (a->length != b->length) return 0;\n"
                 "  rae_map_entry_%s_* __a = rae_map_entries_%s_(a);\n"
                 "  rae_map_entry_%s_* __b = rae_map_entries_%s_(b);\n"
                 "  rae_Bool __equal = 1;\n"
                 "  for (int64_t __i = 0; __equal && __i < a->length; __i++)\n"
                 "    __equal = ", mangled, mangled, mangled, mangled);
    rae_value_equals_expr(ctx, module, &key, "__a[__i]->k", "__b[__i]->k", out);
    fprintf(out, " && ");
    rae_value_equals_expr(ctx, module, &value, "__a[__i]->value", "__b[__i]->value", out);
    fprintf(out, ";\n  free(__a);\n  free(__b);\n  return __equal;\n}\n\n");
  }
}
