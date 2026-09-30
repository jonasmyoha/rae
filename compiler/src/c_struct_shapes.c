/* The user structs the generated per-type helpers (toString, equals, toJson /
 * fromJson) are emitted for: every non-generic user struct, and every concrete
 * instantiation of a generic one (`Pair(String)`, `Pair(Int)`) with its field
 * types substituted. The helper generators iterate this one list, so a generic
 * instance gets exactly the helpers a plain struct gets, from the same code.
 * c_struct types are excluded (their layout is C's), and so are the stdlib
 * containers, which have their own per-T helpers (List's toString / equals) or
 * none. */
#include <string.h>

#include "c_backend_internal.h"
#include "sema.h"
#include "mangler.h"

static bool is_container_base(Str base) {
  return str_eq_cstr(base, "List") || str_eq_cstr(base, "StringMap")
      || str_eq_cstr(base, "IntMap") || str_eq_cstr(base, "Buffer")
      || str_eq_cstr(base, "Opt") || str_eq_cstr(base, "Array")
      || str_eq_cstr(base, "ComponentTable") || str_eq_cstr(base, "Ptr");
}

/* The generic template of a type ref: a user struct declared with type
 * parameters, not a c_struct. NULL for anything else. */
const AstDecl* generic_struct_template(CompilerContext* ctx, const AstTypeRef* type) {
  if (!type || !type->generic_args || type->is_view || type->is_mod || type->is_opt) return NULL;
  Str base = get_base_type_name(type);
  if (is_container_base(base)) return NULL;
  for (size_t k = 0; k < ctx->all_decl_count; k++) {
    const AstDecl* d = ctx->all_decls[k];
    if (d->kind != AST_DECL_TYPE || d->as.type_decl.specialization_args) continue;
    if (!str_eq(d->as.type_decl.name, base)) continue;
    if (!d->as.type_decl.generic_params) return NULL;
    if (has_property(d->as.type_decl.properties, "c_struct")) return NULL;
    return d;
  }
  return NULL;
}

static bool shape_seen(const StructShape* shapes, size_t count, const char* mangled) {
  for (size_t k = 0; k < count; k++)
    if (strcmp(shapes[k].mangled, mangled) == 0) return true;
  return false;
}

size_t collect_struct_shapes(CompilerContext* ctx, StructShape** out_shapes) {
  size_t cap = ctx->all_decl_count + ctx->generic_type_count + 1;
  StructShape* shapes = arena_alloc(ctx->ast_arena, sizeof(StructShape) * cap);
  size_t count = 0;
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
    if (has_property(d->as.type_decl.properties, "c_struct")) continue;
    const AstTypeDecl* td = &d->as.type_decl;
    if (earlier_same_named_type(ctx, i, td->name)) continue;
    const char* mangled = rae_mangle_type_specialized(
        ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = td->name}});
    if (!mangled || shape_seen(shapes, count, mangled)) continue;
    shapes[count].mangled = mangled;
    shapes[count].decl = (AstTypeDecl){.name = td->name, .fields = td->fields};
    count++;
  }
  for (size_t i = 0; i < ctx->generic_type_count; i++) {
    const AstTypeRef* gt = ctx->generic_types[i];
    const AstDecl* tmpl = generic_struct_template(ctx, gt);
    if (!tmpl) continue;
    const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)gt);
    if (!mangled || shape_seen(shapes, count, mangled)) continue;
    const AstTypeDecl* td = &tmpl->as.type_decl;
    AstTypeField* head = NULL;
    AstTypeField* tail = NULL;
    for (const AstTypeField* f = td->fields; f; f = f->next) {
      AstTypeField* nf = arena_alloc(ctx->ast_arena, sizeof(AstTypeField));
      *nf = *f;
      nf->next = NULL;
      nf->type = substitute_type_ref(ctx, td->generic_params, gt->generic_args, f->type);
      if (!head) head = tail = nf; else { tail->next = nf; tail = nf; }
    }
    shapes[count].mangled = mangled;
    shapes[count].decl = (AstTypeDecl){.name = td->name, .fields = head};
    count++;
  }
  *out_shapes = shapes;
  return count;
}
