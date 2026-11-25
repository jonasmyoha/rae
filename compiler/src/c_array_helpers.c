/* The drop and the deep copy of an Array(T, cap: N) whose elements need one
 * (an Array(Float, cap: 16) gets neither: it is plain bytes). An Array has no
 * declaration and no fields, just N elements, so its helpers are a loop where a
 * struct's are a field walk (c_backend.c). Also: which types have a deep copy
 * helper at all. */
#include <stdio.h>
#include <string.h>

#include "c_backend.h"
#include "c_backend_internal.h"
#include "mangler.h"
#include "ownership.h"
#include "sema.h"

/* Is there a `rae_deep_copy_<type>` for a value of this type (no optional)? A
 * List or map, a struct, an instance of a generic struct, an Array; the caller
 * has asked whether it needs one. */
bool c_has_deep_copy_helper(CFuncContext* ctx, const AstTypeRef* type) {
  if (!type || type->is_opt || type->is_view || type->is_mod) return false;
  AstTypeRef concrete = *type;
  if (ctx->generic_params && ctx->generic_args)
    concrete = *substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, (AstTypeRef*)type);
  concrete.next = NULL;
  Str base = get_base_type_name(&concrete);
  if (str_eq_cstr(base, "List") || str_eq_cstr(base, "StringMap") || str_eq_cstr(base, "IntMap"))
    return concrete.generic_args != NULL;
  if (str_eq_cstr(base, "Array") || (concrete.resolved_type && concrete.resolved_type->kind == TYPE_ARRAY)) return true;
  if (concrete.generic_args) return generic_struct_template(ctx->compiler_ctx, &concrete) != NULL;
  const AstDecl* decl = find_type_decl(ctx, ctx->module, base);
  return decl && decl->kind == AST_DECL_TYPE && !decl->as.type_decl.generic_params
      && !has_property(decl->as.type_decl.properties, "c_struct");
}

static const TypeInfo* array_info(const AstTypeRef* type) {
  const TypeInfo* info = type ? type->resolved_type : NULL;
  while (info && (info->kind == TYPE_REF || info->kind == TYPE_OPT))
    info = info->kind == TYPE_REF ? info->as.ref.base : info->as.opt.base;
  return info && info->kind == TYPE_ARRAY ? info : NULL;
}

/* (an Array of Arrays: the inner one first) */
static void add_array(CompilerContext* ctx, const AstModule* module, CArrayHelpers* arrays, const TypeInfo* info) {
  for (size_t k = 0; k < arrays->count; k++) if (arrays->infos[k] == info) return;
  if (!type_needs_cascade_drop(ctx, module, sema_type_ref_of(ctx, (TypeInfo*)info), 0)) return;
  if (info->as.array.base->kind == TYPE_ARRAY) add_array(ctx, module, arrays, info->as.array.base);
  if (arrays->count == arrays->cap) {
    size_t cap = arrays->cap ? arrays->cap * 2 : 32;
    const TypeInfo** grown = arena_alloc(ctx->ast_arena, cap * sizeof *grown);
    if (arrays->count) memcpy(grown, arrays->infos, arrays->count * sizeof *grown);
    arrays->infos = grown;
    arrays->cap = cap;
  }
  arrays->infos[arrays->count++] = info;
}

/* Every Array the program's types name */
void c_collect_array_helpers(CompilerContext* ctx, const AstModule* module, CArrayHelpers* arrays) {
  for (size_t i = 0; i < ctx->generic_type_count; i++) {
    const TypeInfo* info = array_info(ctx->generic_types[i]);
    if (info && info->as.array.base && info->as.array.base->kind != TYPE_GENERIC_PARAM
        && info->as.array.base->kind != TYPE_VOID)
      add_array(ctx, module, arrays, info);
  }
}

static const char* array_name(CompilerContext* ctx, const TypeInfo* info) {
  return type_mangle_name(ctx->ast_arena, (TypeInfo*)info).data;
}

void c_emit_array_drop_declarations(CompilerContext* ctx, const CArrayHelpers* arrays, FILE* out) {
  for (size_t i = 0; i < arrays->count; i++) {
    const char* name = array_name(ctx, arrays->infos[i]);
    fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s(%s* this);\n", name, name);
    fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s_alias(%s* this);\n", name, name);
  }
}

/* After the declarations of every struct's drop. A List's or a map's is a
 * function of its own, declared here where an element needs it. */
void c_emit_array_drops(CompilerContext* ctx, const AstModule* module, const CArrayHelpers* arrays, FILE* out) {
  CFuncContext fctx = {.compiler_ctx = ctx, .module = module};
  for (size_t i = 0; i < arrays->count; i++) {
    const TypeInfo* info = arrays->infos[i];
    const char* name = array_name(ctx, info);
    const AstTypeRef* element = sema_type_ref_of(ctx, info->as.array.base);
    bool is_string = info->as.array.base->kind == TYPE_STRING;
    if (is_drop_target_type(element) && element->generic_args) {
      const AstFuncDecl* drop = find_drop_overload_for(&fctx, get_base_type_name(element));
      if (drop) {
        register_function_specialization(ctx, drop, element->generic_args);
        fprintf(out, "RAE_UNUSED static void %s(%s* this);\n",
                rae_mangle_specialized_function(ctx, drop, element->generic_args),
                rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)element));
      }
    }
    for (int is_alias = 0; is_alias < 2; is_alias++) {
      fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s%s(%s* this) {\n", name, is_alias ? "_alias" : "", name);
      /* The _alias variant skips String elements, as a struct's does: what a
       * call answered may share them with the callee's storage */
      if (is_alias && is_string) {
        fprintf(out, "  (void)this;\n}\n");
        continue;
      }
      fprintf(out, "  for (int64_t __i = 0; __i < %lld; __i++) {\n", (long long)info->as.array.count);
      emit_drop_for_value(&fctx, out, element, "this->v[__i]", !is_alias);
      fprintf(out, "  }\n}\n");
    }
  }
}

void c_emit_array_copy_declarations(CompilerContext* ctx, const CArrayHelpers* arrays, FILE* out) {
  for (size_t i = 0; i < arrays->count; i++) {
    const char* name = array_name(ctx, arrays->infos[i]);
    fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src);\n", name, name, name);
  }
}

void c_emit_array_copies(CompilerContext* ctx, const AstModule* module, const CArrayHelpers* arrays, FILE* out) {
  CFuncContext fctx = {.compiler_ctx = ctx, .module = module};
  for (size_t i = 0; i < arrays->count; i++) {
    const TypeInfo* info = arrays->infos[i];
    const char* name = array_name(ctx, info);
    const AstTypeRef* element = sema_type_ref_of(ctx, info->as.array.base);
    fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src) {\n", name, name, name);
    fprintf(out, "  for (int64_t __i = 0; __i < %lld; __i++) ", (long long)info->as.array.count);
    if (element->is_opt && rae_opt_is_struct_rep(&fctx, element))
      fprintf(out, "rae_deep_copy_%s(&dst->v[__i], &src->v[__i]);\n", rae_opt_type_name(&fctx, element));
    else if (info->as.array.base->kind == TYPE_STRING)
      fprintf(out, "dst->v[__i] = rae_string_copy(src->v[__i]);\n");
    else if (c_has_deep_copy_helper(&fctx, element))
      fprintf(out, "rae_deep_copy_%s(&dst->v[__i], &src->v[__i]);\n",
              rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)element));
    else
      fprintf(out, "dst->v[__i] = src->v[__i];\n");
    fprintf(out, "}\n\n");
  }
}
