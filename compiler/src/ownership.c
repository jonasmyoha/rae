/* Backend-neutral ownership classifiers. See ownership.h for the
 * intent. Bodies are lifted verbatim from c_stmt.c — no behaviour
 * change. */
#include "ownership.h"

#include "ast.h"
#include "sema.h"
#include "str.h"
#include "type.h"

/* `has_property` and `find_type_decl` are AST utilities that
 * currently live in c_backend.c (declared in c_backend_internal.h).
 * `substitute_type_ref` is in sema.h. */
#include "c_backend_internal.h"

#include <string.h>


/* Recover the ELEMENT type of an Array(T, cap: N) reference.
 *
 * All four predicates below need the same thing: an Array is a drop target,
 * owns heap storage, cascades, or needs deep copy exactly when its element
 * type does. Written once here so the four answers cannot drift apart.
 *
 * Returns false when `type` is not an Array. */
static bool array_element_ref(const AstTypeRef* type, AstTypeRef* out) {
  if (!type) return false;
  const TypeInfo* t = type->resolved_type;
  if (t && t->kind == TYPE_REF) t = t->as.ref.base;
  if (t && t->kind == TYPE_ARRAY) {
    memset(out, 0, sizeof(*out));
    out->resolved_type = (TypeInfo*)t->as.array.base;
    return true;
  }
  /* Not yet resolved: fall back to the written generic arguments, skipping
   * the `cap:` value argument. */
  if (!str_eq_cstr(get_base_type_name(type), "Array")) return false;
  for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
    if (a->is_value_arg) continue;
    *out = *a;
    out->next = NULL;
    return true;
  }
  return false;
}

bool is_drop_target_type(const AstTypeRef* type) {
  if (!type) return false;
  if (type->is_opt) return false;
  /* Borrows don't own — they're someone else's value. */
  if (type->is_view || type->is_mod) return false;
  { AstTypeRef elem; if (array_element_ref(type, &elem)) return is_drop_target_type(&elem); }
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "List")) return true;
  if (str_eq_cstr(base, "StringMap")) return true;
  if (str_eq_cstr(base, "IntMap")) return true;
  return false;
}

bool type_owns_heap_storage(CompilerContext* cctx, const AstModule* module,
                            const AstTypeRef* type, int depth) {
  (void)cctx;
  if (!type || depth > 32) return false;
  if (type->is_view || type->is_mod) return false;
  if (type->is_opt) {
    AstTypeRef inner = *type;
    inner.is_opt = false;
    return type_owns_heap_storage(cctx, module, &inner, depth + 1);
  }
  if (is_drop_target_type(type)) return true;
  { AstTypeRef elem; if (array_element_ref(type, &elem)) return type_owns_heap_storage(cctx, module, &elem, depth + 1); }
  Str base = get_base_type_name(type);
  /* c_struct (raylib Color / Vector2 / etc.) and primitives never
   * own Rae-allocated heap storage. */
  const AstDecl* d = find_type_decl(NULL, module, base);
  if (!d || d->kind != AST_DECL_TYPE) return false;
  if (has_property(d->as.type_decl.properties, "c_struct")) return false;
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    if (type_owns_heap_storage(cctx, module, f->type, depth + 1)) return true;
  }
  return false;
}

bool type_needs_cascade_drop(CompilerContext* cctx, const AstModule* module,
                             const AstTypeRef* type, int depth) {
  if (!type || depth > 32) return false;
  if (type->is_view || type->is_mod) return false;
  if (type->is_opt) {
    AstTypeRef inner = *type;
    inner.is_opt = false;
    return type_needs_cascade_drop(cctx, module, &inner, depth + 1);
  }
  if (is_drop_target_type(type)) return true;
  { AstTypeRef elem; if (array_element_ref(type, &elem)) return type_needs_cascade_drop(cctx, module, &elem, depth + 1); }
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "String")) return true;
  const AstDecl* d = find_type_decl(NULL, module, base);
  if (!d || d->kind != AST_DECL_TYPE) return false;
  if (has_property(d->as.type_decl.properties, "c_struct")) return false;
  /* When `type` carries `generic_args` and the decl has matching
   * `generic_params`, substitute on each field before recursing so
   * a `Wrapper(String) { value: T }` reports cascade through T==String.
   * Without this, generic specs always look trivial here. */
  const AstIdentifierPart* gp = d->as.type_decl.generic_params;
  const AstTypeRef* ga = type->generic_args;
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    AstTypeRef* concrete = f->type;
    if (gp && ga) {
      concrete = substitute_type_ref(cctx, gp, ga, f->type);
    }
    if (type_needs_cascade_drop(cctx, module, concrete, depth + 1)) return true;
  }
  return false;
}

bool type_needs_deep_copy(CompilerContext* cctx, const AstModule* module,
                          const AstTypeRef* type, int depth) {
  if (!type || depth > 32) return false;
  if (type->is_view || type->is_mod) return false;
  if (type->is_opt) {
    AstTypeRef inner = *type;
    inner.is_opt = false;
    return type_needs_deep_copy(cctx, module, &inner, depth + 1);
  }
  if (is_drop_target_type(type)) return true;
  { AstTypeRef elem; if (array_element_ref(type, &elem)) return type_needs_deep_copy(cctx, module, &elem, depth + 1); }
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "String")) return true;
  /* Any / RaeAny is an opaque box — shallow assignment is fine
   * because the heap (if any) is reference-counted at the value
   * level. */
  if (str_eq_cstr(base, "Any") || str_eq_cstr(base, "RaeAny")) return false;
  const AstDecl* d = find_type_decl(NULL, module, base);
  if (!d || d->kind != AST_DECL_TYPE) return false;
  if (has_property(d->as.type_decl.properties, "c_struct")) return false;
  const AstIdentifierPart* gp = d->as.type_decl.generic_params;
  const AstTypeRef* ga = type->generic_args;
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    AstTypeRef* concrete = f->type;
    if (gp && ga) {
      concrete = substitute_type_ref(cctx, gp, ga, f->type);
    }
    if (type_needs_deep_copy(cctx, module, concrete, depth + 1)) return true;
  }
  return false;
}
