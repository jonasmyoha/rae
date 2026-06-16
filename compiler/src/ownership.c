/* Backend-neutral ownership classifiers. See ownership.h for the
 * intent. Bodies are lifted verbatim from c_stmt.c — no behaviour
 * change. */
#include "ownership.h"

#include "ast.h"
#include "sema.h"
#include "str.h"

/* `has_property` and `find_type_decl` are AST utilities that
 * currently live in c_backend.c (declared in c_backend_internal.h).
 * They're pure walks of the AST — not C-backend-specific — but
 * relocating them is a polish step deferred to a future commit. */
#include "c_backend_internal.h"

bool is_drop_target_type(const AstTypeRef* type) {
  if (!type) return false;
  /* Borrows don't own — they're someone else's value. */
  if (type->is_view || type->is_mod) return false;
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
  if (is_drop_target_type(type)) return true;
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
  (void)cctx;
  if (!type || depth > 32) return false;
  if (type->is_view || type->is_mod) return false;
  if (is_drop_target_type(type)) return true;
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "String")) return true;
  const AstDecl* d = find_type_decl(NULL, module, base);
  if (!d || d->kind != AST_DECL_TYPE) return false;
  if (has_property(d->as.type_decl.properties, "c_struct")) return false;
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    if (type_needs_cascade_drop(cctx, module, f->type, depth + 1)) return true;
  }
  return false;
}

bool type_needs_deep_copy(CompilerContext* cctx, const AstModule* module,
                          const AstTypeRef* type, int depth) {
  (void)cctx;
  if (!type || depth > 32) return false;
  if (type->is_view || type->is_mod) return false;
  if (is_drop_target_type(type)) return true;
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "String")) return true;
  /* Any / RaeAny is an opaque box — shallow assignment is fine
   * because the heap (if any) is reference-counted at the value
   * level. */
  if (str_eq_cstr(base, "Any") || str_eq_cstr(base, "RaeAny")) return false;
  const AstDecl* d = find_type_decl(NULL, module, base);
  if (!d || d->kind != AST_DECL_TYPE) return false;
  if (has_property(d->as.type_decl.properties, "c_struct")) return false;
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    if (type_needs_deep_copy(cctx, module, f->type, depth + 1)) return true;
  }
  return false;
}
