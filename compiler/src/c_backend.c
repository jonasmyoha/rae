#include "c_backend.h"
#include "c_backend_internal.h"
#include "mangler.h"
#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vm_registry.h"
#include "lexer.h"
#include "diag.h"

// Forward declarations for buffer primitives
void* rae_ext_rae_buf_alloc(int64_t size);
void rae_ext_rae_buf_free(void* ptr);

typedef struct {
  int64_t next;
} TickCounter;

bool emitted_list_contains(EmittedTypeList* list, const char* name) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], name) == 0) return true;
    }
    return false;
}

void emitted_list_add(EmittedTypeList* list, const char* name) {
    if (list->count < list->capacity) {
        list->items[list->count++] = name;
    }
}

// (forward declarations live in c_backend_internal.h)

// --- Value-optional representation ---------------------------------------
//
// `opt T` (value optional, NOT `opt view/mod T`) has two C lowerings:
//   * every non-`Any` payload — structs, List/Map instances, Task, Array,
//     scalars (Int/Float/Float64/Bool/Char), String and enums-as-Int -> a
//     monomorphized `struct rae_opt_<T> { rae_Bool has; T value; }`
//     (malloc-free, mirrors List(T) monomorphization).
//   * `Any` alone keeps the inline `RaeAny` union — RaeAny is reserved for
//     type-erased `Any` (#651). `Buffer`/`Void` also stay RaeAny (they are
//     not value-opt payloads in practice).
// The struct-rep decision MUST agree with both name-manglers (type.c and
// mangler.c) or a local var decl won't match the struct typedef (#238).
bool rae_typeinfo_opt_is_struct_rep(const TypeInfo* base) {
    if (!base) return false;
    switch (base->kind) {
        case TYPE_STRUCT:
        case TYPE_GENERIC_INST:
        case TYPE_TASK:
        case TYPE_ARRAY:
        // #651: scalars, String and enums (enums resolve to TYPE_INT) all move
        // off RaeAny onto the monomorphized struct rep.
        case TYPE_INT:
        case TYPE_FLOAT:
        case TYPE_FLOAT64:
        case TYPE_BOOL:
        case TYPE_CHAR:
        case TYPE_STRING:
            return true;
        default:
            return false;
    }
}

bool rae_opt_is_struct_rep(CFuncContext* ctx, const AstTypeRef* type) {
    if (!type) return false;
    const TypeInfo* base = type->resolved_type;
    if (base && base->kind == TYPE_OPT) base = base->as.opt.base;
    /* A generic param payload: resolve through the current specialization. */
    if (base && base->kind == TYPE_GENERIC_PARAM && ctx
        && ctx->generic_params && ctx->generic_args) {
        const AstIdentifierPart* gp = ctx->generic_params;
        const AstTypeRef* ga = ctx->generic_args;
        while (gp && ga) {
            if (str_eq(gp->text, base->as.generic_param.param_name))
                return rae_opt_is_struct_rep(ctx, ga);
            gp = gp->next; ga = ga->next;
        }
    }
    if (base && base->kind != TYPE_OPT && base->kind != TYPE_GENERIC_PARAM)
        return rae_typeinfo_opt_is_struct_rep(base);
    /* Fallback (unresolved clone / no TypeInfo): name-based. Every non-`Any`
     * payload is struct-rep (#651) — scalars, String, enums and user structs.
     * Only `Any`/`RaeAny` (and the non-payload `Buffer`/`Void`) keep RaeAny. */
    Str nm = get_base_type_name(type);
    if (nm.len == 0) return false;
    if (str_eq_cstr(nm, "Any") || str_eq_cstr(nm, "RaeAny")
        || str_eq_cstr(nm, "Buffer") || str_eq_cstr(nm, "Void")
        || str_eq_cstr(nm, "void")) return false;
    return true;
}

// Mangled C name of the `rae_opt_<T>` struct for a value-optional type.
const char* rae_opt_type_name(CFuncContext* ctx, const AstTypeRef* opt_type) {
    return rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params,
                                       ctx->generic_args, opt_type);
}

void emit_type_info_as_c_type(CFuncContext* ctx, TypeInfo* t, FILE* out) {
    if (!t) { fprintf(out, "RaeAny"); return; }
    AstTypeRef tmp = {0};
    tmp.resolved_type = t;
    emit_type_ref_as_c_type(ctx, &tmp, out, false);
}

bool emit_type_recursive(CompilerContext* ctx, const AstModule* m, const AstTypeRef* type, FILE* out, EmittedTypeList* emitted, EmittedTypeList* visiting, bool ray) {
    if (!type) return true;

    // Value optional over an aggregate payload: emit the payload struct first,
    // then `struct rae_opt_<T> { rae_Bool has; T value; }`. Mirrors List(T).
    if (type->is_opt && !type->is_view && !type->is_mod) {
        CFuncContext octx = {0}; octx.compiler_ctx = ctx; octx.module = m; octx.uses_raylib = ray;
        if (rae_opt_is_struct_rep(&octx, type)) {
            const char* optm = rae_mangle_type_specialized(ctx, NULL, NULL, type);
            if (emitted_list_contains(emitted, optm) || emitted_list_contains(visiting, optm)) return true;
            emitted_list_add(visiting, optm);
            AstTypeRef payload = *type;
            payload.is_opt = false; payload.next = NULL;
            payload.resolved_type = (type->resolved_type && type->resolved_type->kind == TYPE_OPT)
                ? type->resolved_type->as.opt.base
                : (payload.parts ? NULL : type->resolved_type);
            emit_type_recursive(ctx, m, &payload, out, emitted, visiting, ray);
            fprintf(out, "typedef struct %s %s;\n", optm, optm);
            fprintf(out, "struct %s {\n  rae_Bool has;\n  ", optm);
            emit_type_ref_as_c_type(&octx, &payload, out, false);
            fprintf(out, " value;\n};\n\n");
            emitted_list_add(emitted, optm);
            if (visiting->count > 0) visiting->count--;
            return true;
        }
    }

    if (type->resolved_type) {
        if (type->resolved_type->kind == TYPE_ARRAY) {
            /* Emit the element type first, then the wrapper struct. Handled
             * ahead of the generic-argument scan below, which would bail out
             * on the `cap:` value argument (it has no base type name). */
            TypeInfo* at = type->resolved_type;
            const char* amangled = type_mangle_name(ctx->ast_arena, at).data;
            if (emitted_list_contains(emitted, amangled)) return true;
            if (type->generic_args) emit_type_recursive(ctx, m, type->generic_args, out, emitted, visiting, ray);
            AstTypeRef elem = {0}; elem.resolved_type = at->as.array.base;
            CFuncContext tctx = {0}; tctx.compiler_ctx = ctx; tctx.module = m; tctx.uses_raylib = ray;
            fprintf(out, "typedef struct { ");
            emit_type_ref_as_c_type(&tctx, &elem, out, false);
            fprintf(out, " v[%lld]; } %s;\n\n", (long long)(at->as.array.count > 0 ? at->as.array.count : 1), amangled);
            emitted_list_add(emitted, amangled);
            return true;
        }
        if (type->resolved_type->kind == TYPE_BUFFER) {
            // Buffer(T) - T might need registration but Buffer is a pointer
            if (type->generic_args) emit_type_recursive(ctx, m, type->generic_args, out, emitted, visiting, ray);
            return true;
        }
        if (type->resolved_type->kind < TYPE_STRUCT) return true;
    }

    Str base = {0};
    if (type->parts) base = type->parts->text;
    else if (type->resolved_type) base = type->resolved_type->name;
    
    if (base.len == 0) return true;
    if (is_primitive_type(base) || (ray && is_raylib_builtin_type(base))) return true;
    // Skip spurious void/Any specializations
    for (const AstTypeRef* ga = type->generic_args; ga; ga = ga->next) {
        Str ga_base = get_base_type_name(ga);
        if (str_eq_cstr(ga_base, "void") || ga_base.len == 0) return true;
    }
    // Skip c_struct types (raylib types defined externally)
    { const AstDecl* td = find_type_decl(NULL, m, base);
      if (td && td->kind == AST_DECL_TYPE && has_property(td->as.type_decl.properties, "c_struct")) return true; }

    const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, type);
    if (emitted_list_contains(emitted, mangled)) return true;
    if (emitted_list_contains(visiting, mangled)) return true;
    
    emitted_list_add(visiting, mangled);
    
    // Find the declaration
    if (str_eq_cstr(base, "List") || str_eq_cstr(base, "Buffer")) {
        // Built-in List/Buffer — recursively emit element type first
        if (type->generic_args) emit_type_recursive(ctx, m, type->generic_args, out, emitted, visiting, ray);
        fprintf(out, "typedef struct %s %s;\n", mangled, mangled);
        fprintf(out, "struct %s {\n", mangled);
        CFuncContext tctx = {0}; tctx.compiler_ctx = ctx; tctx.module = m; tctx.uses_raylib = ray;
        fprintf(out, "  ");
        emit_type_ref_as_c_type(&tctx, type->generic_args, out, false);
        fprintf(out, "* data;\n  int64_t length;\n  int64_t cap;\n};\n\n");
    } else {
        const AstDecl* d = find_type_decl(NULL, m, base);
        // If we found a specialized version, use the generic template instead
        // (so fields have T not substituted types, and we apply our own substitution)
        if (d && d->kind == AST_DECL_TYPE && d->as.type_decl.specialization_args && d->as.type_decl.generic_template)
            d = d->as.type_decl.generic_template;
        if (d && d->kind == AST_DECL_TYPE) {
            const AstTypeDecl* td = &d->as.type_decl;
            const AstIdentifierPart* params = td->generic_params;
            const AstTypeRef* args = type->generic_args;
            if (!params && d->as.type_decl.generic_template) params = d->as.type_decl.generic_template->as.type_decl.generic_params;
            
            // Dependencies — also recurse into Buffer element types
            for (const AstTypeField* f = td->fields; f; f = f->next) {
                if (!f->type || f->type->is_view || f->type->is_mod) continue;
                AstTypeRef* sub = substitute_type_ref(ctx, params, args, f->type);
                emit_type_recursive(ctx, m, sub, out, emitted, visiting, ray);
                // If the field is Buffer(X), also emit X
                Str fbase = get_base_type_name(sub);
                if ((str_eq_cstr(fbase, "Buffer") || str_eq_cstr(fbase, "List")) && sub->generic_args) {
                    emit_type_recursive(ctx, m, sub->generic_args, out, emitted, visiting, ray);
                }
            }
            
            if (!has_property(td->properties, "c_struct")) {
                // Skip structs with void fields (spurious specializations)
                bool has_void = false;
                for (const AstTypeField* fv = td->fields; fv; fv = fv->next) {
                    if (fv->type) {
                        AstTypeRef* fsub = substitute_type_ref(ctx, params, args, fv->type);
                        Str fb = get_base_type_name(fsub);
                        if (str_eq_cstr(fb, "void") || fb.len == 0) { has_void = true; break; }
                    }
                }
                if (has_void) { emitted_list_add(emitted, mangled); visiting->count--; return true; }
                fprintf(out, "typedef struct %s %s;\n", mangled, mangled);
                fprintf(out, "struct %s {\n", mangled);
                CFuncContext tctx = {0}; tctx.compiler_ctx = ctx; tctx.module = m; tctx.uses_raylib = ray;
                tctx.generic_params = params; tctx.generic_args = args;
                for (const AstTypeField* f = td->fields; f; f = f->next) {
                    fprintf(out, "  ");
                    emit_type_ref_as_c_type(&tctx, f->type, out, false);
                    bool p = f->type && (f->type->is_view || f->type->is_mod);
                    fprintf(out, "%s %.*s;\n", p ? "*" : "", (int)f->name.len, f->name.data);
                }
                // #751: a zero-field struct (a tag/marker component, e.g.
                // `type Tag {}`) would emit an EMPTY C struct — invalid in ISO C
                // (and `{0}`-initializing it errors). Lower it to a one-byte
                // struct so it is legal and value-copies/zero-inits cleanly. The
                // synthesized toJson/fromJson/copy/drop iterate the Rae fields
                // (none), so they never touch this placeholder.
                if (!td->fields) {
                    fprintf(out, "  int8_t _rae_empty;\n");
                }
                fprintf(out, "};\n\n");
            }
        }
    }

    emitted_list_add(emitted, mangled);
    visiting->count--;
    return true;
}

/* Forward declarations for these now live in c_backend_internal.h. */
bool is_primitive_ref(CFuncContext* ctx, const AstTypeRef* tr) {
    if (!tr || !(tr->is_view || tr->is_mod)) return false;
    Str base = get_base_type_name(tr);
    // #758: resolve a generic param (`view T`) to its concrete type before the
    // primitive/String tests, so a specialised `view T`(=String) is recognised
    // as a wrapper ref (rae_View_String, read `(*x.ptr)`), not misclassified as
    // a raw struct pointer — mirroring the concrete `view String` lowering.
    if (ctx && ctx->generic_params && ctx->generic_args) {
        const AstIdentifierPart* gp = ctx->generic_params;
        const AstTypeRef* ga = ctx->generic_args;
        while (gp && ga) {
            if (str_eq(gp->text, base)) { base = get_base_type_name(ga); break; }
            gp = gp->next; ga = ga->next;
        }
    }
    // Buffer and List are already pointers — no wrapper struct
    if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List") || str_eq_cstr(base, "Any")) return false;
    // Stage 6: for plain numeric/bool/char primitives, `view T` lowers
    // to the same pass-by-value machine code as bare T — the source-
    // level `view` is semantic intent only. Only `mod T` keeps a true
    // reference wrapper, because the callee must write back through
    // the pointer. String stays a ref under view/mod because it is
    // heap-owning at the language level (the ref avoids deep copies).
    bool is_num_prim = str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64") ||
        str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32") || str_eq_cstr(base, "Float64") ||
        str_eq_cstr(base, "Bool") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32");
    if (is_num_prim) return tr->is_mod;
    if (str_eq_cstr(base, "String")) return true;
    return false;
}


bool has_property(const AstProperty* props, const char* name) {
  while (props) { if (str_eq_cstr(props->name, name)) return true; props = props->next; }
  return false;
}

static const AstModule* g_find_module_stack[64];
static size_t g_find_module_stack_count = 0;

bool types_match(Str a, Str b) {
  if (str_eq(a, b)) return true;
  if (str_eq_cstr(a, "String") && (str_eq_cstr(b, "const char*") || str_eq_cstr(b, "rae_String"))) return true;
  if (str_eq_cstr(b, "String") && (str_eq_cstr(a, "const char*") || str_eq_cstr(a, "rae_String"))) return true;
  if (str_eq_cstr(a, "String") && str_eq_cstr(b, "const_char_p")) return true;
  if (str_eq_cstr(b, "String") && str_eq_cstr(a, "const_char_p")) return true;
  return false;
}

const AstDecl* find_type_decl(CFuncContext* ctx, const AstModule* module, Str name) {
  // Prefer the generic template over specialisation clones — both share the
  // same `name`, but a spec clone has already-substituted field types which
  // would mislead substitution at the caller. Pass 1: template/non-generic.
  // Pass 2: anything that matches.
  if (ctx && ctx->compiler_ctx) {
      for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
          const AstDecl* decl = ctx->compiler_ctx->all_decls[i];
          if (decl->kind == AST_DECL_TYPE && !decl->as.type_decl.specialization_args &&
              types_match(decl->as.type_decl.name, name)) return decl;
      }
      for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
          const AstDecl* decl = ctx->compiler_ctx->all_decls[i];
          if (decl->kind == AST_DECL_TYPE && types_match(decl->as.type_decl.name, name)) return decl;
      }
  }
  if (!module) return NULL;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
      if (decl->kind == AST_DECL_TYPE && !decl->as.type_decl.specialization_args &&
          types_match(decl->as.type_decl.name, name)) return decl;
  }
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) { if (decl->kind == AST_DECL_TYPE && types_match(decl->as.type_decl.name, name)) return decl; }
  for (size_t i = 0; i < g_find_module_stack_count; i++) if (g_find_module_stack[i] == module) return NULL;
  if (g_find_module_stack_count >= 64) return NULL;
  g_find_module_stack[g_find_module_stack_count++] = module;
  const AstDecl* found = NULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) { found = find_type_decl(ctx, imp->module, name); if (found) break; }
  g_find_module_stack_count--; return found;
}

const AstDecl* find_enum_decl(CFuncContext* ctx, const AstModule* module, Str name) {
  if (ctx && ctx->compiler_ctx) {
      for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
          const AstDecl* decl = ctx->compiler_ctx->all_decls[i];
          if (decl->kind == AST_DECL_ENUM && types_match(decl->as.enum_decl.name, name)) return decl;
      }
  }
  if (!module) return NULL;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) { if (decl->kind == AST_DECL_ENUM && types_match(decl->as.enum_decl.name, name)) return decl; }
  for (size_t i = 0; i < g_find_module_stack_count; i++) if (g_find_module_stack[i] == module) return NULL;
  if (g_find_module_stack_count >= 64) return NULL;
  g_find_module_stack[g_find_module_stack_count++] = module;
  const AstDecl* found = NULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) { found = find_enum_decl(ctx, imp->module, name); if (found) break; }
  g_find_module_stack_count--; return found;
}

void register_decl(CompilerContext* ctx, const AstDecl* decl) {
    if (!decl) return;
    for (size_t i = 0; i < ctx->all_decl_count; i++) { if (ctx->all_decls[i] == decl) return; }
    if (ctx->all_decl_count < ctx->all_decl_cap) ctx->all_decls[ctx->all_decl_count++] = decl;
}

void collect_decls_from_module(CompilerContext* ctx, const AstModule* module) {
    if (!module) return;
    if (module->decls) { for (size_t i = 0; i < ctx->all_decl_count; i++) { if (ctx->all_decls[i] == module->decls) return; } }
    for (const AstDecl* decl = module->decls; decl; decl = decl->next) register_decl(ctx, decl);
    for (const AstImport* imp = module->imports; imp; imp = imp->next) collect_decls_from_module(ctx, imp->module);
}

int g_type_equal_depth = 0;
bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->resolved_type && b->resolved_type && a->resolved_type == b->resolved_type) return true;
    if ((uintptr_t)a < 0x1000 || (uintptr_t)b < 0x1000) return false;
    if (g_type_equal_depth > 32) return false;
    g_type_equal_depth++;
    bool res = false;
    if (a->parts && b->parts) {
        if ((uintptr_t)a->parts < 0x1000 || (uintptr_t)b->parts < 0x1000) { res = false; goto done; }
        if (!str_eq(a->parts->text, b->parts->text)) { res = false; goto done; }
    } else if (a->parts != b->parts) { res = false; goto done; }
    if (a->is_opt != b->is_opt || a->is_view != b->is_view || a->is_mod != b->is_mod) { res = false; goto done; }
    const AstTypeRef* arg_a = a->generic_args; const AstTypeRef* arg_b = b->generic_args;
    while (arg_a && arg_b) { if (!type_refs_equal(arg_a, arg_b)) { res = false; goto done; } arg_a = arg_a->next; arg_b = arg_b->next; }
    res = (arg_a == arg_b);
done:
    g_type_equal_depth--; return res;
}

bool is_concrete_type(const AstTypeRef* type) {
    if (!type) return true;
    if (type->parts && !type->parts->next) { Str base = type->parts->text; if (base.len == 1 && base.data[0] >= 'A' && base.data[0] <= 'Z') return false; }
    const AstTypeRef* arg = type->generic_args;
    while (arg) { if (!is_concrete_type(arg)) return false; arg = arg->next; }
    return true;
}

void register_function_specialization(CompilerContext* ctx, const AstFuncDecl* decl, const AstTypeRef* concrete_args) {
    if (!decl || !concrete_args) return;
    for (const AstTypeRef* arg = concrete_args; arg; arg = arg->next) {
        if ((uintptr_t)arg->parts < 0x1000 && (uintptr_t)arg->parts != 0) return;
        if (!is_concrete_type(arg)) return;
    }
    for (size_t i = 0; i < ctx->specialized_func_count; i++) {
        if (ctx->specialized_funcs[i].decl == decl) {
            const AstTypeRef* a = ctx->specialized_funcs[i].concrete_args; const AstTypeRef* b = concrete_args;
            bool match = true; while (a && b) { if (!type_refs_equal(a, b)) { match = false; break; } a = a->next; b = b->next; }
            if (match && !a && !b) return;
        }
    }
    if (ctx->specialized_func_count < ctx->specialized_func_cap) {
        ctx->specialized_funcs[ctx->specialized_func_count].decl = decl;
        ctx->specialized_funcs[ctx->specialized_func_count].concrete_args = (AstTypeRef*)concrete_args;
        ctx->specialized_func_count++;
    }
}

void register_generic_type(CompilerContext* ctx, const AstTypeRef* type) {
    if (!type || !is_concrete_type(type)) return;
    if ((uintptr_t)type < 0x1000) return;
    /* A `cap: N` value argument is not a type and has nothing to register. */
    if (type->is_value_arg) return;
    // Don't register types with void generic args (spurious specializations)
    for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
        Str ab = get_base_type_name(a);
        if (str_eq_cstr(ab, "void") || (a->resolved_type && a->resolved_type->kind == TYPE_VOID)) return;
    }
    // Value optional over an aggregate payload: also register the bare payload
    // so its own struct/drop/copy helpers are emitted (the opt struct's drop
    // and deep-copy call into them).
    if (type->is_opt && !type->is_view && !type->is_mod) {
        CFuncContext octx = {0}; octx.compiler_ctx = ctx; octx.module = ctx->current_module;
        if (rae_opt_is_struct_rep(&octx, type)) {
            AstTypeRef* payload = (AstTypeRef*)malloc(sizeof(AstTypeRef));
            *payload = *type;
            payload->is_opt = false;
            payload->next = NULL;
            payload->resolved_type = (type->resolved_type && type->resolved_type->kind == TYPE_OPT)
                ? type->resolved_type->as.opt.base
                : (payload->parts ? NULL : type->resolved_type);
            register_generic_type(ctx, payload);
        }
    }
    if (type->resolved_type && type->resolved_type->kind < TYPE_STRUCT) {
        if (type->resolved_type->kind == TYPE_BUFFER) { for (const AstTypeRef* arg = type->generic_args; arg; arg = arg->next) register_generic_type(ctx, arg); }
        return;
    }
    Str base = {0};
    if (type->parts) base = type->parts->text; else if (type->resolved_type) base = type->resolved_type->name;
    if (base.len > 0) { if (str_eq_cstr(base, "Void") || str_eq_cstr(base, "void") || is_primitive_type(base)) return; }
    for (size_t i = 0; i < ctx->generic_type_count; i++) { if (type_refs_equal(ctx->generic_types[i], type)) goto scan_args; }
    if (ctx->generic_type_count < ctx->generic_type_cap) ctx->generic_types[ctx->generic_type_count++] = type;
scan_args:
    for (const AstTypeRef* arg = type->generic_args; arg; arg = arg->next) register_generic_type(ctx, arg);
    bool is_list = str_eq_cstr(base, "List");
    bool is_buffer = (type->resolved_type && type->resolved_type->kind == TYPE_BUFFER) || str_eq_cstr(base, "Buffer");
    if (is_buffer || is_list || str_eq_cstr(base, "Box")) return;
    const AstDecl* d = NULL;
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        const AstDecl* ad = ctx->all_decls[i];
        if (ad->kind == AST_DECL_TYPE && types_match(ad->as.type_decl.name, base)) { d = ad; break; }
    }
    if (!d && ctx->current_module) d = find_type_decl(NULL, ctx->current_module, base);
    if (d && d->kind == AST_DECL_TYPE) {
        for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
            if (f->type) {
                const AstIdentifierPart* params = d->as.type_decl.generic_params; const AstTypeRef* args = type->generic_args;
                if (!params && d->as.type_decl.generic_template) params = d->as.type_decl.generic_template->as.type_decl.generic_params;
                AstTypeRef* sub = substitute_type_ref(ctx, params, args, f->type); register_generic_type(ctx, sub);
            }
        }
    }
}

const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count, bool is_method, const AstExpr* call_expr) {
    if (!module) return NULL;
    
    for (const AstDecl* d = module->decls; d; d = d->next) {
        if (d->kind == AST_DECL_FUNC) {
            const AstFuncDecl* fd = &d->as.func_decl;
            if (str_eq(fd->name, name)) {
                uint16_t fd_param_count = 0;
                for (const AstParam* p = fd->params; p; p = p->next) fd_param_count++;
                
                if (fd_param_count == param_count) {
                    if (is_method && fd->params) {
                        TypeInfo* fd_rec_t = sema_resolve_type(ctx->compiler_ctx, fd->params->type);
                        Str obj_type = {0};
                        if (param_types && param_types[0].len > 0) obj_type = param_types[0];
                        else if (call_expr && call_expr->kind == AST_EXPR_METHOD_CALL) {
                            const AstTypeRef* tr = infer_expr_type_ref(ctx, call_expr->as.method_call.object);
                            if (tr) {
                                const char* m = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
                                obj_type = str_from_cstr(m);
                            }
                        }
                        
                        if (obj_type.len > 0) {
                            if (types_match(fd_rec_t->name, obj_type)) return fd;
                            const char* mfd = rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, fd->params->type);
                            if (str_eq_cstr(obj_type, mfd)) return fd;
                            // For generic methods, the mangled template (e.g. "rae_List_rae_T") never
                            // matches the receiver's concrete name (e.g. "rae_List_int64_t").
                            // Accept a match when the template base equals the receiver's base.
                            if (fd->generic_params) {
                                Str base = get_base_type_name(fd->params->type);
                                if (base.len > 0) {
                                    char prefix[256];
                                    int n = snprintf(prefix, sizeof(prefix), "rae_%.*s", (int)base.len, base.data);
                                    if ((size_t)n < sizeof(prefix)) {
                                        if (str_eq_cstr(obj_type, prefix)) return fd;
                                        if (obj_type.len > (size_t)n + 1 &&
                                            memcmp(obj_type.data, prefix, n) == 0 &&
                                            obj_type.data[n] == '_') return fd;
                                    }
                                }
                            }
                        }
                    } else if (!is_method) {
                        return fd;
                    }
                }
            }
        }
    }

    // Search in imported modules
    for (const AstImport* imp = module->imports; imp; imp = imp->next) {
        if (!imp->module) continue;
        const AstFuncDecl* found = find_function_overload(imp->module, ctx, name, param_types, param_count, is_method, call_expr);
        if (found) return found;
    }

    return NULL;
}


int binary_op_precedence(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_ADD: return PREC_ADD;
    case AST_BIN_SUB: return PREC_ADD;
    case AST_BIN_MUL: return PREC_MUL;
    case AST_BIN_DIV: return PREC_MUL;
    case AST_BIN_MOD: return PREC_MUL;
    case AST_BIN_LT: return PREC_RELATIONAL;
    case AST_BIN_GT: return PREC_RELATIONAL;
    case AST_BIN_LE: return PREC_RELATIONAL;
    case AST_BIN_GE: return PREC_RELATIONAL;
    case AST_BIN_IS: return PREC_EQUALITY;
    case AST_BIN_NEQ: return PREC_EQUALITY;
    case AST_BIN_AND: return PREC_LOGICAL_AND;
    case AST_BIN_OR: return PREC_LOGICAL_OR;
    // C-accurate precedence so emitted-C parens preserve the Rae AST grouping
    // (in C, `<<`/`&` bind LOOSER than `+`, the opposite of Rae's parse-time
    // precedence — only the AST structure carries the real grouping).
    case AST_BIN_BITOR: return PREC_BITWISE_OR;
    case AST_BIN_BITXOR: return PREC_BITWISE_XOR;
    case AST_BIN_BITAND: return PREC_BITWISE_AND;
    case AST_BIN_SHL: return PREC_SHIFT;
    case AST_BIN_SHR: return PREC_SHIFT;
  }
  return PREC_LOWEST;
}



bool emit_string_literal(FILE* out, Str literal) {
  fprintf(out, "(rae_String){(uint8_t*)\"");
  for (size_t i = 0; i < literal.len; i++) {
    char c = literal.data[i];
    switch (c) {
      case '"': fprintf(out, "\\\""); break; case '\\': fprintf(out, "\\\\"); break; case '\n': fprintf(out, "\\n"); break;
      case '\r': fprintf(out, "\\r"); break; case '\t': fprintf(out, "\\t"); break;
      /* OCTAL, not hex. A C hex escape is GREEDY — it consumes every hex digit
       * that follows — so "\xc5\xa1ek" is read as \xa1e, one escape out of
       * range, and the C compiler rejects it. Any Rae literal with a non-ASCII
       * byte followed by [0-9a-fA-F] hit this: "Hosek" with an s-caron, or any
       * string with an umlaut before an 'e'. An octal escape is capped at three
       * digits, so \303\244 is unambiguous whatever follows. */
      default: { if ((unsigned char)c < 32 || (unsigned char)c > 126) fprintf(out, "\\%03o", (unsigned char)c); else fputc(c, out); break; }
    }
  }
  fprintf(out, "\", %lld}", (long long)literal.len); return true;
}

// True when `type` (a non-view/mod param type) is a heap aggregate for which
// a `rae_deep_copy_<MangledT>` helper is guaranteed to be emitted, so the
// spawn site can hand a worker a private deep copy. Covers List/StringMap/
// IntMap instances (a helper is emitted per generic container instance) and
// non-generic, non-c_struct user structs that need cascade-drop (collected
// into copy_entries). Generic-instance structs and c_structs have no helper.
bool c_spawn_arg_deepcopy_aggregate(CFuncContext* ctx, const AstTypeRef* type) {
  if (!type || type->is_view || type->is_mod) return false;
  Str base = get_base_type_name(type);
  if ((str_eq_cstr(base, "List") || str_eq_cstr(base, "StringMap")
       || str_eq_cstr(base, "IntMap")) && type->generic_args) {
    return true;
  }
  const AstDecl* td = ctx ? find_type_decl(ctx, ctx->module, base) : NULL;
  if (td && td->kind == AST_DECL_TYPE && !td->as.type_decl.generic_params
      && !has_property(td->as.type_decl.properties, "c_struct")) {
    return type_needs_cascade_drop(ctx->compiler_ctx, ctx->module,
                                   (AstTypeRef*)type, 0);
  }
  return false;
}

// Path-1/2 thread eligibility — see header. Every param must be capturable
// for the worker: passed by value in the C ABI (scalar/enum) or deep-copyable
// at the spawn site (String, heap aggregate). Pointer-into-parent shapes
// (mod, view-aggregate) stay on the sequential fallback.
bool c_spawn_threadable(CFuncContext* ctx, const AstFuncDecl* f) {
  if (!f || f->is_extern || f->generic_params || f->specialization_args) return false;
  for (const AstParam* p = f->params; p; p = p->next) {
    if (!p->type) return false;
    if (p->type->is_mod) return false;            // mod = pointer into parent
    Str base = get_base_type_name(p->type);
    bool is_scalar = str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64")
                  || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32") || str_eq_cstr(base, "Float64")
                  || str_eq_cstr(base, "Bool")
                  || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32");
    if (is_scalar) continue;                      // view-numeric is by value; own/copy/plain too
    bool is_enum = ctx && find_enum_decl(ctx, ctx->module, base) != NULL;
    if (is_enum && !p->type->is_view) continue;   // plain/own/copy enum = value; view-enum = pointer
    // own/copy/plain String: the spawn site hands the worker a private
    // rae_string_copy, so it owns a stable heap independent of the parent.
    // view/mod String is a pointer wrapper into the parent → unsafe.
    if (str_eq_cstr(base, "String") && !p->type->is_view && !p->type->is_mod) continue;
    // own/copy/plain heap aggregate (List/Map/struct): the spawn site deep
    // copies it (lvalue) or moves a fresh rvalue, so the worker owns it.
    if (c_spawn_arg_deepcopy_aggregate(ctx, p->type)) continue;
    // POD struct (no heap fields → no cascade drop): captured BY VALUE in the
    // C ABI, like a scalar, so a worker thread safely owns its copy. Covers
    // plain value structs (Vec3, Camera, ...) and c_structs. view/mod are
    // pointers into the parent and stay on the sequential path.
    if (!p->type->is_view && !p->type->is_mod) {
      const AstDecl* td = ctx ? find_type_decl(ctx, ctx->module, base) : NULL;
      if (td && td->kind == AST_DECL_TYPE
          && !type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, (AstTypeRef*)p->type, 0)) {
        continue;
      }
    }
    return false;                                 // view-aggregate, mod, or unknown → sequential
  }
  return true;
}

bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out, bool skip_ptr) {
  if (!type) { fprintf(out, "int64_t"); return true; }
  // A VALUE optional (`opt T`, not `opt view/mod T`) has a fixed C lowering
  // independent of the payload's resolved-kind dispatch below: a struct-rep
  // payload spells `rae_opt_<T>`, everything else spells `RaeAny`. Handled up
  // front so a generic-param payload (`opt T` in a template) can't lose its
  // opt-ness when the payload is substituted to a primitive.
  {
    bool res_opt = type->resolved_type && type->resolved_type->kind == TYPE_OPT;
    if ((type->is_opt || res_opt) && !type->is_view && !type->is_mod) {
      AstTypeRef ot = *type; ot.is_opt = true;
      if (rae_opt_is_struct_rep(ctx, &ot)) fprintf(out, "%s", rae_opt_type_name(ctx, &ot));
      else fprintf(out, "RaeAny");
      return true;
    }
  }
    if (type->resolved_type) {
      TypeInfo* t = type->resolved_type; bool is_ptr = (type->is_view || type->is_mod) && !skip_ptr;
      if (t->kind == TYPE_GENERIC_PARAM && ctx && ctx->generic_params && ctx->generic_args) {
          const AstIdentifierPart* gp = ctx->generic_params; const AstTypeRef* arg = ctx->generic_args;
          while (gp && arg) {
              if (str_eq(gp->text, t->as.generic_param.param_name)) {
                  AstTypeRef tmp = *arg; tmp.is_view = type->is_view; tmp.is_mod = type->is_mod;
                  return emit_type_ref_as_c_type(ctx, &tmp, out, skip_ptr);
              }
              gp = gp->next; arg = arg->next;
          }
      }
      // DEBUG:
      // fprintf(stderr, "emit_type_ref_as_c_type: kind=%d name=%.*s\n", t->kind, (int)t->name.len, t->name.data);

      if (t->kind == TYPE_INT) {
          const char* inm = rae_int_c_name(t->as.integer.bits, t->as.integer.is_unsigned);
          bool is_canonical_int = (t->as.integer.bits == 64 && !t->as.integer.is_unsigned);
          if (is_ptr && is_canonical_int) { fprintf(out, "rae_%s_Int64", type->is_mod ? "Mod" : "View"); }
          else if (is_ptr) { if (type->is_view) fprintf(out, "const "); fprintf(out, "%s", inm); /* '*' added by caller */ }
          else fprintf(out, "%s", inm);
          return true;
      }
      if (t->kind == TYPE_FLOAT) { if (is_ptr) fprintf(out, "rae_%s_Float", type->is_mod ? "Mod" : "View"); else fprintf(out, "float"); return true; }
      if (t->kind == TYPE_FLOAT64) { if (is_ptr) fprintf(out, "rae_%s_Float64", type->is_mod ? "Mod" : "View"); else fprintf(out, "double"); return true; }
      if (t->kind == TYPE_BOOL) { if (is_ptr) fprintf(out, "rae_%s_Bool", type->is_mod ? "Mod" : "View"); else fprintf(out, "rae_Bool"); return true; }
      if (t->kind == TYPE_CHAR) { if (is_ptr) fprintf(out, "rae_%s_Char", type->is_mod ? "Mod" : "View"); else fprintf(out, "uint32_t"); return true; }
      if (t->kind == TYPE_STRING) { if (is_ptr) fprintf(out, "rae_%s_String", type->is_mod ? "Mod" : "View"); else fprintf(out, "rae_String"); return true; }
      if (t->kind == TYPE_OPT && rae_typeinfo_opt_is_struct_rep(t->as.opt.base)) {
          fprintf(out, "%s", type_mangle_name(ctx->compiler_ctx->ast_arena, t).data);
          if (is_ptr) fprintf(out, "*");
          return true;
      }
      if (t->kind == TYPE_ANY || t->kind == TYPE_OPT) { fprintf(out, "RaeAny"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_ARRAY) {
          /* Array(T, cap: N) lowers to a STRUCT wrapping T[N], never a bare
           * T[N]: a bare C array decays to a pointer on assignment and
           * parameter passing, so `a = b` would copy a pointer and silently
           * reintroduce aliasing. The struct gives real by-value semantics
           * with identical layout and no ABI cost.
           * See docs/value-aggregates-and-ownership.md §1.5. */
          if (type->is_view) fprintf(out, "const ");
          fprintf(out, "%s", type_mangle_name(ctx->compiler_ctx->ast_arena, t).data);
          if (is_ptr) fprintf(out, "*");
          return true;
      }
      if (t->kind == TYPE_BUFFER) {
          if (type->is_view) fprintf(out, "const ");
          if (t->as.buffer.base->kind == TYPE_ANY || t->as.buffer.base->kind == TYPE_VOID) fprintf(out, "void*");
          else { AstTypeRef tmp = { .resolved_type = t->as.buffer.base }; emit_type_ref_as_c_type(ctx, &tmp, out, false); fprintf(out, "*"); }
          return true;
      }
      if (t->kind == TYPE_TASK) {
          // Type-erased handle: the result type T is recovered at the
          // get() call site. A Task is already a pointer; view/mod are no-ops.
          fprintf(out, "RaeTask*");
          return true;
      }
      if (t->kind == TYPE_STRUCT) {
          if (type->is_view) fprintf(out, "const ");
          // c_struct types (raylib's, and any binding's WGPU*/SDL_* etc.) emit
          // their BARE C name — the real library struct from a cheader, never a
          // rae_-prefixed redefinition. General FFI (#497), not raylib-only.
          const AstDecl* sdecl = t->as.structure.decl;
          bool is_c_struct = sdecl && sdecl->kind == AST_DECL_TYPE
              && has_property(sdecl->as.type_decl.properties, "c_struct");
          if (is_raylib_builtin_type(t->name) || is_c_struct) {
              fprintf(out, "%.*s", (int)t->name.len, t->name.data);
          } else {
              const char* name = type_mangle_name(ctx->compiler_ctx->ast_arena, t).data;
              fprintf(out, "%s", name);
          }
          if (is_ptr) fprintf(out, "*");
          return true;
      }
  }
  if (!type->parts) { fprintf(out, "int64_t"); return true; }
  bool is_ptr = (type->is_view || type->is_mod) && !skip_ptr;
  // `opt view T` / `opt mod T` (spec 4.1) lower to the SAME reference wrapper as
  // `view T` / `mod T`, with a null `.ptr` meaning `none`. A reference is
  // already a pointer, and "might not be there" is what a null pointer means --
  // so an optional reference costs 8 bytes and no allocation, where boxing it
  // into RaeAny would cost 48 and, for anything wider than the union, a malloc.
  //
  // Only a NON-reference optional needs the box.
  if (type->is_opt && !(type->is_view || type->is_mod)) {
    if (rae_opt_is_struct_rep(ctx, type)) fprintf(out, "%s", rae_opt_type_name(ctx, type));
    else fprintf(out, "RaeAny");
    return true;
  }
  Str base = type->parts->text; bool is_mod = type->is_mod;
  if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int")) { if (is_ptr) fprintf(out, "rae_%s_Int64", is_mod ? "Mod" : "View"); else fprintf(out, "int64_t"); return true; }
  if (str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) { if (is_ptr) fprintf(out, "rae_%s_Float", is_mod ? "Mod" : "View"); else fprintf(out, "float"); return true; }
  if (str_eq_cstr(base, "Float64")) { if (is_ptr) fprintf(out, "rae_%s_Float64", is_mod ? "Mod" : "View"); else fprintf(out, "double"); return true; }
  if (str_eq_cstr(base, "Bool")) { if (is_ptr) fprintf(out, "rae_%s_Bool", is_mod ? "Mod" : "View"); else fprintf(out, "rae_Bool"); return true; }
  if (str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) { if (is_ptr) fprintf(out, "rae_%s_Char%s", is_mod ? "Mod" : "View", str_eq_cstr(base, "Char32") ? "32" : ""); else fprintf(out, "uint32_t"); return true; }
  if (str_eq_cstr(base, "String")) { if (is_ptr) fprintf(out, "rae_%s_String", is_mod ? "Mod" : "View"); else fprintf(out, "rae_String"); return true; }
  if (str_eq_cstr(base, "Any")) { if (is_ptr) fprintf(out, "%sRaeAny*", type->is_view ? "const " : ""); else fprintf(out, "RaeAny"); return true; }
  if (str_eq_cstr(base, "Buffer") && type->generic_args) {
        if (type->is_view) fprintf(out, "const ");
        Str arg_base = get_base_type_name(type->generic_args); if (str_eq_cstr(arg_base, "Any") || arg_base.len == 0) { fprintf(out, "void*"); return true; }
        emit_type_ref_as_c_type(ctx, type->generic_args, out, false); fprintf(out, "*"); return true;
  }
  if (str_eq_cstr(base, "Task")) { fprintf(out, "RaeTask*"); return true; }
  if (ctx && ctx->generic_params && ctx->generic_args) {
      const AstIdentifierPart* gp = ctx->generic_params; const AstTypeRef* arg = ctx->generic_args;
      while (gp && arg) {
          if (str_eq(gp->text, base)) {
              // #758: carry the ORIGINAL view/mod (+ skip_ptr) onto the concrete
              // type, so `view T`(=String/struct) lowers to its reference form
              // (rae_View_String / const Struct*) instead of the by-value type.
              AstTypeRef tmp = *arg; tmp.is_view = type->is_view; tmp.is_mod = type->is_mod;
              // Stage 6: `view`(not `mod`) on a NUMERIC primitive still passes by
              // value — don't re-introduce the ref wrapper when T resolves to one.
              Str cb = get_base_type_name(arg);
              bool num = str_eq_cstr(cb, "Int") || str_eq_cstr(cb, "Int64") ||
                  str_eq_cstr(cb, "Float") || str_eq_cstr(cb, "Float32") || str_eq_cstr(cb, "Float64") ||
                  str_eq_cstr(cb, "Bool") || str_eq_cstr(cb, "Char") || str_eq_cstr(cb, "Char32");
              if (num && tmp.is_view && !tmp.is_mod) tmp.is_view = false;
              return emit_type_ref_as_c_type(ctx, &tmp, out, skip_ptr);
          }
          gp = gp->next; arg = arg->next;
      }
  }
  // Check if this is an enum type — emit as int64_t
  if (ctx) {
      const AstDecl* ed = find_enum_decl(ctx, ctx->module, base);
      if (ed) { fprintf(out, "int64_t"); if (is_ptr) fprintf(out, "*"); return true; }
  }
  // Check for c_struct property types (raylib types etc.) — emit as bare name
  if (is_raylib_builtin_type(base)) {
      fprintf(out, "%.*s", (int)base.len, base.data);
      if (is_ptr) fprintf(out, "*");
      return true;
  }
  if (ctx) {
      const AstDecl* td = find_type_decl(ctx, ctx->module, base);
      if (td && td->kind == AST_DECL_TYPE && has_property(td->as.type_decl.properties, "c_struct")) {
          fprintf(out, "%.*s", (int)base.len, base.data);
          if (is_ptr) fprintf(out, "*");
          return true;
      }
  }
  const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
  if (ctx && ctx->uses_raylib && is_raylib_builtin_type(base)) {
        const AstDecl* td = find_type_decl(NULL, ctx->module, base);
        if (td && td->kind == AST_DECL_TYPE && has_property(td->as.type_decl.properties, "c_struct")) fprintf(out, "%.*s", (int)base.len, base.data);
        else if (!td) fprintf(out, "%.*s", (int)base.len, base.data); else fprintf(out, "rae_%.*s", (int)base.len, base.data);
  } else fprintf(out, "%s", mangled);
  if (is_ptr) fprintf(out, "*");
  return true;
}

bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out, bool is_extern) {
  size_t index = 0;
  for (const AstParam* p = params; p; p = p->next) {
    if (index > 0) fprintf(out, ", ");
    if (p->type) {
        bool is_mod = p->type->is_mod, is_val = p->type->is_val, is_view = p->type->is_view;
        Str base = get_base_type_name(p->type);
        // Stage 6: `view`/`copy`/`own` on a numeric primitive lowers
        // to the same plain pass-by-value type as bare T. Only `mod`
        // on a numeric primitive needs the ref wrapper. String stays
        // a ref under view/mod because String owns heap.
        bool is_num_prim = is_primitive_type(base)
            && !str_eq_cstr(base, "String")
            && !str_eq_cstr(base, "Buffer")
            && !str_eq_cstr(base, "Any");
        bool view_or_mod = is_view || is_mod;
        if (is_num_prim && view_or_mod && !is_mod) {
            // view-on-primitive collapses to bare T at the C level.
            is_view = false;
            view_or_mod = false;
        }
        bool is_ptr = is_extern ? (is_mod || is_view) : (is_mod || is_view || (!is_val && !is_primitive_type(base) && !(ctx->uses_raylib && is_raylib_builtin_type(base))));
        if (is_view && !is_ptr && !str_eq_cstr(base, "String")) fprintf(out, "const ");
        CFuncContext p_ctx = *ctx; AstTypeRef p_type = *p->type; p_type.is_view = is_view; p_type.is_mod = is_mod;
        emit_type_ref_as_c_type(&p_ctx, &p_type, out, false); fprintf(out, " %.*s", (int)p->name.len, p->name.data);
    }
    index++;
  }
  if (index == 0) fprintf(out, "void");
  return true;
}

const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func) {
  if (str_eq_cstr(func->name, "rae_ext_rae_buf_alloc") || str_eq_cstr(func->name, "__buf_alloc") || str_eq_cstr(func->name, "rae_ext_rae_buf_resize") || str_eq_cstr(func->name, "__buf_resize") || str_eq_cstr(func->name, "rae_ext_rae_str_to_cstr") || str_eq_cstr(func->name, "toCStr")) return "void*";
  if (str_eq_cstr(func->name, "rae_ext_rae_buf_free") || str_eq_cstr(func->name, "__buf_free") || str_eq_cstr(func->name, "rae_ext_rae_buf_copy") || str_eq_cstr(func->name, "__buf_copy")) return "void";
  if (func->returns && func->returns->type) {
    // An optional REFERENCE return is a nullable pointer, not a box (spec 4.1);
    // only a non-reference optional needs RaeAny.
    AstTypeRef* tr = func->returns->type;
    if (ctx && ctx->generic_params && ctx->generic_args) {
      tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params,
                               ctx->generic_args, tr);
    }
    if (tr->is_opt && !(tr->is_view || tr->is_mod)) {
      if (rae_opt_is_struct_rep(ctx, tr)) return rae_opt_type_name(ctx, tr);
      return "RaeAny";
    }
    bool is_view = tr->is_view, is_mod = tr->is_mod, is_ptr = is_view || is_mod;
    Str base = get_base_type_name(tr);
    // Check if return type is an enum — emit as int64_t
    if (ctx && ctx->module) {
        const AstDecl* ed = find_enum_decl(ctx, ctx->module, base);
        if (ed) return is_ptr ? "int64_t*" : "int64_t";
    }
    if (is_primitive_type(base)) {
        if (tr->is_opt && is_ptr) {
          const char* const_prefix = is_view ? "const " : "";
          const char* primitive = "int64_t";
          if (str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) primitive = "float";
          else if (str_eq_cstr(base, "Float64")) primitive = "double";
          else if (str_eq_cstr(base, "Bool")) primitive = "rae_Bool";
          else if (str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) primitive = "uint32_t";
          else if (str_eq_cstr(base, "String")) primitive = "rae_String";
          char* pointer_type = malloc(strlen(const_prefix) + strlen(primitive) + 2);
          sprintf(pointer_type, "%s%s*", const_prefix, primitive);
          return pointer_type;
        }
        if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int")) return is_ptr ? (is_mod ? "rae_Mod_Int64" : "rae_View_Int64") : "int64_t";
        if (str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) return is_ptr ? (is_mod ? "rae_Mod_Float" : "rae_View_Float") : "float";
        if (str_eq_cstr(base, "Float64")) return is_ptr ? (is_mod ? "rae_Mod_Float64" : "rae_View_Float64") : "double";
        if (str_eq_cstr(base, "Bool")) return is_ptr ? (is_mod ? "rae_Mod_Bool" : "rae_View_Bool") : "rae_Bool";
        if (str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) return is_ptr ? (is_mod ? "rae_Mod_Char32" : "rae_View_Char32") : "uint32_t";
        if (str_eq_cstr(base, "String")) return is_ptr ? (is_mod ? "rae_Mod_String" : "rae_View_String") : "rae_String";
    }
    // Buffer(T) is a raw pointer (no wrapper struct), like rae_ext_rae_buf_alloc's
    // return — so a Buffer-returning extern (e.g. image.loadPng) lowers to void*,
    // not the mangled rae_Buffer_<T> (which has no typedef). Mirrors the param
    // path in emit_type_ref_as_c_type.
    if (str_eq_cstr(base, "Buffer")) return "void*";
    // Ptr is the opaque pointer primitive (Buffer(void)); a Ptr-returning Rae
    // function lowers to void*, not the phantom rae_Ptr. Mirrors sema resolving
    // Ptr to Buffer(void) and the param path.
    if (str_eq_cstr(base, "Ptr")) return "void*";
    const char* m = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
    if (strcmp(m, "RaeAny") == 0) return "RaeAny";
    if (strcmp(m, "rae_Int64") == 0 || strcmp(m, "int64_t") == 0) return "int64_t";
    if (strcmp(m, "rae_Bool") == 0) return "rae_Bool";
    if (strcmp(m, "rae_String") == 0) return "rae_String";
    if (is_ptr) { char* b = malloc(strlen(m) + 16); sprintf(b, "%s%s*", is_view ? "const " : "", m); return b; }
    return m;
  }
  return func_has_return_value(func) ? "int64_t" : "void";
}

bool func_has_return_value(const AstFuncDecl* func) { return func->returns != NULL; }
Str get_local_type_name(CFuncContext* ctx, Str name) { for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_types[i]; return (Str){0}; }
const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name) { for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_type_refs[i]; return NULL; }

bool emit_auto_init(CFuncContext* ctx, const AstTypeRef* type, FILE* out) {
    if (!type) { fprintf(out, "{0}"); return true; }
    if (type->is_opt) {
        if (!(type->is_view || type->is_mod) && rae_opt_is_struct_rep(ctx, type))
            fprintf(out, "(%s){0}", rae_opt_type_name(ctx, type));
        else fprintf(out, "rae_any_none()");
        return true;
    }
    Str base = get_base_type_name(type);
    if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int32") || str_eq_cstr(base, "UInt64") || str_eq_cstr(base, "UInt32") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, "0");
    else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) fprintf(out, "0.0");
    else if (str_eq_cstr(base, "Bool")) fprintf(out, "false");
    else if (str_eq_cstr(base, "String")) fprintf(out, "(rae_String){0}");
    /* Buffer and Ptr are emitted as C POINTERS, so their zero value is a null
     * pointer, not a braced aggregate. `{0}` compiles but earns
     * -Wbraced-scalar-init on every one -- fourteen of them in example 114
     * alone, from the List globals whose `data` field is a Buffer. */
    else if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "Ptr")) fprintf(out, "NULL");
    else {
        const AstDecl* d = find_type_decl(ctx, ctx->module, base);
        if (d && d->kind == AST_DECL_TYPE) emit_struct_auto_init(ctx, d, type, out);
        else if (find_enum_decl(ctx, ctx->module, base)) fprintf(out, "0");
        else fprintf(out, "{0}");
    }
    return true;
}

bool emit_struct_auto_init(CFuncContext* ctx, const AstDecl* decl, const AstTypeRef* tr, FILE* out) {
    fprintf(out, "{ ");
    for (const AstTypeField* f = decl->as.type_decl.fields; f; f = f->next) {
        fprintf(out, ".%.*s = ", (int)f->name.len, f->name.data);
        AstTypeRef* field_tr = substitute_type_ref(ctx->compiler_ctx, decl->as.type_decl.generic_params, tr->generic_args, f->type);
        emit_auto_init(ctx, field_tr, out); if (f->next) fprintf(out, ", ");
    }
    fprintf(out, " }"); return true;
}

bool is_pointer_type(CFuncContext* ctx, Str name) {
    if (str_eq_cstr(name, "Buffer") || str_eq_cstr(name, "List")) return true;
    for (int i = (int)ctx->local_count - 1; i >= 0; i--) {
        if (str_eq(ctx->locals[i], name)) {
            if (ctx->local_is_ptr[i]) return true;
            const AstTypeRef* tr = ctx->local_type_refs[i];
            if (tr) {
                Str base = get_base_type_name(tr);
                if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List")) return true;
            }
            return false;
        }
    }
    return false;
}

bool is_generic_param(const AstIdentifierPart* params, Str name) { const AstIdentifierPart* p = params; while (p) { if (str_eq(p->text, name)) return true; p = p->next; } return false; }

// Try to interpret an argument expression as a compile-time type
// argument. Returns the corresponding AstTypeRef* if the expression
// names a known type (primitive like `Int` / `String`, a user-
// declared `type ...`, or a parameterised type like `List(Int)`),
// otherwise NULL. Shared between emission (c_call.c) and discovery
// (c_discovery.c) so both passes see the same hoisted form of the
// new generic-call syntax:
//
//   createList(String, initialCap: 4)        // positional type arg
//   createList(type: String, initialCap: 4)  // named type arg
//   String.createList(initialCap: 4)         // dot-call on type
//
// all hoist `String` into `expr->as.call.generic_args`.
AstTypeRef* try_as_type_arg(CFuncContext* ctx, const AstExpr* val) {
    if (!val) return NULL;
    Str name = {0};
    AstCallArg* nested_args = NULL;
    if (val->kind == AST_EXPR_IDENT) {
        name = val->as.ident;
    } else if (val->kind == AST_EXPR_CALL && val->as.call.callee
               && val->as.call.callee->kind == AST_EXPR_IDENT) {
        name = val->as.call.callee->as.ident;
        nested_args = val->as.call.args;
    } else {
        return NULL;
    }
    if (name.len == 0) return NULL;
    // A local / global binding with the same name takes priority —
    // `let String = 0; foo(String, ...)` passes a value, not a type.
    if (get_local_type_ref(ctx, name)) return NULL;
    bool is_type = is_primitive_type(name) || (ctx->module && find_type_decl(ctx, ctx->module, name) != NULL);
    // Inside a generic function body, the bound generic param is also
    // a valid type expression — e.g. `createIntMap(V)` body calls
    // `createInt64Map(V, initialCap: …)` where V resolves to a type.
    if (!is_type && ctx->generic_params) {
        for (const AstIdentifierPart* gp = ctx->generic_params; gp; gp = gp->next) {
            if (str_eq(gp->text, name)) { is_type = true; break; }
        }
    }
    if (!is_type) return NULL;
    AstIdentifierPart* part = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart));
    *part = (AstIdentifierPart){.text = name};
    AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
    *tr = (AstTypeRef){.parts = part};
    if (nested_args) {
        AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
        for (AstCallArg* na = nested_args; na; na = na->next) {
            AstTypeRef* inner = try_as_type_arg(ctx, na->value);
            if (!inner) return NULL;
            if (!head) head = inner; else tail->next = inner;
            tail = inner;
        }
        tr->generic_args = head;
    }
    return tr;
}

// Hoist a type argument out of the value-arg list into generic_args.
// Returns a new AstExpr if the call had a hoistable type arg, or
// NULL to signal "use expr as-is". Accepted positions:
//   - first positional arg
//   - any named arg called `type:`
AstExpr* hoist_type_arg_if_present(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr || expr->kind != AST_EXPR_CALL) return NULL;
    if (expr->as.call.generic_args) return NULL;
    if (!expr->as.call.args) return NULL;

    // Type-arg slot recognised by shape, not by hard-coded name. The
    // first argument — positional OR named — whose value parses as a
    // type identifier is hoisted to `generic_args`. This covers all
    // three accepted call spellings:
    //   createList(T: Int, cap: 4)      — named, using the param's name
    //   createList(type: Int, cap: 4)   — legacy keyword spelling
    //   createList(Int, cap: 4)         — positional
    AstCallArg* type_arg_node = NULL;
    AstTypeRef* tr = NULL;
    if (expr->as.call.args) {
        tr = try_as_type_arg(ctx, expr->as.call.args->value);
        if (tr) type_arg_node = expr->as.call.args;
    }
    if (!type_arg_node) return NULL;

    AstCallArg* new_head = NULL; AstCallArg* new_tail = NULL;
    for (AstCallArg* a = expr->as.call.args; a; a = a->next) {
        if (a == type_arg_node) continue;
        AstCallArg* node = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstCallArg));
        *node = *a; node->next = NULL;
        if (!new_head) new_head = node; else new_tail->next = node;
        new_tail = node;
    }
    AstExpr* new_expr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstExpr));
    *new_expr = *expr;
    new_expr->as.call.generic_args = tr;
    new_expr->as.call.args = new_head;
    return new_expr;
}

// #651: synthesize an AstTypeRef carrying a sema-resolved TypeInfo, preserving
// the `opt` / `view` / `mod` qualifier. Used as a fallback for generic
// opt-returning calls (`map.get(k)`, `list.copyAt(i)`) whose template return
// type param inference otherwise drops the `opt` — leaving none-checks,
// interpolation and `if let` to mis-treat a `rae_opt_<T>` as a RaeAny box.
static const AstTypeRef* infer_tr_from_resolved(CFuncContext* ctx, const TypeInfo* ti) {
    if (!ti) return NULL;
    AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
    memset(tr, 0, sizeof(*tr));
    if (ti->kind == TYPE_REF) {
        tr->is_view = !ti->as.ref.is_mod;
        tr->is_mod = ti->as.ref.is_mod;
        ti = ti->as.ref.base;
        if (ti && ti->kind == TYPE_OPT) { tr->is_opt = true; }
        tr->resolved_type = (TypeInfo*)ti;
        return tr;
    }
    if (ti->kind == TYPE_OPT) tr->is_opt = true;
    tr->resolved_type = (TypeInfo*)ti;
    return tr;
}

const AstTypeRef* infer_expr_type_ref(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return NULL;
    // Cache primitive literal type-refs in static storage so callers can hold a
    // pointer past the function return.
    static AstIdentifierPart kInt_part = { .text = { .data = "Int", .len = 3 } };
    static AstTypeRef kInt_tr = { .parts = &kInt_part };
    static AstIdentifierPart kFloat_part = { .text = { .data = "Float", .len = 5 } };
    static AstTypeRef kFloat_tr = { .parts = &kFloat_part };
    static AstIdentifierPart kBool_part = { .text = { .data = "Bool", .len = 4 } };
    static AstTypeRef kBool_tr = { .parts = &kBool_part };
    static AstIdentifierPart kString_part = { .text = { .data = "String", .len = 6 } };
    static AstTypeRef kString_tr = { .parts = &kString_part };
    switch (expr->kind) {
        /* A cast's type IS its target — this is what stops the backend
         * re-inferring the operand's (pre-conversion) type. */
        case AST_EXPR_CAST: return expr->as.cast.target;
        /* Indexing an Array(T, cap: N) yields T. Sema already recorded it;
         * surfacing it here is what lets the ordinary assignment path see a
         * String target and wrap the RHS in rae_string_pool_take, exactly as
         * it does for a struct field. Without it, an interpolation assigned
         * into an Array element stays pool-owned and is freed twice. */
        case AST_EXPR_INDEX: {
            const TypeInfo* tt = expr->as.index.target ? expr->as.index.target->resolved_type : NULL;
            if (tt && tt->kind == TYPE_REF) tt = tt->as.ref.base;
            if (tt && tt->kind == TYPE_ARRAY && expr->resolved_type) {
                if (expr->resolved_type->kind == TYPE_STRING) return &kString_tr;
                AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
                memset(tr, 0, sizeof(*tr));
                tr->resolved_type = expr->resolved_type;
                return tr;
            }
            break;
        }
        case AST_EXPR_INTEGER: return &kInt_tr;
        case AST_EXPR_FLOAT: return &kFloat_tr;
        case AST_EXPR_BOOL: return &kBool_tr;
        case AST_EXPR_STRING: return &kString_tr;
        /* A binary op's result type is its (sema-)resolved type — String for
         * `+` concat, Int/Float for arithmetic, Bool for comparisons. Surfacing
         * it is what lets an owned-String destination wrap the concat result in
         * rae_string_pool_take, exactly as the `.concat()` method-call path
         * does. Without it a `String + String` passed to an `own String`
         * parameter (e.g. `list.add(a + b)`) stayed pool-registered and was
         * freed by the statement's pool flush AND by the container drop — a
         * double-free / malloc abort. */
        case AST_EXPR_BINARY: {
            const TypeInfo* rt = expr->resolved_type;
            if (rt && rt->kind == TYPE_REF) rt = rt->as.ref.base;
            if (!rt) break;
            if (rt->kind == TYPE_STRING) return &kString_tr;
            if (rt->kind == TYPE_INT) return &kInt_tr;
            if (rt->kind == TYPE_FLOAT) return &kFloat_tr;
            if (rt->kind == TYPE_BOOL) return &kBool_tr;
            break;
        }
        /* Interpolation ALWAYS produces a freshly-allocated String, so its
         * type is never in doubt. Leaving this unhandled (returning NULL)
         * silently disabled every ownership decision that asks "is this
         * argument a String?" — most visibly the pool_take wrap in
         * c_call.c, whose own guard lists AST_EXPR_INTERP but could never
         * fire, so `list.add(value: "{x}")` stored a pooled pointer that
         * the statement flush then freed (#343: garbage element + a
         * double-free abort at drop). */
        case AST_EXPR_INTERP: return &kString_tr;
        case AST_EXPR_IDENT: {
            const AstTypeRef* lt = get_local_type_ref(ctx, expr->as.ident);
            if (lt) return lt;
            // Module-level global (`var`/`let` at module scope, AST_DECL_GLOBAL_LET):
            // resolve its declared type so method calls / member access on a
            // global receiver (e.g. `g_list.get(i)`, `g_list.length`) dispatch
            // with the right receiver type instead of a bare, type-less mangling.
            if (ctx->compiler_ctx) {
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind == AST_DECL_GLOBAL_LET && str_eq(d->as.let_decl.name, expr->as.ident))
                        return d->as.let_decl.type;
                }
            }
            return NULL;
        }
        case AST_EXPR_MEMBER: {
            const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.member.object); Str obj_name = get_base_type_name(obj_tr);
            if (obj_name.len == 0) obj_name = infer_expr_type(ctx, expr->as.member.object);
            const AstDecl* d = find_type_decl(ctx, ctx->module, obj_name);
            if (d && d->kind == AST_DECL_TYPE) {
                for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                    if (str_eq(f->name, expr->as.member.member)) return substitute_type_ref(ctx->compiler_ctx, d->as.type_decl.generic_params, (obj_tr && obj_tr->generic_args) ? obj_tr->generic_args : ctx->generic_args, f->type);
                }
            }
            break;
        }
        case AST_EXPR_CALL: {
            if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) {
                const AstTypeRef* crt = expr->decl_link->as.func_decl.returns
                    ? expr->decl_link->as.func_decl.returns->type : NULL;
                // A generic `ret opt T` template return carries the payload as a
                // type param; if sema pinned the concrete result, prefer it so
                // the `opt` survives with a real payload TypeInfo (#651).
                if (crt && crt->is_opt && expr->resolved_type
                    && expr->resolved_type->kind == TYPE_OPT)
                    return infer_tr_from_resolved(ctx, expr->resolved_type);
                if (crt) return crt;
            }
            // Unresolved generic free-function call (no decl_link): fall back to
            // the sema-resolved type so opt-returning calls keep their `opt`.
            if (expr->resolved_type)
                return infer_tr_from_resolved(ctx, expr->resolved_type);
            // Still nothing (a generic free function called inside another
            // template, e.g. `get(this, k) is not none` in `has`): recover the
            // callee by name + first-argument base type, exactly as the
            // METHOD_CALL fallback does, and hand back its (possibly `opt`)
            // return type so none-checks/`if let` see the optional (#651).
            if (expr->as.call.callee && expr->as.call.callee->kind == AST_EXPR_IDENT) {
                Str fname = expr->as.call.callee->as.ident;
                const AstTypeRef* a0 = expr->as.call.args
                    ? infer_expr_type_ref(ctx, expr->as.call.args->value) : NULL;
                Str a0base = get_base_type_name(a0);
                const AstFuncDecl* best = NULL;
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind != AST_DECL_FUNC) continue;
                    const AstFuncDecl* cfd = &d->as.func_decl;
                    if (!str_eq(cfd->name, fname) || !cfd->returns) continue;
                    // Require the first value parameter's base type to match the
                    // first argument's — a bare name match would pick the wrong
                    // overload (e.g. a String-returning `get` for an Int call),
                    // mis-typing the result and corrupting downstream ownership
                    // decisions (string-pool-take on an Int, #651).
                    if (a0base.len > 0 && cfd->params
                        && str_eq(get_base_type_name(cfd->params->type), a0base)) {
                        best = cfd; break;
                    }
                }
                if (best && best->returns) {
                    const AstTypeRef* rt2 = best->returns->type;
                    // If the return is a generic type param (or `opt <param>`),
                    // substitute it from the arguments so the result is the
                    // CONCRETE `opt Int` / `opt String`, not the unresolvable
                    // template `opt T` (which mangles to `rae_opt_rae_T`).
                    Str rname = get_base_type_name(rt2);
                    const AstIdentifierPart* gps = best->generic_params;
                    if (!gps && best->generic_template
                        && best->generic_template->kind == AST_DECL_FUNC)
                        gps = best->generic_template->as.func_decl.generic_params;
                    bool is_tp = false;
                    for (const AstIdentifierPart* gp = gps; gp; gp = gp->next)
                        if (str_eq(gp->text, rname)) { is_tp = true; break; }
                    if (is_tp) {
                        const AstParam* p = best->params;
                        const AstCallArg* a = expr->as.call.args;
                        const AstTypeRef* concrete = NULL;
                        for (; p && a; p = p->next, a = a->next) {
                            if (!p->type) continue;
                            if (str_eq(get_base_type_name(p->type), rname)) {
                                concrete = infer_expr_type_ref(ctx, a->value); break;
                            }
                            size_t slot = 0; bool found = false;
                            for (const AstTypeRef* pa = p->type->generic_args; pa; pa = pa->next, slot++)
                                if (str_eq(get_base_type_name(pa), rname)) { found = true; break; }
                            if (found) {
                                const AstTypeRef* at = infer_expr_type_ref(ctx, a->value);
                                const AstTypeRef* ga = at ? at->generic_args : NULL;
                                for (size_t k = 0; k < slot && ga; k++) ga = ga->next;
                                if (ga) { concrete = ga; break; }
                            }
                        }
                        if (concrete) {
                            if (rt2->is_opt || rt2->is_view || rt2->is_mod) {
                                AstTypeRef* w = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
                                *w = *concrete; w->next = NULL;
                                w->is_opt = rt2->is_opt;
                                w->is_view = rt2->is_view;
                                w->is_mod = rt2->is_mod;
                                return w;
                            }
                            return concrete;
                        }
                    }
                    return rt2;
                }
            }
            break;
        }
        case AST_EXPR_METHOD_CALL: {
            const AstDecl* mdecl = (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC)
                                 ? expr->decl_link : NULL;
            /* Sema leaves a GENERIC method call unresolved (no decl_link, no
             * type) because its receiver-type matching compares a mangled
             * instantiation name against the written template name. Recover the
             * callee here by name + receiver base type, exactly the way the
             * lowering below picks it. Without this the receiver of a CHAINED
             * call types as nothing and the next method binds a same-named
             * overload for a different type. */
            if (!mdecl) {
                const AstTypeRef* rtr = infer_expr_type_ref(ctx, expr->as.method_call.object);
                Str rbase = get_base_type_name(rtr);
                if (rbase.len > 0) {
                    for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                        const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                        if (d->kind != AST_DECL_FUNC) continue;
                        const AstFuncDecl* cfd = &d->as.func_decl;
                        if (!str_eq(cfd->name, expr->as.method_call.method_name)) continue;
                        if (cfd->specialization_args) continue;
                        if (!cfd->params || !str_eq_cstr(cfd->params->name, "this")) continue;
                        if (!str_eq(get_base_type_name(cfd->params->type), rbase)) continue;
                        mdecl = d; break;
                    }
                }
            }
            if (!mdecl) break;
            const AstFuncDecl* mfd = &mdecl->as.func_decl;
            if (!mfd->returns) return NULL;
            const AstTypeRef* rt = mfd->returns->type;
            /* A generic method declared `ret T` returns its OWN type parameter.
             * Handing that back verbatim makes a CHAINED call resolve against
             * the name "T": `parts.at(i).toInt()` then binds whatever overload
             * of `toInt` comes first (the Int one), and the mismatch only
             * surfaces as a C type error — sema believed it resolved fine.
             *
             * Substitute from the receiver: find where the type parameter sits
             * in the `this` parameter's written type (`view List(T)` -> slot 0)
             * and take the receiver's generic argument in the same slot
             * (`List(String)` -> String). Same positional matching
             * infer_generic_args does, so a method whose type params appear in
             * a different order than the struct's still maps correctly.
             *
             * Deliberately confined to the BACKEND's type inference: earlier
             * attempts to fix this in sema's overload selection regressed eight
             * generic-container tests, because anything that changes which
             * candidate is chosen perturbs ECS/StringMap resolution. This only
             * answers "what type does this expression have", which was simply
             * wrong before. A specialization whose return is already concrete
             * falls through untouched. */
            const AstIdentifierPart* gps = mfd->generic_params;
            if (!gps && mfd->generic_template && mfd->generic_template->kind == AST_DECL_FUNC) {
                gps = mfd->generic_template->as.func_decl.generic_params;
            }
            if (gps && mfd->params) {
                Str rname = get_base_type_name(rt);
                bool is_type_param = false;
                for (const AstIdentifierPart* gp = gps; gp; gp = gp->next) {
                    if (str_eq(gp->text, rname)) { is_type_param = true; break; }
                }
                if (is_type_param) {
                    /* Locate the slot of `rname` in the receiver parameter's
                     * written generic args. */
                    size_t slot = 0; bool found_slot = false;
                    for (const AstTypeRef* pa = mfd->params->type ? mfd->params->type->generic_args : NULL;
                         pa; pa = pa->next, slot++) {
                        if (str_eq(get_base_type_name(pa), rname)) { found_slot = true; break; }
                    }
                    if (found_slot) {
                        // Prefer sema's concrete result: it keeps the `opt`
                        // (and a real payload TypeInfo) that the bare type-param
                        // substitution below would drop (#651).
                        if ((rt->is_opt || rt->is_view || rt->is_mod)
                            && expr->resolved_type)
                            return infer_tr_from_resolved(ctx, expr->resolved_type);
                        const AstTypeRef* recv = infer_expr_type_ref(ctx, expr->as.method_call.object);
                        if (recv) {
                            const AstTypeRef* ga = recv->generic_args;
                            for (size_t k = 0; k < slot && ga; k++) ga = ga->next;
                            if (ga) {
                                // Re-apply the return's qualifier: `ret opt T`
                                // over T=Int is `opt Int`, not bare Int.
                                if (rt->is_opt || rt->is_view || rt->is_mod) {
                                    AstTypeRef* w = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
                                    *w = *ga; w->next = NULL;
                                    w->is_opt = rt->is_opt;
                                    w->is_view = rt->is_view;
                                    w->is_mod = rt->is_mod;
                                    return w;
                                }
                                return ga;
                            }
                        }
                    }
                }
            }
            return rt;
        }
        default: break;
    }
    return NULL;
}

Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return (Str){0};
    const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
    if (tr) return str_from_cstr(rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr));
    return (Str){0};
}

// A module-level global whose initializer is a function/method call can't be a
// C static initializer (static init must be constant). Such globals are emitted
// as bare (zero-init) declarations and assigned at the top of main() instead.
// Literal / object / arithmetic initializers stay as valid static initializers.
static bool global_init_is_deferred(const AstExpr* v) {
    return v && (v->kind == AST_EXPR_CALL || v->kind == AST_EXPR_METHOD_CALL);
}


bool emit_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, FILE* out, const struct VmRegistry* r, bool ray) {
  if (f->is_extern || str_starts_with_cstr(f->name, "rae_ext_")) return true;
  CFuncContext tctx = {.compiler_ctx = ctx, .module = m, .func_decl = f, .uses_raylib = ray, .registry = r, .func_first_let_idx = (size_t)-1};
  const char* rt = c_return_type(&tctx, f); const char* mangled = rae_mangle_function(ctx, f);
  
  bool is_main = str_eq_cstr(f->name, "main");
  if (is_main) {
      fprintf(out, "int main(int argc, char** argv) {\n  (void)argc; (void)argv;\n");
  } else {
      fprintf(out, "RAE_UNUSED static %s %s(", rt, mangled); emit_param_list(&tctx, f->params, out, false); fprintf(out, ") {\n");
  }

  for (const AstParam* p = f->params; p; p = p->next) {
      if (tctx.local_count < 256) {
          tctx.locals[tctx.local_count] = p->name;
          tctx.local_type_refs[tctx.local_count] = p->type;
          const char* tn = rae_mangle_type_specialized(ctx, NULL, NULL, p->type);
          tctx.local_types[tctx.local_count] = str_from_cstr(tn);
          // Stage C: `own T` parameter — callee receives ownership and
          // is responsible for end-of-scope cascade drop. Mark the
          // slot as uniquely owning so emit_implicit_drops_for_params
          // picks the full (non-alias) drop variant.
          //
          // Stage 3 (`copy T`): callee gets a fresh deep copy paid for
          // at the call site. It owns the heap and must drop at scope
          // end, same as `own T`.
          if (p->type && (p->type->is_own || p->type->is_copy)) {
              tctx.local_struct_owns_heap[tctx.local_count] = true;
          }
          tctx.local_count++;
      }
  }
  // Param auto-drop: tried three flavours, all reverted.
  // - Full auto-drop (any cascade-heap T param): test 413 crashes
  //   when caller passes struct.field (no local to mark moved).
  // - String-only auto-drop: test 430 case2 expects callerSrc to
  //   stay readable after passing to a function. Move tracking is
  //   compile-time only, so the runtime heap is freed under the
  //   caller's feet.
  // - String-only auto-drop + caller-side rae_string_copy: closes
  //   434 / 435 leak class AND keeps 430 case2's value semantics —
  //   but parseScene→parseJson chain has hidden alias somewhere
  //   that still crashes test 413 during scene drop. Diagnostic
  //   work continues (see project-mobile-ui-leak memory note).
  size_t first_let_idx = tctx.local_count;
  // Stage 7: stash on the context so the ret-stmt epilogue can drop
  // the same range of locals before each return (not just fallthrough).
  tctx.func_first_let_idx = first_let_idx;

  // Stage 4: per-function string-temp-pool guard. Catches any
  // pool registrations from `rae_ext_rae_str_interp` that escape
  // their containing statement (e.g. `let n: Int = "{i}".length()`
  // where the interp result lives long enough to be read but isn't
  // captured by any String binding). The expression-statement and
  // String-let wrappers handle the common cases inline; this is
  // the safety net so the global pool doesn't grow unbounded
  // across long-running call chains.
  fprintf(out, "  int __rae_spm_func = rae_string_pool_mark();\n");

  // Assign module-level globals whose initializers are function/method calls
  // (not valid as C static initializers) — run once here, before main's body,
  // in declaration order. See global_init_is_deferred + the globals emitter.
  if (is_main) {
      for (size_t gi = 0; gi < ctx->all_decl_count; gi++) {
          const AstDecl* gd = ctx->all_decls[gi];
          if (gd->kind != AST_DECL_GLOBAL_LET || !global_init_is_deferred(gd->as.let_decl.value)) continue;
          bool sh = tctx.has_expected_type; AstTypeRef se = tctx.expected_type;
          if (gd->as.let_decl.type) { tctx.expected_type = *gd->as.let_decl.type; tctx.has_expected_type = true; }
          fprintf(out, "  %.*s = ", (int)gd->as.let_decl.name.len, gd->as.let_decl.name.data);
          emit_expr(&tctx, gd->as.let_decl.value, out, PREC_LOWEST, false, false);
          fprintf(out, ";\n");
          tctx.has_expected_type = sh; tctx.expected_type = se;
      }
  }

  if (f->body) { for (AstStmt* s = f->body->first; s; s = s->next) emit_stmt(&tctx, s, out); }

  // Emit any remaining defers at function end
  if (tctx.defer_stack.count > 0) emit_defers(&tctx, 0, out);

  // Stage 2 + 3 (docs/scope-exit-dealloc.md, docs/ownership-model.md):
  // drop heap-owning lets at end-of-body fallthrough, then `own T`
  // parameters that haven't been moved onward.
  emit_implicit_drops_for_body(&tctx, out, first_let_idx);
  emit_implicit_drops_for_own_params(&tctx, out, first_let_idx);

  fprintf(out, "  rae_string_pool_flush(__rae_spm_func);\n");

  if (is_main) fprintf(out, "  return 0;\n}\n\n");
  else fprintf(out, "}\n\n");
  return true;
}

// Track emitted specialized functions to avoid redefinitions
const char* g_emitted_spec_funcs[4096];
static size_t g_emitted_spec_func_count = 0;

bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray) {
  // Specialized externs (sizeof(T)(), rae_ext_rae_buf_get(V), ...) have no
  // body and their call sites are inlined elsewhere — emitting an empty
  // function body produces -Wreturn-type warnings.
  if (f->is_extern) return true;
  const AstIdentifierPart* gp_src = f->generic_params; if (!gp_src && f->generic_template) gp_src = f->generic_template->as.func_decl.generic_params;
  CFuncContext tctx = {.compiler_ctx = ctx, .module = m, .func_decl = f, .uses_raylib = ray, .registry = r, .generic_params = gp_src, .generic_args = args, .func_first_let_idx = (size_t)-1};
  const char* rt = c_return_type(&tctx, f); const char* mangled = rae_mangle_specialized_function(ctx, f, args);
  // Dedup check: skip if already emitted
  for (size_t i = 0; i < g_emitted_spec_func_count; i++) {
      if (strcmp(g_emitted_spec_funcs[i], mangled) == 0) return true;
  }
  if (g_emitted_spec_func_count < 4096) g_emitted_spec_funcs[g_emitted_spec_func_count++] = mangled;
  fprintf(out, "RAE_UNUSED static %s %s(", rt, mangled); emit_param_list(&tctx, f->params, out, false); fprintf(out, ") {\n");
  for (const AstParam* p = f->params; p; p = p->next) {
      if (tctx.local_count < 256) {
          tctx.locals[tctx.local_count] = p->name;
          tctx.local_type_refs[tctx.local_count] = p->type;
          tctx.local_types[tctx.local_count] = str_from_cstr(rae_mangle_type_specialized(ctx, gp_src, args, p->type));
          // Stage C: `own T` param — callee owns; mark for end-of-scope drop.
          // Stage 3: `copy T` param — callee owns the deep-copied
          // value the caller paid for; same drop responsibility.
          if (p->type && (p->type->is_own || p->type->is_copy)) {
              tctx.local_struct_owns_heap[tctx.local_count] = true;
          }
          tctx.local_count++;
      }
  }

  // Layer 5 element-drop synthesis: when the function is the stdlib
  // `drop(T)(this: mod List(T))` (or StringMap / IntMap), inject a
  // per-element drop loop BEFORE the template body's `buf_free`.
  // The stdlib body only frees the backing buffer, so without this
  // any heap a per-element T owns (a List, a StringMap, a nested
  // struct that owns those) leaks. See test 425_list_element_drop.
  //
  // Conditions:
  //   - function is named "drop"
  //   - takes exactly one param `this`
  //   - param type base is List / StringMap / IntMap
  //   - the substituted element type transitively owns heap
  if (f->body && str_eq_cstr(f->name, "drop") && f->params && !f->params->next
      && f->params->type) {
    Str pbase = get_base_type_name(f->params->type);
    bool is_list = str_eq_cstr(pbase, "List");
    bool is_smap = str_eq_cstr(pbase, "StringMap");
    bool is_imap = str_eq_cstr(pbase, "IntMap");
    if ((is_list || is_smap || is_imap) && args) {
      // Element type T = args (single concrete type arg).
      //
      // Predicate split: we use the strict predicate (no String) as
      // the GATE — that's the set of elements that have a working
      // drop chain (rae_drop_struct_<T> exists, or T is a List/Map).
      // The one extra case Stage 3 enables is List(String) /
      // Map(String): String element-drop calls rae_ext_rae_str_free
      // directly (no drop_struct needed). Structs whose only heap is
      // a String stay un-iterated for now — same leak status as
      // before Stage 3, no crash.
      AstTypeRef* elem = (AstTypeRef*)args;
      Str ebase = get_base_type_name(elem);
      bool elem_is_opt = elem && elem->is_opt;
      bool elem_is_string = !elem_is_opt && str_eq_cstr(ebase, "String");
      // Phase 3 follow-up: PERMISSIVE element iteration so List<T>
      // with String-only-struct T (Name, NodeId, JsonField, …) drops
      // each element's Strings. Paired with:
      //   - Phase 2 deep-copying String fields at struct literal init
      //   - struct `_alias` drop variant skipping List/Map fields
      //     when element T needs cascade drop
      //   - lib/json.rae parseObject manual `localFields.length = 0`
      //     after bulk-transfer to suppress double-iteration
      bool elem_needs_drop = elem_is_string ||
          type_needs_cascade_drop(ctx, m, elem, 0);
      // StringMap always needs an entry iteration because its keys
      // are Strings and must be freed regardless of whether the value
      // type is heap-owning (e.g. StringMap(Int) — keys like "AlbumRoot"
      // are heap-owned by the map after a JSON parse).
      bool needs_loop = elem_needs_drop || is_smap;
      if (needs_loop) {
        bool elem_opt_struct = elem_is_opt && rae_opt_is_struct_rep(&tctx, elem);
        const char* elem_mangled = (elem_is_opt && !elem_opt_struct)
            ? "RaeAny"
            : rae_mangle_type_specialized(ctx, NULL, NULL, elem);
        bool elem_is_container = !elem_is_opt && (str_eq_cstr(ebase, "List") || str_eq_cstr(ebase, "StringMap") || str_eq_cstr(ebase, "IntMap"));
        // Find the per-T drop overload for nested containers.
        const AstFuncDecl* nested_drop = NULL;
        if (elem_is_container) {
          for (size_t i = 0; i < ctx->all_decl_count; i++) {
            const AstDecl* d = ctx->all_decls[i];
            if (d->kind != AST_DECL_FUNC) continue;
            if (!str_eq_cstr(d->as.func_decl.name, "drop")) continue;
            if (!d->as.func_decl.generic_params) continue;
            const AstParam* fp = d->as.func_decl.params;
            if (!fp || !fp->type) continue;
            Str fpb = get_base_type_name(fp->type);
            if (str_eq(fpb, ebase)) { nested_drop = &d->as.func_decl; break; }
          }
        }
        if (is_list) {
          fprintf(out, "  for (int64_t __i = 0; __i < this->length; __i++) {\n");
          fprintf(out, "    %s* __elem = (%s*)((char*)this->data + __i * sizeof(%s));\n",
                  elem_mangled, elem_mangled, elem_mangled);
          if (elem_is_opt) {
            if (elem_opt_struct) fprintf(out, "    rae_drop_%s(__elem);\n", elem_mangled);
            else fprintf(out, "    rae_any_drop(__elem);\n");
          } else if (elem_is_string) {
            // List(String) — call the string-free helper. is_owned
            // check inside makes borrowed entries safe.
            fprintf(out, "    rae_ext_rae_str_free(*__elem);\n");
          } else if (elem_is_container && nested_drop) {
            const AstTypeRef* inner = elem->generic_args;
            if (inner) {
              register_function_specialization(ctx, nested_drop, inner);
              const char* nested_fn = rae_mangle_specialized_function(ctx, nested_drop, inner);
              fprintf(out, "    %s(__elem);\n", nested_fn);
            }
          } else if (!elem_is_container) {
            // Heap-owning user struct (e.g. SceneNode { childrenIds: List(String) }).
            fprintf(out, "    rae_drop_struct_%s(__elem);\n", elem_mangled);
          }
          fprintf(out, "  }\n");
        } else if (is_smap || is_imap) {
          // StringMap / IntMap entries are stored in a sparse buffer
          // keyed by `occupied`. Only drop where occupied is true.
          // Entry struct: { k: <Key>, value: V, occupied: Bool }
          // The dense data is `Buffer(StringMapEntry(V))`. Iterate up
          // to capacity, skip unoccupied. For now only drop entry.value
          // (key Strings are skipped for the same reason single-let
          // String locals are skipped — see test 425 follow-up).
          const char* entry_struct = (is_smap) ? "rae_StringMapEntry" : "rae_IntMapEntry";
          fprintf(out, "  {\n");
          fprintf(out, "    char* __buf = (char*)this->data;\n");
          fprintf(out, "    size_t __stride = sizeof(%s_%s);\n", entry_struct, elem_mangled);
          fprintf(out, "    for (int64_t __i = 0; __i < this->cap; __i++) {\n");
          fprintf(out, "      %s_%s* __entry = (%s_%s*)(__buf + __i * __stride);\n",
                  entry_struct, elem_mangled, entry_struct, elem_mangled);
          fprintf(out, "      if (!__entry->occupied) continue;\n");
          if (is_smap) {
            // StringMap key is always a String — free it. The
            // is_owned check in rae_ext_rae_str_free makes literal-
            // backed keys a safe no-op.
            fprintf(out, "      rae_ext_rae_str_free(__entry->k);\n");
          }
          if (elem_needs_drop) {
            if (elem_is_opt) {
              if (elem_opt_struct) fprintf(out, "      rae_drop_%s(&__entry->value);\n", elem_mangled);
              else fprintf(out, "      rae_any_drop(&__entry->value);\n");
            } else if (elem_is_string) {
              fprintf(out, "      rae_ext_rae_str_free(__entry->value);\n");
            } else if (elem_is_container && nested_drop) {
              const AstTypeRef* inner = elem->generic_args;
              if (inner) {
                register_function_specialization(ctx, nested_drop, inner);
                const char* nested_fn = rae_mangle_specialized_function(ctx, nested_drop, inner);
                fprintf(out, "      %s(&__entry->value);\n", nested_fn);
              }
            } else if (!elem_is_container) {
              fprintf(out, "      rae_drop_struct_%s(&__entry->value);\n", elem_mangled);
            }
          }
          fprintf(out, "    }\n");
          fprintf(out, "  }\n");
        }
      }
    }
  }

  // Stage 2 + 3: see emit_function above.
  size_t first_let_idx = tctx.local_count;
  tctx.func_first_let_idx = first_let_idx;
  // Stage 4: per-function string-temp-pool guard. See emit_function.
  fprintf(out, "  int __rae_spm_func = rae_string_pool_mark();\n");
  // #773: compile-time field reflection through a generic world parameter. If
  // this specialization's body contains a `loop ... in fields(world)`, the field
  // set depends on the concrete generic args, so expand it AFTER substitution —
  // once per instantiation — into ordinary alias-bindings the emitter already
  // handles. reflect_instantiate_body returns NULL for every other function, so
  // this is inert for all non-reflection generics.
  const AstBlock* body_to_emit = f->body;
  {
      AstBlock* instantiated = reflect_instantiate_body(ctx, m, f->body, f->params, gp_src, args);
      if (instantiated) body_to_emit = instantiated;
  }
  if (body_to_emit) { for (AstStmt* s = body_to_emit->first; s; s = s->next) emit_stmt(&tctx, s, out); }
  emit_implicit_drops_for_body(&tctx, out, first_let_idx);
  emit_implicit_drops_for_own_params(&tctx, out, first_let_idx);
  fprintf(out, "  rae_string_pool_flush(__rae_spm_func);\n");
  fprintf(out, "}\n\n"); return true;
}

// True if an earlier decl in all_decls already defines a non-generic type with
// this name. Two same-named type decls (e.g. a prelude type that a user file
// also defines) mangle to one C symbol, so their auto-derived toJson/fromJson/
// to_str would emit twice -> C "redefinition". Emitting the derived methods
// only for the first occurrence keeps the generated C valid instead of a hard
// compile failure with a .c line. (#136)
static bool earlier_same_named_type(CompilerContext* ctx, size_t idx, Str name) {
    for (size_t j = 0; j < idx; j++) {
        const AstDecl* e = ctx->all_decls[j];
        if (e->kind == AST_DECL_TYPE && !e->as.type_decl.generic_params
            && str_eq(e->as.type_decl.name, name)) return true;
    }
    return false;
}

/* Value equality for `is` on a user struct (#703). C forbids `struct == struct`,
 * so `a is b` on a value struct is lowered to a synthesized field-wise
 * `rae_eq_<T>`. That is only sound when every field is itself value-comparable:
 * a scalar, an enum (lowered to int), a String (content equality), or a nested
 * value-comparable struct. A struct with a List/Map/opt/reference field is NOT
 * value-comparable — comparing those by value is ambiguous — so `is` stays
 * unsupported there (compare the specific fields instead). This is why
 * `EntityId is EntityId` works with no helper function. */
bool rae_struct_value_comparable(const AstModule* module, const AstTypeDecl* td);
static bool rae_typeref_value_comparable(const AstModule* module, const AstTypeRef* ft) {
    if (!ft || ft->is_opt || ft->is_view || ft->is_mod) return false;
    Str base = get_base_type_name(ft);
    if (is_primitive_type(base)) return true;
    if (str_eq_cstr(base, "String")) return true;
    if (str_eq_cstr(base, "Bool") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) return true;
    if (find_enum_decl(NULL, module, base)) return true;
    const AstDecl* d = find_type_decl(NULL, module, base);
    if (d && d->kind == AST_DECL_TYPE && !d->as.type_decl.generic_params
        && !has_property(d->as.type_decl.properties, "c_struct"))
        return rae_struct_value_comparable(module, &d->as.type_decl);
    return false;
}
bool rae_struct_value_comparable(const AstModule* module, const AstTypeDecl* td) {
    if (!td) return false;
    for (const AstTypeField* f = td->fields; f; f = f->next)
        if (!rae_typeref_value_comparable(module, f->type)) return false;
    return true;
}
// Convenience for the expression emitter: is the named type a synthesizable
// value-comparable struct (so `a is b` can call rae_eq_<name>)?
bool rae_named_type_value_comparable(const AstModule* module, Str base) {
    if (is_primitive_type(base) || str_eq_cstr(base, "String")) return false;
    const AstDecl* d = find_type_decl(NULL, module, base);
    if (!d || d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params
        || has_property(d->as.type_decl.properties, "c_struct")) return false;
    return rae_struct_value_comparable(module, &d->as.type_decl);
}

bool c_backend_emit_module(CompilerContext* ctx, const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {
  (void)out_uses_raylib;
  if (!module) return false;
  g_emitted_spec_func_count = 0; // Reset dedup for this compilation
  ctx->all_decl_count = 0; collect_decls_from_module(ctx, module); ctx->current_module = (AstModule*)module;

  // Discover generic specializations by walking all function bodies
  collect_type_refs_module(ctx);

  FILE* out = fopen(out_path, "w"); if (!out) return false;
  fprintf(out, "#include \"rae_runtime.h\"\n");
  // C headers declared by binding modules (`cheader "..."`, general FFI #497),
  // so their c_struct types and extern("symbol") functions resolve against the
  // real library declarations. Walk the module + its imports, deduping both the
  // modules visited and the header paths emitted.
  {
    const AstModule* seen_mods[256]; size_t seen_mod_n = 0;
    Str seen_hdrs[256]; size_t seen_hdr_n = 0;
    const AstModule* stack[256]; size_t sp = 0;
    stack[sp++] = module;
    while (sp > 0) {
      const AstModule* m = stack[--sp];
      if (!m) continue;
      bool visited = false;
      for (size_t i = 0; i < seen_mod_n; i++) if (seen_mods[i] == m) { visited = true; break; }
      if (visited) continue;
      if (seen_mod_n < 256) seen_mods[seen_mod_n++] = m;
      for (const AstCHeader* h = m->c_headers; h; h = h->next) {
        bool dup = false;
        for (size_t i = 0; i < seen_hdr_n; i++)
          if (str_eq(seen_hdrs[i], h->path)) { dup = true; break; }
        if (dup) continue;
        if (seen_hdr_n < 256) seen_hdrs[seen_hdr_n++] = h->path;
        fprintf(out, "#include \"%.*s\"\n", (int)h->path.len, h->path.data);
      }
      for (const AstImport* imp = m->imports; imp; imp = imp->next)
        if (imp->module && sp < 256) stack[sp++] = imp->module;
    }
  }
  fprintf(out, "\n");
  EmittedTypeList emitted = { .items = malloc(sizeof(char*) * 1024), .capacity = 1024, .count = 0 };
  EmittedTypeList visiting = { .items = malloc(sizeof(char*) * 1024), .capacity = 1024, .count = 0 };
  for (size_t i = 0; i < ctx->generic_type_count; i++) emit_type_recursive(ctx, module, ctx->generic_types[i], out, &emitted, &visiting, false);

  // Emit enum definitions as #define constants
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_ENUM) {
          int64_t idx = 0;
          for (const AstEnumMember* m = d->as.enum_decl.members; m; m = m->next) {
              fprintf(out, "#define %.*s_%.*s ((int64_t)%lldLL)\n",
                  (int)d->as.enum_decl.name.len, d->as.enum_decl.name.data,
                  (int)m->name.len, m->name.data, (long long)idx++);
          }
          // Auto enum -> member-name string, so `value.toString()` and string
          // interpolation yield the member NAME (ClipKind.walk -> "walk") rather
          // than the ordinal. One per enum; RAE_UNUSED silences unused ones.
          fprintf(out, "RAE_UNUSED static rae_String rae_enum_toString_%.*s(int64_t v) {\n",
              (int)d->as.enum_decl.name.len, d->as.enum_decl.name.data);
          fprintf(out, "  switch (v) {\n");
          idx = 0;
          for (const AstEnumMember* m = d->as.enum_decl.members; m; m = m->next) {
              fprintf(out, "  case %lldLL: return (rae_String){(uint8_t*)\"%.*s\", %d};\n",
                  (long long)idx++, (int)m->name.len, m->name.data, (int)m->name.len);
          }
          fprintf(out, "  }\n  return (rae_String){(uint8_t*)\"\", 0};\n}\n");
          // Reverse map (member NAME -> ordinal) so a synthesized fromJson can
          // round-trip an enum field the toJson wrote by name (#767). Unknown
          // names fall back to 0 (the first member).
          fprintf(out, "RAE_UNUSED static int64_t rae_enum_fromString_%.*s(rae_String s) {\n",
              (int)d->as.enum_decl.name.len, d->as.enum_decl.name.data);
          idx = 0;
          for (const AstEnumMember* m = d->as.enum_decl.members; m; m = m->next) {
              fprintf(out, "  if (s.len == %d && memcmp(s.data, \"%.*s\", %d) == 0) return %lldLL;\n",
                  (int)m->name.len, (int)m->name.len, m->name.data, (int)m->name.len, (long long)idx++);
          }
          fprintf(out, "  return 0;\n}\n");
          fprintf(out, "\n");
      }
  }

  // Emit non-generic user-defined struct types
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_TYPE && !d->as.type_decl.generic_params) {
          AstTypeRef tr = {0};
          AstIdentifierPart part = {0};
          part.text = d->as.type_decl.name;
          tr.parts = &part;
          emit_type_recursive(ctx, module, &tr, out, &emitted, &visiting, false);
      }
  }

  // Value-optional helper collection (opt T over an aggregate payload).
  // Each distinct struct-rep opt type gets `rae_drop_<optT>` and
  // `rae_deep_copy_<optT>` when its payload transitively owns heap. Forward
  // declarations go here (all payload structs are now declared); bodies are
  // emitted at the end of the deep-copy section, once the payload drop/copy
  // helpers are also declared.
  typedef struct {
    const AstTypeRef* type;      // the opt AstTypeRef
    AstTypeRef payload;          // payload with is_opt cleared
    const char* optm;            // rae_opt_<payload> mangled name
    bool needs_drop;
    bool needs_copy;
  } OptHelperEntry;
  OptHelperEntry opt_entries[512];
  size_t opt_entry_count = 0;
  // Add one struct-rep opt type (the AstTypeRef `_gt`) to opt_entries.
  #define TRY_ADD_OPT(_gt) do { \
    const AstTypeRef* gt = (_gt); \
    if (gt && gt->is_opt && !gt->is_view && !gt->is_mod && opt_entry_count < 512) { \
      CFuncContext octx = {0}; octx.compiler_ctx = ctx; octx.module = module; \
      if (rae_opt_is_struct_rep(&octx, gt)) { \
        const char* optm = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)gt); \
        bool dup = !optm; \
        for (size_t k = 0; !dup && k < opt_entry_count; k++) \
          if (strcmp(opt_entries[k].optm, optm) == 0) dup = true; \
        if (!dup) { \
          emit_type_recursive(ctx, module, gt, out, &emitted, &visiting, false); \
          AstTypeRef payload = *gt; \
          payload.is_opt = false; payload.next = NULL; \
          payload.resolved_type = (gt->resolved_type && gt->resolved_type->kind == TYPE_OPT) \
              ? gt->resolved_type->as.opt.base \
              : (payload.parts ? NULL : gt->resolved_type); \
          opt_entries[opt_entry_count].type = gt; \
          opt_entries[opt_entry_count].payload = payload; \
          opt_entries[opt_entry_count].optm = optm; \
          opt_entries[opt_entry_count].needs_drop = type_needs_cascade_drop(ctx, module, &payload, 0); \
          opt_entries[opt_entry_count].needs_copy = type_needs_deep_copy(ctx, module, &payload, 0); \
          opt_entry_count++; \
        } \
      } \
    } \
  } while (0)
  for (size_t i = 0; i < ctx->generic_type_count; i++) TRY_ADD_OPT(ctx->generic_types[i]);
  // Struct FIELDS: a struct-rep opt field (e.g. `Player { nowPlaying: opt Track }`)
  // reaches the field-drop/copy sites which call rae_drop_/rae_deep_copy_<optT>,
  // but the field type isn't necessarily a standalone generic_types entry.
  // Scan non-generic struct fields, and generic-instance struct fields (with
  // their concrete args substituted).
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
    if (has_property(d->as.type_decl.properties, "c_struct")) continue;
    for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next)
      if (f->type && f->type->is_opt) TRY_ADD_OPT(f->type);
  }
  for (size_t i = 0; i < ctx->generic_type_count; i++) {
    const AstTypeRef* gt = ctx->generic_types[i];
    if (!gt || gt->is_opt || gt->is_view || gt->is_mod || !gt->generic_args) continue;
    Str gb = get_base_type_name(gt);
    const AstDecl* td = NULL;
    for (size_t k = 0; k < ctx->all_decl_count; k++) {
      const AstDecl* dd = ctx->all_decls[k];
      if (dd->kind == AST_DECL_TYPE && !dd->as.type_decl.specialization_args
          && str_eq(dd->as.type_decl.name, gb)) { td = dd; break; }
    }
    if (!td || !td->as.type_decl.generic_params) continue;
    for (const AstTypeField* f = td->as.type_decl.fields; f; f = f->next) {
      if (!f->type) continue;
      AstTypeRef* sub = substitute_type_ref(ctx, td->as.type_decl.generic_params,
                                            gt->generic_args, f->type);
      if (sub && sub->is_opt) TRY_ADD_OPT(sub);
    }
  }
  // Non-generic function signatures (concrete opt return/param types).
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_FUNC || d->as.func_decl.generic_params) continue;
    const AstFuncDecl* fd = &d->as.func_decl;
    if (fd->returns && fd->returns->type && fd->returns->type->is_opt)
      TRY_ADD_OPT(fd->returns->type);
    for (const AstParam* p = fd->params; p; p = p->next)
      if (p->type && p->type->is_opt) TRY_ADD_OPT(p->type);
  }
  // Specialized generic-function signatures (substituted opt return/params) —
  // e.g. `List(Track).get()` -> `opt Track`.
  for (size_t i = 0; i < ctx->specialized_func_count; i++) {
    const AstFuncDecl* fd = ctx->specialized_funcs[i].decl;
    const AstTypeRef* cargs = ctx->specialized_funcs[i].concrete_args;
    if (!fd) continue;
    const AstIdentifierPart* gps = fd->generic_params;
    if (!gps && fd->generic_template && fd->generic_template->kind == AST_DECL_FUNC)
      gps = fd->generic_template->as.func_decl.generic_params;
    if (fd->returns && fd->returns->type && fd->returns->type->is_opt) {
      AstTypeRef* sub = substitute_type_ref(ctx, gps, cargs, fd->returns->type);
      if (sub && sub->is_opt) TRY_ADD_OPT(sub);
    }
    for (const AstParam* p = fd->params; p; p = p->next) {
      if (p->type && p->type->is_opt) {
        AstTypeRef* sub = substitute_type_ref(ctx, gps, cargs, p->type);
        if (sub && sub->is_opt) TRY_ADD_OPT(sub);
      }
    }
  }
  #undef TRY_ADD_OPT
  for (size_t i = 0; i < opt_entry_count; i++) {
    if (opt_entries[i].needs_drop)
      fprintf(out, "RAE_UNUSED static void rae_drop_%s(%s* o);\n",
              opt_entries[i].optm, opt_entries[i].optm);
    if (opt_entries[i].needs_copy)
      fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src);\n",
              opt_entries[i].optm, opt_entries[i].optm, opt_entries[i].optm);
  }
  if (opt_entry_count > 0) fprintf(out, "\n");

  // #703: value-equality (`is`) for value-comparable structs. Forward-declare
  // all of them first (a nested field may reference a struct defined later),
  // then define. `a is b` / `a is not b` lower to rae_eq_<T> in c_expr.c.
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
      if (has_property(d->as.type_decl.properties, "c_struct")) continue;
      const AstTypeDecl* td = &d->as.type_decl;
      if (earlier_same_named_type(ctx, i, td->name)) continue;
      if (!rae_struct_value_comparable(module, td)) continue;
      const char* m = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = td->name}});
      fprintf(out, "RAE_UNUSED static rae_Bool rae_eq_%s(%s a, %s b);\n", m, m, m);
  }
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
      if (has_property(d->as.type_decl.properties, "c_struct")) continue;
      const AstTypeDecl* td = &d->as.type_decl;
      if (earlier_same_named_type(ctx, i, td->name)) continue;
      if (!rae_struct_value_comparable(module, td)) continue;
      const char* m = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = td->name}});
      fprintf(out, "RAE_UNUSED static rae_Bool rae_eq_%s(%s a, %s b) {\n  return (rae_Bool)(", m, m, m);
      bool efirst = true;
      for (const AstTypeField* f = td->fields; f; f = f->next) {
          Str fbase = get_base_type_name(f->type);
          int nl = (int)f->name.len; const char* nd = f->name.data;
          if (!efirst) fprintf(out, " && ");
          efirst = false;
          if (str_eq_cstr(fbase, "String")) {
              fprintf(out, "rae_ext_rae_str_eq(a.%.*s, b.%.*s)", nl, nd, nl, nd);
          } else if (!is_primitive_type(fbase) && !str_eq_cstr(fbase, "Bool")
                     && !str_eq_cstr(fbase, "Char") && !str_eq_cstr(fbase, "Char32")
                     && !find_enum_decl(NULL, module, fbase)) {
              // nested value-comparable struct
              const char* fm = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = fbase}});
              fprintf(out, "rae_eq_%s(a.%.*s, b.%.*s)", fm, nl, nd, nl, nd);
          } else {
              // scalar / bool / char / enum -> plain ==
              fprintf(out, "a.%.*s == b.%.*s", nl, nd, nl, nd);
          }
      }
      if (efirst) fprintf(out, "1");   // empty struct: always equal
      fprintf(out, ");\n}\n\n");
  }

  // Generate toJson/fromJson for non-generic user struct types
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
      if (has_property(d->as.type_decl.properties, "c_struct")) continue;
      const AstTypeDecl* td = &d->as.type_decl;
      if (earlier_same_named_type(ctx, i, td->name)) continue;
      const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = td->name}});

      // toJson: rae_String rae_toJson_TYPE_(TYPE* this)
      fprintf(out, "RAE_UNUSED static rae_String rae_toJson_%s_(%s* this) {\n", mangled, mangled);
      fprintf(out, "  char __buf[4096]; int __p = 0;\n");
      fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"{\");\n");
      bool first = true;
      for (const AstTypeField* f = td->fields; f; f = f->next) {
          if (!first) fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \", \");\n");
          first = false;
          Str base = get_base_type_name(f->type);
          if (f->type && f->type->is_opt) {
              // #651: a value-opt field is `struct rae_opt_<T> { has; value; }`
              // (except `opt Any`, still RaeAny). Serialize `value` per concrete
              // type when `has`, else JSON null. `opt Any` / aggregate payloads
              // that have no scalar spelling stay null (as before).
              CFuncContext _jctx = {0}; _jctx.compiler_ctx = ctx; _jctx.module = module;
              bool opt_struct = !f->type->is_view && !f->type->is_mod
                  && rae_opt_is_struct_rep(&_jctx, f->type);
              int nl = (int)f->name.len; const char* nd = f->name.data;
              if (opt_struct && str_eq_cstr(base, "String")) {
                  fprintf(out, "  if (this->%.*s.has) __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": \\\"%%.*s\\\"\", (int)this->%.*s.value.len, (char*)this->%.*s.value.data);\n",
                      nl, nd, nl, nd, nl, nd, nl, nd);
                  fprintf(out, "  else __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": null\");\n", nl, nd);
              } else if (opt_struct && (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int32"))) {
                  fprintf(out, "  if (this->%.*s.has) __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": %%lld\", (long long)this->%.*s.value);\n", nl, nd, nl, nd, nl, nd);
                  fprintf(out, "  else __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": null\");\n", nl, nd);
              } else if (opt_struct && (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32"))) {
                  fprintf(out, "  if (this->%.*s.has) __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": %%g\", (double)this->%.*s.value);\n", nl, nd, nl, nd, nl, nd);
                  fprintf(out, "  else __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": null\");\n", nl, nd);
              } else if (opt_struct && str_eq_cstr(base, "Bool")) {
                  fprintf(out, "  if (this->%.*s.has) __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": %%s\", this->%.*s.value ? \"true\" : \"false\");\n", nl, nd, nl, nd, nl, nd);
                  fprintf(out, "  else __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": null\");\n", nl, nd);
              } else {
                  // opt Any / opt <aggregate/enum/char> — no scalar JSON form.
                  fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": null\");\n", nl, nd);
              }
          } else if (str_eq_cstr(base, "String")) {
              fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": \\\"%%.*s\\\"\", (int)this->%.*s.len, (char*)this->%.*s.data);\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int32")) {
              fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": %%lld\", (long long)this->%.*s);\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) {
              fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": %%g\", this->%.*s);\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (str_eq_cstr(base, "Bool")) {
              fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": %%s\", this->%.*s ? \"true\" : \"false\");\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (find_enum_decl(NULL, module, base)) {
              // #767: an enum field serializes as its member NAME string, so
              // fromJson can round-trip it (was the "...": placeholder below).
              fprintf(out, "  { rae_String __e = rae_enum_toString_%.*s((int64_t)this->%.*s); __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": \\\"%%.*s\\\"\", (int)__e.len, (char*)__e.data); }\n",
                  (int)base.len, base.data, (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else {
              fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"\\\"%.*s\\\": ...\");\n",
                  (int)f->name.len, f->name.data);
          }
      }
      fprintf(out, "  __p += snprintf(__buf + __p, sizeof(__buf) - __p, \"}\");\n");
      fprintf(out, "  return rae_json_build(__buf, __p);\n}\n\n");

      // fromJson: TYPE rae_fromJson_TYPE_(rae_String json)
      fprintf(out, "RAE_UNUSED static %s rae_fromJson_%s_(rae_String json) {\n", mangled, mangled);
      fprintf(out, "  %s __r = {0};\n", mangled);
      for (const AstTypeField* f = td->fields; f; f = f->next) {
          Str base = get_base_type_name(f->type);
          if (f->type && f->type->is_opt) {
              // #651: parse into `struct rae_opt_<T>{ has, value }`. A present,
              // non-null key -> has=1 + decoded value; absent/null -> stays {0}
              // (none). `opt Any` / aggregate payloads have no scalar decoder so
              // stay none.
              CFuncContext _jctx = {0}; _jctx.compiler_ctx = ctx; _jctx.module = module;
              bool opt_struct = !f->type->is_view && !f->type->is_mod
                  && rae_opt_is_struct_rep(&_jctx, f->type);
              int nl = (int)f->name.len; const char* nd = f->name.data;
              const char* extractor = NULL;
              if (opt_struct) {
                  if (str_eq_cstr(base, "String")) extractor = "rae_json_extract_string";
                  else if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int32")) extractor = "rae_json_extract_int";
                  else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) extractor = "rae_json_extract_float";
                  else if (str_eq_cstr(base, "Bool")) extractor = "rae_json_extract_bool";
              }
              if (extractor) {
                  fprintf(out, "  { rae_Bool __h = rae_json_key_present(json, \"%.*s\"); __r.%.*s.has = __h; if (__h) __r.%.*s.value = %s(json, \"%.*s\"); }\n",
                      nl, nd, nl, nd, nl, nd, extractor, nl, nd);
              }
          } else if (str_eq_cstr(base, "String")) {
              fprintf(out, "  __r.%.*s = rae_json_extract_string(json, \"%.*s\");\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int32")) {
              fprintf(out, "  __r.%.*s = rae_json_extract_int(json, \"%.*s\");\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) {
              fprintf(out, "  __r.%.*s = rae_json_extract_float(json, \"%.*s\");\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (str_eq_cstr(base, "Bool")) {
              fprintf(out, "  __r.%.*s = rae_json_extract_bool(json, \"%.*s\");\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data);
          } else if (find_enum_decl(NULL, module, base)) {
              // #767: parse the member name string toJson wrote back to its
              // ordinal. Free the extracted temp (it is malloc'd + mem-tagged) so
              // the leak-gated tests stay at outstanding=0.
              fprintf(out, "  { rae_String __s = rae_json_extract_string(json, \"%.*s\"); __r.%.*s = rae_enum_fromString_%.*s(__s); rae_ext_rae_str_free(__s); }\n",
                  (int)f->name.len, f->name.data, (int)f->name.len, f->name.data, (int)base.len, base.data);
          }
      }
      fprintf(out, "  return __r;\n}\n\n");
  }

  // Generate rae_to_str_TYPE_ for non-c_struct user types so interpolation
  // (`"{p}"`) and `.toString()` produce the same "{ 10, 20 }" output the
  // Live VM gives. The _Generic-based rae_ext_rae_str macro can't be
  // extended for user types, so we emit a per-type function and switch
  // call sites to call it directly when the arg type is a user struct.
  // Emit forward declarations first so structs can reference each other
  // regardless of source order.
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
      if (has_property(d->as.type_decl.properties, "c_struct")) continue;
      const AstTypeDecl* td = &d->as.type_decl;
      if (earlier_same_named_type(ctx, i, td->name)) continue;
      const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = td->name}});
      fprintf(out, "RAE_UNUSED static rae_String rae_to_str_%s_(const %s* this);\n", mangled, mangled);
  }
  fprintf(out, "\n");
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
      if (has_property(d->as.type_decl.properties, "c_struct")) continue;
      const AstTypeDecl* td = &d->as.type_decl;
      if (earlier_same_named_type(ctx, i, td->name)) continue;
      const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = td->name}});

      fprintf(out, "RAE_UNUSED static rae_String rae_to_str_%s_(const %s* this) {\n", mangled, mangled);
      fprintf(out, "  rae_String __out = (rae_String){(uint8_t*)\"{ \", 2};\n");
      bool first = true;
      for (const AstTypeField* f = td->fields; f; f = f->next) {
          if (!first) fprintf(out, "  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)\", \", 2});\n");
          first = false;
          Str fbase = get_base_type_name(f->type);
          // opt T fields are stored as RaeAny; routing through the _Generic
          // macro picks rae_ext_rae_str_any. Nested concrete user structs go
          // through their own rae_to_str_; c_struct fields (raylib Color etc.)
          // and generic instantiations (List(Int), Map(K,V)) have no entry in
          // the _Generic macro, so render them as a "<Type>" placeholder
          // rather than hit a compile error.
          CFuncContext lookup_ctx = {.compiler_ctx = ctx, .module = module};
          const AstDecl* fd = find_type_decl(&lookup_ctx, module, fbase);
          bool is_user_struct = fd && fd->kind == AST_DECL_TYPE
              && !has_property(fd->as.type_decl.properties, "c_struct")
              && !fd->as.type_decl.generic_params;
          bool is_c_struct = fd && fd->kind == AST_DECL_TYPE
              && has_property(fd->as.type_decl.properties, "c_struct");
          bool has_generic_args = f->type && f->type->generic_args;
          bool is_generic_template = fd && fd->kind == AST_DECL_TYPE && fd->as.type_decl.generic_params;
          bool is_opt_field = f->type && f->type->is_opt;
          CFuncContext _sctx = {0}; _sctx.compiler_ctx = ctx; _sctx.module = module;
          bool is_opt_struct_field = is_opt_field && f->type && !f->type->is_view
              && !f->type->is_mod && rae_opt_is_struct_rep(&_sctx, f->type);
          if (is_opt_struct_field) {
              // A struct-rep opt field is `struct rae_opt_<T>` (#651). Format
              // `.value` per representation when present, else "none" (matching
              // the Live VM). Scalars/String/Char/Bool/enum route through the
              // _Generic rae_ext_rae_str on the concrete `.value`; a nested user
              // struct payload uses its own rae_to_str_. Aggregates without a
              // formatter fall back to a "<opt>" placeholder.
              int nl = (int)f->name.len; const char* nd = f->name.data;
              if (is_user_struct) {
                  const char* fmangled = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = fbase}});
                  fprintf(out, "  __out = rae_ext_rae_str_concat(__out, this->%.*s.has ? rae_to_str_%s_(&this->%.*s.value) : (rae_String){(uint8_t*)\"none\", 4});\n",
                      nl, nd, fmangled, nl, nd);
              } else if (!has_generic_args && !is_generic_template && !is_c_struct) {
                  // scalar / String / Char / Bool / enum -> _Generic on value.
                  fprintf(out, "  __out = rae_ext_rae_str_concat(__out, this->%.*s.has ? rae_ext_rae_str(this->%.*s.value) : (rae_String){(uint8_t*)\"none\", 4});\n",
                      nl, nd, nl, nd);
              } else {
                  fprintf(out, "  __out = rae_ext_rae_str_concat(__out, this->%.*s.has ? (rae_String){(uint8_t*)\"<opt>\", 5} : (rae_String){(uint8_t*)\"none\", 4});\n",
                      nl, nd);
              }
          } else if (is_user_struct && !is_opt_field && !has_generic_args) {
              const char* fmangled = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = fbase}});
              fprintf(out, "  __out = rae_ext_rae_str_concat(__out, rae_to_str_%s_(&this->%.*s));\n",
                  fmangled, (int)f->name.len, f->name.data);
          } else if ((is_c_struct || has_generic_args || is_generic_template) && !is_opt_field) {
              fprintf(out, "  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)\"<%.*s>\", %d});\n",
                  (int)fbase.len, fbase.data, (int)fbase.len + 2);
          } else {
              fprintf(out, "  __out = rae_ext_rae_str_concat(__out, rae_ext_rae_str(this->%.*s));\n",
                  (int)f->name.len, f->name.data);
          }
      }
      fprintf(out, "  __out = rae_ext_rae_str_concat(__out, (rae_String){(uint8_t*)\" }\", 2});\n");
      fprintf(out, "  return __out;\n}\n\n");
  }

  // Layer 5 (docs/scope-exit-dealloc.md) — synthesised per-struct
  // drop fns. For every non-generic user struct that transitively
  // owns heap (a field whose type is List / StringMap / IntMap, or
  // another heap-owning struct), emit
  //
  //   static void rae_drop_struct_<MangledType>(<MangledType>* this) {
  //     rae_drop_<field1_drop>(&this->field1);
  //     rae_drop_<field2_drop>(&this->field2);
  //     ...
  //   }
  //
  // c_stmt.c::emit_implicit_drops_for_body emits calls to these for
  // any heap-owning user-struct local at end of function body, so
  // dropping a UiWorld cascades through every ComponentTable, which
  // in turn drops its internal sparse + dense Lists.
  //
  // Scope deliberately limited to non-generic structs for now. The
  // generic-spec pass tripped the mangler on user struct types that
  // hadn't been touched elsewhere (JsonValue, AnimState, …); landing
  // that needs a separate refactor of how synthesised drops register
  // with the spec list. Container fields use the existing per-T
  // `drop(T)` from lib/core.rae, which already discovery-registers.
  typedef struct {
    const AstDecl* decl;
    const AstTypeRef* type_ref;  // non-NULL when this is a generic specialisation
    const char* mangled;
  } StructDropEntry;
  StructDropEntry drop_entries[512];
  size_t drop_entry_count = 0;
  // Pass A — non-generic user structs that transitively need cascade
  // drop. Uses the permissive predicate (includes String fields), so
  // string-only structs (e.g. SceneNode, StringMapEntry, Theme tokens
  // synthesised by parsers) also get a rae_drop_struct_<T> helper.
  // The synthesised body drops every owned heap field — Lists/Maps
  // via their generic drop overloads, Strings via rae_string_drop, and
  // nested user structs via recursion.
  //
  // Call-site gating: emit_implicit_drops_for_body only invokes this
  // for locals whose `local_struct_owns_heap` flag is set (struct
  // literal init or auto-init). Container extraction lets
  // (`let v: T = list.get(i)`) and bare-ident copies are flagged as
  // aliasing and skip the cascade — they keep their pre-Phase-3 leak
  // status. Function-call results are conservatively aliasing too;
  // callees that genuinely transfer ownership need to return into
  // a struct literal at the call site to trigger cascade today.
  for (size_t i = 0; i < ctx->all_decl_count && drop_entry_count < 512; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_TYPE) continue;
    if (d->as.type_decl.generic_params) continue;
    if (has_property(d->as.type_decl.properties, "c_struct")) continue;
    AstIdentifierPart* part = malloc(sizeof(AstIdentifierPart));
    *part = (AstIdentifierPart){.text = d->as.type_decl.name};
    AstTypeRef* tr = malloc(sizeof(AstTypeRef));
    *tr = (AstTypeRef){.parts = part};
    if (!type_needs_cascade_drop(ctx, module, tr, 0)) { free(tr); free(part); continue; }
    const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, tr);
    drop_entries[drop_entry_count++] = (StructDropEntry){.decl = d, .type_ref = tr, .mangled = mangled};
  }
  // Pass A' — concrete generic struct specializations from
  // ctx->generic_types[] (populated by discover_specializations_module).
  // Closes Stage 1's last gap: backend-dependent cleanup for valid
  // Rae code (the Live VM already cascades these via vm_drop.c since
  // commit 0a3023e; the C backend used to skip them, leaking the
  // inner heap on locals of type `Wrapper(String)` / `Pair(String,
  // List(String))` / etc.).
  //
  // Selection mirrors vm_drop's Pass 1b:
  //   * skip leaf containers (List/StringMap/IntMap/Buffer/Opt)
  //     — they have their own per-T drop overload mechanism that
  //     Pass C's field dispatch wires up.
  //   * skip c_struct
  //   * require the template to be a user generic struct
  //   * require the SUBSTITUTED fields to cascade (avoids emitting
  //     a helper for `Wrapper(Int)` whose only field is a primitive).
  //   * dedup by mangled name to handle duplicate AstTypeRef
  //     entries surfacing the same spec.
  //
  // `Box(T)` used to be on the skip list as if it were a stdlib leaf
  // container, but it isn't — there's no built-in Box and no per-T
  // `drop(Box(T))` overload. Treating it as a regular user struct
  // means `Box(String)` correctly gets a synthesised cascade drop.
  for (size_t gi = 0;
       gi < ctx->generic_type_count && drop_entry_count < 512;
       gi++) {
    const AstTypeRef* gt = ctx->generic_types[gi];
    if (!gt || gt->is_view || gt->is_mod || gt->is_opt) continue;
    if (!gt->generic_args) continue;
    Str gb = get_base_type_name(gt);
    if (str_eq_cstr(gb, "List")
        || str_eq_cstr(gb, "StringMap")
        || str_eq_cstr(gb, "IntMap")
        || str_eq_cstr(gb, "Buffer")
        || str_eq_cstr(gb, "Opt")) continue;
    const AstDecl* tdecl = NULL;
    for (size_t k = 0; k < ctx->all_decl_count; k++) {
      const AstDecl* d = ctx->all_decls[k];
      if (d->kind != AST_DECL_TYPE) continue;
      if (d->as.type_decl.specialization_args) continue;
      if (!str_eq(d->as.type_decl.name, gb)) continue;
      tdecl = d;
      break;
    }
    if (!tdecl
        || !tdecl->as.type_decl.generic_params
        || has_property(tdecl->as.type_decl.properties, "c_struct")) continue;
    if (!type_needs_cascade_drop(ctx, module, (AstTypeRef*)gt, 0)) continue;
    const char* mangled =
        rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)gt);
    if (!mangled) continue;
    bool seen = false;
    for (size_t k = 0; k < drop_entry_count; k++) {
      if (drop_entries[k].mangled
          && strcmp(drop_entries[k].mangled, mangled) == 0) {
        seen = true; break;
      }
    }
    if (seen) continue;
    drop_entries[drop_entry_count++] = (StructDropEntry){
      .decl = tdecl, .type_ref = gt, .mangled = mangled,
    };
  }
  // Array(T, cap: N) drop helpers.
  //
  // Emitted by their own pass rather than through StructDropEntry: an Array
  // has no AstDecl and no fields, just N identical elements, so the body is a
  // loop where the struct path is a field walk. Only arrays whose ELEMENT
  // cascades appear here — Array(Float, cap: 16) gets no helper at all, which
  // is the renderer case and must stay exactly free.
  const TypeInfo* array_drops[128];
  size_t array_drop_count = 0;
  for (size_t gi = 0; gi < ctx->generic_type_count && array_drop_count < 128; gi++) {
    const AstTypeRef* gt = ctx->generic_types[gi];
    if (!gt || gt->is_view || gt->is_mod || !gt->resolved_type) continue;
    const TypeInfo* at = gt->resolved_type;
    if (at->kind == TYPE_REF) at = at->as.ref.base;
    if (!at || at->kind != TYPE_ARRAY) continue;
    if (!type_needs_cascade_drop(ctx, module, (AstTypeRef*)gt, 0)) continue;
    bool seen = false;
    for (size_t k = 0; k < array_drop_count; k++) if (array_drops[k] == at) { seen = true; break; }
    if (!seen) array_drops[array_drop_count++] = at;
  }
  for (size_t i = 0; i < array_drop_count; i++) {
    const TypeInfo* at = array_drops[i];
    const char* am = type_mangle_name(ctx->ast_arena, (TypeInfo*)at).data;
    fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s(%s* this);\n", am, am);
    fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s_alias(%s* this);\n", am, am);
  }
  for (size_t i = 0; i < array_drop_count; i++) {
    const TypeInfo* at = array_drops[i];
    const char* am = type_mangle_name(ctx->ast_arena, (TypeInfo*)at).data;
    AstTypeRef elem = {0}; elem.resolved_type = (TypeInfo*)at->as.array.base;
    const char* em = type_mangle_name(ctx->ast_arena, (TypeInfo*)at->as.array.base).data;
    bool elem_is_string = at->as.array.base->kind == TYPE_STRING;
    for (int is_alias = 0; is_alias < 2; is_alias++) {
      fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s%s(%s* this) {\n",
              am, is_alias ? "_alias" : "", am);
      /* The _alias variant skips String elements for the same reason the
       * struct path does: a call-result local's Strings may alias the
       * callee's storage, and freeing them would double-free. */
      if (!(is_alias && elem_is_string)) {
        fprintf(out, "  for (int64_t __i = 0; __i < %lld; __i++) {\n",
                (long long)at->as.array.count);
        if (elem_is_string) fprintf(out, "    rae_string_drop(&this->v[__i]);\n");
        else fprintf(out, "    rae_drop_struct_%s%s(&this->v[__i]);\n", em, is_alias ? "_alias" : "");
        fprintf(out, "  }\n");
      } else {
        fprintf(out, "  (void)this;\n");
      }
      fprintf(out, "}\n");
    }
  }
  // Forward declarations — each struct drop AND each container-drop
  // we plan to call from the bodies. The container-drop forward
  // decls also register the specialisations so the iterative spec-
  // emission pass later in this function actually emits the body.
  // Phase 3: we emit TWO drop variants per struct:
  //   rae_drop_struct_<T>       — full cascade (drops String fields too).
  //                               Used for struct-literal/auto-init locals
  //                               that uniquely own their heap.
  //   rae_drop_struct_<T>_alias — strict cascade (skips String fields,
  //                               keeps List/Map drops). Used for
  //                               call-result locals whose String fields
  //                               might alias the callee's storage.
  // The strict variant preserves the pre-Phase-3 leak/no-crash invariant
  // for call-result locals; the full variant closes the Phase 2
  // struct-literal-String leak. Nested-struct recursion stays in mode.
  for (size_t i = 0; i < drop_entry_count; i++) {
    fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s(%s* this);\n",
            drop_entries[i].mangled, drop_entries[i].mangled);
    fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s_alias(%s* this);\n",
            drop_entries[i].mangled, drop_entries[i].mangled);
  }
  for (size_t i = 0; i < drop_entry_count; i++) {
    const StructDropEntry* e = &drop_entries[i];
    const AstIdentifierPart* gp = e->decl->as.type_decl.generic_params;
    const AstTypeRef* ga = (e->type_ref && gp) ? e->type_ref->generic_args : NULL;
    for (const AstTypeField* f = e->decl->as.type_decl.fields; f; f = f->next) {
      AstTypeRef* concrete = (gp && ga) ? substitute_type_ref(ctx, gp, ga, f->type) : f->type;
      if (!type_needs_cascade_drop(ctx, module, concrete, 0)) continue;
      if (concrete && concrete->is_opt) continue;
      Str fbase = get_base_type_name(concrete);
      // String fields go through the runtime helper rae_string_drop —
      // no forward decl needed for that path.
      if (str_eq_cstr(fbase, "String")) continue;
      const AstFuncDecl* drop_fd = NULL;
      for (size_t k = 0; k < ctx->all_decl_count; k++) {
        const AstDecl* dd = ctx->all_decls[k];
        if (dd->kind != AST_DECL_FUNC) continue;
        if (!str_eq_cstr(dd->as.func_decl.name, "drop")) continue;
        if (!dd->as.func_decl.generic_params) continue;
        const AstParam* first = dd->as.func_decl.params;
        if (!first || !first->type) continue;
        Str dp_base = get_base_type_name(first->type);
        if (str_eq(dp_base, fbase)) { drop_fd = &dd->as.func_decl; break; }
      }
      if (!drop_fd) continue;
      const AstTypeRef* elem_type = concrete->generic_args;
      if (!elem_type) continue;
      register_function_specialization(ctx, drop_fd, elem_type);
      const char* fn = rae_mangle_specialized_function(ctx, drop_fd, elem_type);
      const char* container_mangled = rae_mangle_type_specialized(ctx, NULL, NULL, concrete);
      fprintf(out, "RAE_UNUSED static void %s(%s* this);\n", fn, container_mangled);
    }
  }
  if (drop_entry_count > 0) fprintf(out, "\n");
  // Re-run discovery so the drop specialisations we just registered
  // (e.g. `drop(T)(this: mod ComponentTable(T))` for each T) get
  // their bodies walked and their own nested specs (e.g.
  // `drop(List(T))` inside ComponentTable's drop body) registered
  // BEFORE the spec-emission pipeline writes call sites. Without
  // this re-discovery, those nested calls go out as undeclared
  // C functions because the prototype comes later in the output.
  collect_type_refs_module(ctx);
  // Bodies — reverse field order so LIFO drop matches construction.
  // Emits both `rae_drop_struct_<T>` (full) and
  // `rae_drop_struct_<T>_alias` (skip String fields) in a single
  // walk. `is_alias` flips the String/recursive branch behaviour;
  // List/Map drops are identical in both modes (the container owns
  // its elements regardless of how the enclosing struct was bound).
  for (size_t i = 0; i < drop_entry_count; i++) {
    const StructDropEntry* e = &drop_entries[i];
    const AstIdentifierPart* gp = e->decl->as.type_decl.generic_params;
    const AstTypeRef* ga = (e->type_ref && gp) ? e->type_ref->generic_args : NULL;
    const AstTypeField* fields[256];
    size_t field_count = 0;
    for (const AstTypeField* f = e->decl->as.type_decl.fields; f && field_count < 256; f = f->next) {
      fields[field_count++] = f;
    }
    for (int is_alias = 0; is_alias < 2; is_alias++) {
      fprintf(out, "RAE_UNUSED static void rae_drop_struct_%s%s(%s* this) {\n",
              e->mangled, is_alias ? "_alias" : "", e->mangled);
      for (size_t j = field_count; j > 0; j--) {
        const AstTypeField* f = fields[j - 1];
        AstTypeRef* concrete = (gp && ga) ? substitute_type_ref(ctx, gp, ga, f->type) : f->type;
        if (!type_needs_cascade_drop(ctx, module, concrete, 0)) continue;
        if (concrete && concrete->is_opt) {
          if (is_alias) continue;
          CFuncContext fctx = {0}; fctx.compiler_ctx = ctx; fctx.module = module;
          if (!concrete->is_view && !concrete->is_mod
              && rae_opt_is_struct_rep(&fctx, concrete)) {
            fprintf(out, "  rae_drop_%s(&this->%.*s);\n",
                    rae_opt_type_name(&fctx, concrete),
                    (int)f->name.len, f->name.data);
          } else {
            fprintf(out, "  rae_any_drop(&this->%.*s);\n",
                    (int)f->name.len, f->name.data);
          }
          continue;
        }
        Str fbase = get_base_type_name(concrete);
        if (str_eq_cstr(fbase, "String")) {
          // Alias variant: skip — the String might be a view into the
          // callee's storage and double-free would crash.
          if (is_alias) continue;
          fprintf(out, "  rae_string_drop(&this->%.*s);\n",
                  (int)f->name.len, f->name.data);
          continue;
        }
        // First: look for a generic `drop(T)(this: mod <base>(T))` in
        // user code or stdlib. Handles List / StringMap / IntMap (from
        // lib/core.rae) AND user-supplied container drops (e.g. the
        // explicit `drop(T)(this: mod ComponentTable(T))` in lib/ui/
        // ecs.rae). Falls through to the nested-struct path if none.
        const AstFuncDecl* drop_fd = NULL;
        for (size_t k = 0; k < ctx->all_decl_count; k++) {
          const AstDecl* dd = ctx->all_decls[k];
          if (dd->kind != AST_DECL_FUNC) continue;
          if (!str_eq_cstr(dd->as.func_decl.name, "drop")) continue;
          if (!dd->as.func_decl.generic_params) continue;
          const AstParam* first = dd->as.func_decl.params;
          if (!first || !first->type) continue;
          Str dp_base = get_base_type_name(first->type);
          if (str_eq(dp_base, fbase)) { drop_fd = &dd->as.func_decl; break; }
        }
        if (drop_fd) {
          const AstTypeRef* elem_type = concrete->generic_args;
          if (!elem_type) continue;
          // Alias variant: skip List/Map field drop when the element
          // type has String fields. The per-T drop now iterates
          // String elements; calling it in alias mode would double-
          // free Strings shared with the source. Buffers leak in
          // alias mode (same as the pre-Phase-3 status quo for
          // value-typed-struct extraction).
          if (is_alias
              && type_needs_cascade_drop(ctx, module, (AstTypeRef*)elem_type, 0)) {
            continue;
          }
          const char* fn = rae_mangle_specialized_function(ctx, drop_fd, elem_type);
          fprintf(out, "  %s(&this->%.*s);\n", fn,
                  (int)f->name.len, f->name.data);
        } else {
          // Nested non-generic user struct — recurse via the matching
          // mode (full -> full, alias -> alias).
          const char* fmangled = rae_mangle_type_specialized(ctx, NULL, NULL, concrete);
          fprintf(out, "  rae_drop_struct_%s%s(&this->%.*s);\n", fmangled,
                  is_alias ? "_alias" : "",
                  (int)f->name.len, f->name.data);
        }
      }
      fprintf(out, "}\n\n");
    }
  }

  // Phase 1+2: synthesise deep-copy helpers.
  //
  // Two function families, both named with `rae_deep_copy_<MangledType>`
  // (no `_struct_`/`_list_` distinction in the name — keeps callers
  // type-agnostic):
  //
  //   For non-generic user structs (T):
  //     static void rae_deep_copy_<T>(<T>* dst, const <T>* src)
  //
  //   For container specializations (List(E), StringMap(V), IntMap(V)):
  //     static void rae_deep_copy_<List_E>(<List_E>* dst, const <List_E>* src)
  //
  // The struct variant walks fields, dispatching per-field type:
  //   - String        → rae_string_copy
  //   - user struct U → recursive rae_deep_copy_<U>
  //   - List/Map      → recursive rae_deep_copy_<container_type>
  //   - view/mod      → pointer copy
  //   - primitive     → plain assignment
  //
  // The container variant allocates a fresh buffer, then walks the
  // src elements/entries deep-copying each.
  //
  // Used by c_stmt.c's let-stmt deep-copy path (`let b: T = a` where
  // `a` is a bare identifier and T needs deep copy).

  // Pass A: collect struct entries (permissive — string-only structs included).
  StructDropEntry copy_entries[512];
  size_t copy_entry_count = 0;
  for (size_t i = 0; i < ctx->all_decl_count && copy_entry_count < 512; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_TYPE) continue;
    if (d->as.type_decl.generic_params) continue;
    if (has_property(d->as.type_decl.properties, "c_struct")) continue;
    AstIdentifierPart* part = malloc(sizeof(AstIdentifierPart));
    *part = (AstIdentifierPart){.text = d->as.type_decl.name};
    AstTypeRef* tr = malloc(sizeof(AstTypeRef));
    *tr = (AstTypeRef){.parts = part};
    if (!type_needs_cascade_drop(ctx, module, tr, 0)) { free(tr); free(part); continue; }
    const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, tr);
    copy_entries[copy_entry_count++] = (StructDropEntry){.decl = d, .type_ref = tr, .mangled = mangled};
  }

  // Pass B: collect container specializations (List / StringMap / IntMap)
  // from the discovered generic_types list. Only collect ones whose
  // element type transitively needs deep copy OR which are StringMap
  // (because StringMap keys are heap-owned Strings that must always
  // be copied when the map is duplicated).
  typedef struct {
    const AstTypeRef* type_ref;   // List(E) / StringMap(V) / IntMap(V)
    const char* mangled;          // rae_List_<E>, rae_StringMap_<V>, rae_IntMap_<V>
    int kind;                     // 0=list, 1=smap, 2=imap
  } ContainerCopyEntry;
  ContainerCopyEntry container_entries[512];
  size_t container_entry_count = 0;
  for (size_t i = 0; i < ctx->generic_type_count && container_entry_count < 512; i++) {
    const AstTypeRef* gt = ctx->generic_types[i];
    if (!gt || gt->is_view || gt->is_mod || gt->is_opt) continue;
    Str gb = get_base_type_name(gt);
    bool is_list = str_eq_cstr(gb, "List");
    bool is_smap = str_eq_cstr(gb, "StringMap");
    bool is_imap = str_eq_cstr(gb, "IntMap");
    if (!is_list && !is_smap && !is_imap) continue;
    if (!gt->generic_args) continue;
    // Dedup by mangled name.
    const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)gt);
    bool seen = false;
    for (size_t k = 0; k < container_entry_count; k++) {
      if (strcmp(container_entries[k].mangled, mangled) == 0) { seen = true; break; }
    }
    if (seen) continue;
    container_entries[container_entry_count].type_ref = gt;
    container_entries[container_entry_count].mangled = mangled;
    container_entries[container_entry_count].kind = is_list ? 0 : (is_smap ? 1 : 2);
    container_entry_count++;
  }

  // Forward decls — structs first, then containers.
  for (size_t i = 0; i < copy_entry_count; i++) {
    fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src);\n",
            copy_entries[i].mangled, copy_entries[i].mangled, copy_entries[i].mangled);
  }
  for (size_t i = 0; i < container_entry_count; i++) {
    fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src);\n",
            container_entries[i].mangled, container_entries[i].mangled, container_entries[i].mangled);
  }
  // Legacy compat alias — older codegen paths and tests may still refer
  // to `rae_deep_copy_struct_<T>`. Keep the alias so we don't break them
  // while migrating callers to the unified name. (Marked RAE_UNUSED.)
  for (size_t i = 0; i < copy_entry_count; i++) {
    fprintf(out, "#define rae_deep_copy_struct_%s rae_deep_copy_%s\n",
            copy_entries[i].mangled, copy_entries[i].mangled);
  }
  if (copy_entry_count > 0 || container_entry_count > 0) fprintf(out, "\n");

  // Helper: emit a single per-field copy statement for a struct deep-copy
  // body, dispatching on the field's concrete type.
  #define EMIT_FIELD_COPY(dst_expr, src_expr, ft, fbase) do { \
      if ((ft) && ((ft)->is_view || (ft)->is_mod)) { \
        fprintf(out, "  %s = %s;\n", (dst_expr), (src_expr)); \
        break; \
      } \
      /* opt T is represented as RaeAny — dispatch on the \
       * REPRESENTATION before the base-type checks below, or an \
       * `opt String` field gets rae_string_copy(RaeAny) (#138). */ \
      if ((ft) && (ft)->is_opt) { \
        CFuncContext _octx = {0}; _octx.compiler_ctx = ctx; _octx.module = module; \
        if (!(ft)->is_view && !(ft)->is_mod && rae_opt_is_struct_rep(&_octx, (ft))) { \
          if (type_needs_deep_copy(ctx, module, (AstTypeRef*)(ft), 0)) \
            fprintf(out, "  rae_deep_copy_%s(&%s, &%s);\n", rae_opt_type_name(&_octx, (ft)), (dst_expr), (src_expr)); \
          else \
            fprintf(out, "  %s = %s;\n", (dst_expr), (src_expr)); \
        } else { \
          fprintf(out, "  %s = rae_any_copy(%s);\n", (dst_expr), (src_expr)); \
        } \
        break; \
      } \
      if (str_eq_cstr((fbase), "String")) { \
        fprintf(out, "  %s = rae_string_copy(%s);\n", (dst_expr), (src_expr)); \
        break; \
      } \
      bool _f_is_list = str_eq_cstr((fbase), "List"); \
      bool _f_is_smap = str_eq_cstr((fbase), "StringMap"); \
      bool _f_is_imap = str_eq_cstr((fbase), "IntMap"); \
      if ((_f_is_list || _f_is_smap || _f_is_imap) && (ft) && (ft)->generic_args) { \
        const char* _fmangled = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)(ft)); \
        fprintf(out, "  rae_deep_copy_%s(&%s, &%s);\n", _fmangled, (dst_expr), (src_expr)); \
        break; \
      } \
      if ((ft) && !(ft)->is_view && !(ft)->is_mod && !(ft)->is_opt \
          && type_needs_deep_copy(ctx, module, (AstTypeRef*)(ft), 0)) { \
        const AstDecl* _fd = find_type_decl(NULL, module, (fbase)); \
        bool _is_user_struct = _fd && _fd->kind == AST_DECL_TYPE \
            && !has_property(_fd->as.type_decl.properties, "c_struct") \
            && !_fd->as.type_decl.generic_params; \
        if (_is_user_struct) { \
          const char* _fm = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)(ft)); \
          fprintf(out, "  rae_deep_copy_%s(&%s, &%s);\n", _fm, (dst_expr), (src_expr)); \
          break; \
        } \
      } \
      fprintf(out, "  %s = %s;\n", (dst_expr), (src_expr)); \
  } while (0)

  // Struct bodies.
  for (size_t i = 0; i < copy_entry_count; i++) {
    const StructDropEntry* e = &copy_entries[i];
    fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src) {\n",
            e->mangled, e->mangled, e->mangled);
    for (const AstTypeField* f = e->decl->as.type_decl.fields; f; f = f->next) {
      const AstTypeRef* ft = f->type;
      Str fbase = ft ? get_base_type_name(ft) : (Str){0};
      char dst_expr[128];
      char src_expr[128];
      snprintf(dst_expr, sizeof(dst_expr), "dst->%.*s", (int)f->name.len, f->name.data);
      snprintf(src_expr, sizeof(src_expr), "src->%.*s", (int)f->name.len, f->name.data);
      EMIT_FIELD_COPY(dst_expr, src_expr, ft, fbase);
    }
    fprintf(out, "}\n\n");
  }

  // Container bodies. For List(E): allocate a fresh buffer sized to
  // src->capacity, then iterate elements deep-copying each. For
  // StringMap(V) / IntMap(V): allocate a fresh sparse buffer, iterate
  // entries, copy occupied ones with key (Strings deep-copied).
  for (size_t i = 0; i < container_entry_count; i++) {
    const ContainerCopyEntry* e = &container_entries[i];
    const AstTypeRef* elem = e->type_ref->generic_args;
    if (!elem) continue;
    Str ebase = get_base_type_name(elem);
    // opt T elements are RaeAny at the C level — classify by
    // representation first (#138), mirroring the drop synthesis.
    bool elem_is_opt = elem->is_opt;
    CFuncContext ecctx = {0}; ecctx.compiler_ctx = ctx; ecctx.module = module;
    bool elem_opt_struct = elem_is_opt && rae_opt_is_struct_rep(&ecctx, (AstTypeRef*)elem);
    const char* elem_mangled = (elem_is_opt && !elem_opt_struct)
        ? "RaeAny"
        : rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)elem);
    bool elem_is_string = !elem_is_opt && str_eq_cstr(ebase, "String");
    bool elem_is_list = !elem_is_opt && str_eq_cstr(ebase, "List");
    bool elem_is_smap = !elem_is_opt && str_eq_cstr(ebase, "StringMap");
    bool elem_is_imap = !elem_is_opt && str_eq_cstr(ebase, "IntMap");
    bool elem_is_container = elem_is_list || elem_is_smap || elem_is_imap;
    AstTypeRef elem_opt_payload = *elem;
    elem_opt_payload.is_opt = false; elem_opt_payload.next = NULL;
    elem_opt_payload.resolved_type = (elem->resolved_type && elem->resolved_type->kind == TYPE_OPT)
        ? elem->resolved_type->as.opt.base
        : (elem_opt_payload.parts ? NULL : elem->resolved_type);
    bool elem_opt_needs_copy = elem_opt_struct
        && type_needs_deep_copy(ctx, module, &elem_opt_payload, 0);
    bool elem_needs_deep = elem_is_string || elem_is_container || elem_opt_needs_copy ||
        type_needs_deep_copy(ctx, module, (AstTypeRef*)elem, 0);
    // Element C TYPE spelling (for casts/sizeof/pointer decls). A c_struct
    // element emits its BARE C name (WGPUColor, not rae_WGPUColor) — the same
    // name the List struct's `data` field uses — so List(WGPUColor) etc. work
    // for building descriptor arrays over the WebGPU bindings (#503). c_structs
    // are POD, so the bulk-copy path below is the one that uses it.
    const char* elem_c_type = elem_mangled;
    {
      const AstDecl* c_elem_decl = find_type_decl(NULL, module, ebase);
      if (c_elem_decl && c_elem_decl->kind == AST_DECL_TYPE &&
          has_property(c_elem_decl->as.type_decl.properties, "c_struct"))
        elem_c_type = str_to_cstr(ebase);
      // A `Task(T)` element lowers to the opaque `RaeTask*` (the struct field
      // uses `RaeTask** data`), not a `rae_Task_<T>` struct — mirror that here
      // so List(Task(T))'s deep-copy allocates/copies pointers, not a
      // nonexistent element struct (#238).
      else if (str_eq_cstr(ebase, "Task")) elem_c_type = "RaeTask*";
    }

    fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src) {\n",
            e->mangled, e->mangled, e->mangled);

    if (e->kind == 0) {
      // List(E): allocate buffer, copy elements.
      fprintf(out, "  dst->length = src->length;\n");
      fprintf(out, "  dst->cap = src->cap;\n");
      fprintf(out, "  if (src->cap > 0) {\n");
      fprintf(out, "    dst->data = (%s*)rae_ext_rae_buf_alloc(src->cap, sizeof(%s));\n",
              elem_c_type, elem_c_type);
      if (!elem_needs_deep) {
        // POD path — bulk copy.
        fprintf(out, "    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(%s));\n",
                elem_c_type);
      } else {
        fprintf(out, "    for (int64_t __i = 0; __i < src->length; __i++) {\n");
        if (elem_is_opt) {
          if (elem_opt_struct)
            fprintf(out, "      rae_deep_copy_%s(&dst->data[__i], &src->data[__i]);\n", elem_mangled);
          else
            fprintf(out, "      dst->data[__i] = rae_any_copy(src->data[__i]);\n");
        } else if (elem_is_string) {
          fprintf(out, "      dst->data[__i] = rae_string_copy(src->data[__i]);\n");
        } else if (elem_is_container) {
          fprintf(out, "      rae_deep_copy_%s(&dst->data[__i], &src->data[__i]);\n", elem_mangled);
        } else {
          // User struct element.
          fprintf(out, "      rae_deep_copy_%s(&dst->data[__i], &src->data[__i]);\n", elem_mangled);
        }
        fprintf(out, "    }\n");
      }
      fprintf(out, "  } else {\n");
      fprintf(out, "    dst->data = NULL;\n");
      fprintf(out, "  }\n");
    } else {
      // StringMap / IntMap — sparse buffer of entries.
      // Entry struct: rae_StringMapEntry_<V> { k: rae_String, value: V, occupied: rae_Bool }
      // or rae_IntMapEntry_<V> { k: int64_t, value: V, occupied: rae_Bool }
      const char* entry_struct = (e->kind == 1) ? "rae_StringMapEntry" : "rae_IntMapEntry";
      fprintf(out, "  dst->length = src->length;\n");
      fprintf(out, "  dst->cap = src->cap;\n");
      fprintf(out, "  if (src->cap > 0) {\n");
      fprintf(out, "    size_t __stride = sizeof(%s_%s);\n", entry_struct, elem_mangled);
      fprintf(out, "    dst->data = rae_ext_rae_buf_alloc(src->cap, (int64_t)__stride);\n");
      fprintf(out, "    memcpy(dst->data, src->data, (size_t)src->cap * __stride);\n");
      // Now deep-copy keys (if smap) and values (if needed) per occupied slot.
      fprintf(out, "    char* __sbuf = (char*)src->data;\n");
      fprintf(out, "    char* __dbuf = (char*)dst->data;\n");
      fprintf(out, "    for (int64_t __i = 0; __i < src->cap; __i++) {\n");
      fprintf(out, "      %s_%s* __se = (%s_%s*)(__sbuf + __i * __stride);\n",
              entry_struct, elem_mangled, entry_struct, elem_mangled);
      fprintf(out, "      %s_%s* __de = (%s_%s*)(__dbuf + __i * __stride);\n",
              entry_struct, elem_mangled, entry_struct, elem_mangled);
      fprintf(out, "      if (!__se->occupied) continue;\n");
      if (e->kind == 1) {
        // StringMap — copy key.
        fprintf(out, "      __de->k = rae_string_copy(__se->k);\n");
      }
      // Copy value per element type.
      if (elem_is_opt) {
        if (elem_opt_struct)
          fprintf(out, "      rae_deep_copy_%s(&__de->value, &__se->value);\n", elem_mangled);
        else
          fprintf(out, "      __de->value = rae_any_copy(__se->value);\n");
      } else if (elem_is_string) {
        fprintf(out, "      __de->value = rae_string_copy(__se->value);\n");
      } else if (elem_is_container) {
        fprintf(out, "      rae_deep_copy_%s(&__de->value, &__se->value);\n", elem_mangled);
      } else if (elem_needs_deep) {
        fprintf(out, "      rae_deep_copy_%s(&__de->value, &__se->value);\n", elem_mangled);
      }
      // POD value: already copied by the bulk memcpy above.
      fprintf(out, "    }\n");
      fprintf(out, "  } else {\n");
      fprintf(out, "    dst->data = NULL;\n");
      fprintf(out, "  }\n");
    }
    fprintf(out, "}\n\n");
  }
  #undef EMIT_FIELD_COPY

  // Value-optional helper BODIES. Payload drop/copy helpers are all declared
  // by now, so `rae_drop_<optT>` / `rae_deep_copy_<optT>` can call into them.
  for (size_t i = 0; i < opt_entry_count; i++) {
    OptHelperEntry* e = &opt_entries[i];
    CFuncContext octx = {0}; octx.compiler_ctx = ctx; octx.module = module;
    Str pbase = get_base_type_name(&e->payload);
    if (e->needs_drop) {
      fprintf(out, "RAE_UNUSED static void rae_drop_%s(%s* o) {\n", e->optm, e->optm);
      fprintf(out, "  if (!o->has) return;\n");
      if (str_eq_cstr(pbase, "Task")) {
        fprintf(out, "  rae_task_drop(o->value);\n");
      } else if (str_eq_cstr(pbase, "String")) {
        // #651: `opt String` is now struct-rep; its String payload drops
        // through the runtime string helper, not a struct cascade.
        fprintf(out, "  rae_string_drop(&o->value);\n");
      } else if (is_drop_target_type(&e->payload)) {
        const AstFuncDecl* drop_fd = find_drop_overload_for(&octx, pbase);
        const AstTypeRef* elem = e->payload.generic_args;
        if (drop_fd && elem) {
          register_function_specialization(ctx, drop_fd, elem);
          const char* fn = rae_mangle_specialized_function(ctx, drop_fd, elem);
          fprintf(out, "  %s(&o->value);\n", fn);
        }
      } else {
        const char* pm = rae_mangle_type_specialized(ctx, NULL, NULL, &e->payload);
        fprintf(out, "  rae_drop_struct_%s(&o->value);\n", pm);
      }
      fprintf(out, "}\n\n");
    }
    if (e->needs_copy) {
      fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src) {\n",
              e->optm, e->optm, e->optm);
      fprintf(out, "  dst->has = src->has;\n");
      fprintf(out, "  if (!src->has) return;\n");
      if (str_eq_cstr(pbase, "String")) {
        // #651: `opt String` deep-copies its String payload via rae_string_copy.
        fprintf(out, "  dst->value = rae_string_copy(src->value);\n");
      } else {
        // Struct and container payloads both use rae_deep_copy_<payload>.
        const char* pm = rae_mangle_type_specialized(ctx, NULL, NULL, &e->payload);
        fprintf(out, "  rae_deep_copy_%s(&dst->value, &src->value);\n", pm);
      }
      fprintf(out, "}\n\n");
    }
  }

  // Emit top-level `let` globals as static C variables. We bundle every
  // imported module into one translation unit, so plain `static` works
  // (no need for extern/header). Initialised lets get their initialiser
  // expression; uninitialised ones get the type's zero value.
  {
      CFuncContext gctx = {.compiler_ctx = ctx, .module = module};
      for (size_t i = 0; i < ctx->all_decl_count; i++) {
          const AstDecl* d = ctx->all_decls[i];
          if (d->kind != AST_DECL_GLOBAL_LET) continue;
          fprintf(out, "RAE_UNUSED static ");
          if (d->as.let_decl.type) emit_type_ref_as_c_type(&gctx, d->as.let_decl.type, out, false);
          else fprintf(out, "int64_t");
          fprintf(out, " %.*s = ", (int)d->as.let_decl.name.len, d->as.let_decl.name.data);
          if (d->as.let_decl.value && !global_init_is_deferred(d->as.let_decl.value))
              emit_expr(&gctx, d->as.let_decl.value, out, PREC_LOWEST, false, false);
          else
              // Deferred (function-call) init OR no init: zero-initialise here;
              // deferred ones are assigned at the top of main().
              emit_auto_init(&gctx, d->as.let_decl.type, out);
          fprintf(out, ";\n");
      }
      fprintf(out, "\n");
  }

  // Forward declarations for user extern functions (not in runtime header)
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && d->as.func_decl.is_extern && !d->as.func_decl.generic_params) {
          const char* mangled = rae_mangle_function(ctx, &d->as.func_decl);
          // An explicit extern("symbol") binds a symbol DECLARED ELSEWHERE — by
          // a cheader'd library header (webgpu.h) or a standard header the
          // runtime already includes. Emitting our own prototype would clash
          // with the real one (our ABI-compat void*/int32_t view of the types
          // differs from WGPUInstance/WGPUStatus/const-qualified pointers), so
          // let the header be the single source of truth (general FFI, #497).
          if (d->as.func_decl.extern_symbol) continue;
          // Skip functions already declared in runtime header (rae_ext_rae_* and known builtins)
          if (str_starts_with_cstr(d->as.func_decl.name, "rae_ext_") ||
              str_starts_with_cstr(d->as.func_decl.name, "rae_") ||
              str_starts_with_cstr(d->as.func_decl.name, "__buf_")) continue;
          CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .func_decl = &d->as.func_decl};
          fprintf(out, "%s %s(", c_return_type(&tctx, &d->as.func_decl), mangled);
          emit_param_list(&tctx, d->as.func_decl.params, out, true);
          fprintf(out, ");\n");
      }
  }

  // Prototypes for non-generic functions
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params && !d->as.func_decl.specialization_args && !d->as.func_decl.is_extern && !str_eq_cstr(d->as.func_decl.name, "main")) {
          CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .func_decl = &d->as.func_decl};
          fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&tctx, &d->as.func_decl), rae_mangle_function(ctx, &d->as.func_decl));
          emit_param_list(&tctx, d->as.func_decl.params, out, false);
          fprintf(out, ");\n");
      }
  }

  // Prototypes for specialized functions (skip externs — their call sites are inlined)
  for (size_t i = 0; i < ctx->specialized_func_count; i++) {
      const AstFuncDecl* f = ctx->specialized_funcs[i].decl; const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
      if (f->is_extern) continue;
      const char* mangled = rae_mangle_specialized_function(ctx, f, args);
      const AstIdentifierPart* gp = f->generic_params;
      if (!gp && f->generic_template && f->generic_template->kind == AST_DECL_FUNC) gp = f->generic_template->as.func_decl.generic_params;
      CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .generic_params = gp, .generic_args = args};
      fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&tctx, f), mangled); emit_param_list(&tctx, f->params, out, false); fprintf(out, ");\n");
  }
  
  // Path-1 spawn thunks: one per threadable function (all params passed by
  // value, so a worker thread safely owns its copies). pthread needs a
  // void*(*)(void*); the thunk unpacks the args struct, runs the function,
  // stores the result into the task, and frees the struct. Over-emitted for
  // every threadable function (RAE_UNUSED) to avoid a separate discovery pass.
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_FUNC || str_eq_cstr(d->as.func_decl.name, "main")) continue;
      const AstFuncDecl* f = &d->as.func_decl;
      CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .func_decl = f};
      if (!c_spawn_threadable(&tctx, f)) continue;
      const char* mangled = rae_mangle_function(ctx, f);
      const char* rt = c_return_type(&tctx, f);
      bool is_void = (strcmp(rt, "void") == 0);
      fprintf(out, "typedef struct { ");
      int pi = 0;
      for (const AstParam* p = f->params; p; p = p->next, pi++) {
          AstTypeRef vt = *p->type; vt.is_view = false; vt.is_mod = false;
          emit_type_ref_as_c_type(&tctx, &vt, out, false);
          fprintf(out, " f%d; ", pi);
      }
      fprintf(out, "RaeTask* __task; } __raespawn_args_%s;\n", mangled);
      fprintf(out, "RAE_UNUSED static void* __raespawn_thunk_%s(void* __vp) {\n", mangled);
      fprintf(out, "  __raespawn_args_%s* __a = (__raespawn_args_%s*)__vp;\n", mangled, mangled);
      if (!is_void) fprintf(out, "  *(%s*)__a->__task->result = %s(", rt, mangled);
      else fprintf(out, "  %s(", mangled);
      for (int k = 0; k < pi; k++) { if (k) fprintf(out, ", "); fprintf(out, "__a->f%d", k); }
      fprintf(out, ");\n  __a->__task->done = 1; free(__a); return ((void*)0);\n}\n");
  }

  // Bodies for non-generic functions
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params && !d->as.func_decl.specialization_args && !d->as.func_decl.is_extern && !str_eq_cstr(d->as.func_decl.name, "main")) {
          emit_function(ctx, module, &d->as.func_decl, out, registry, false);
      }
  }

  // Bodies for specialized functions (iterative — emitting may discover new specializations)
  // First: emit ALL prototypes from discovery pass (may include ones found during iterative discovery)
  //
  // `register_function_specialization` dedupes by (decl, concrete_args)
  // tuple, so entries in `specialized_funcs` are unique. The previous
  // implementation re-mangled every previous entry on every iteration
  // (O(N²) mangle calls) to dedup on name, which was redundant and
  // exhausted the arena on large modules (~480 specs × ~480 dedup
  // mangles × ~70-byte names = many megabytes of arena allocations).
  // If two different decls ever do produce the same mangled name,
  // that's a mangler bug worth surfacing as a C link error rather
  // than papering over here.
  for (size_t i = 0; i < ctx->specialized_func_count; i++) {
      const AstFuncDecl* pf = ctx->specialized_funcs[i].decl;
      const AstTypeRef* pa = ctx->specialized_funcs[i].concrete_args;
      if (pf->is_extern) continue;
      const char* pm = rae_mangle_specialized_function(ctx, pf, pa);
      const AstIdentifierPart* pgp = pf->generic_params;
      if (!pgp && pf->generic_template && pf->generic_template->kind == AST_DECL_FUNC) pgp = pf->generic_template->as.func_decl.generic_params;
      CFuncContext ptctx = {.compiler_ctx = ctx, .module = module, .generic_params = pgp, .generic_args = pa};
      fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&ptctx, pf), pm); emit_param_list(&ptctx, pf->params, out, false); fprintf(out, ");\n");
  }
  {
      size_t emitted_idx = 0;
      size_t prototyped_count = ctx->specialized_func_count; // already prototyped above
      while (emitted_idx < ctx->specialized_func_count) {
          // Emit prototypes for any newly discovered specializations
          while (prototyped_count < ctx->specialized_func_count) {
              const AstFuncDecl* f = ctx->specialized_funcs[prototyped_count].decl;
              const AstTypeRef* args = ctx->specialized_funcs[prototyped_count].concrete_args;
              prototyped_count++;
              if (f->is_extern) continue;
              const char* mangled = rae_mangle_specialized_function(ctx, f, args);
              const AstIdentifierPart* gp = f->generic_params;
              if (!gp && f->generic_template && f->generic_template->kind == AST_DECL_FUNC)
                  gp = f->generic_template->as.func_decl.generic_params;
              CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .generic_params = gp, .generic_args = args};
              fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&tctx, f), mangled);
              emit_param_list(&tctx, f->params, out, false);
              fprintf(out, ");\n");
          }
          emit_specialized_function(ctx, module, ctx->specialized_funcs[emitted_idx].decl, ctx->specialized_funcs[emitted_idx].concrete_args, out, registry, false);
          emitted_idx++;
      }
  }
  
  // Finally emit main
  size_t pre_main_spec_count = ctx->specialized_func_count;
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && str_eq_cstr(d->as.func_decl.name, "main")) {
          emit_function(ctx, module, &d->as.func_decl, out, registry, false);
      }
  }

  // Emit any specializations discovered during main (e.g. from collection literals)
  {
      size_t emitted_idx2 = pre_main_spec_count;
      while (emitted_idx2 < ctx->specialized_func_count) {
          const AstFuncDecl* f = ctx->specialized_funcs[emitted_idx2].decl;
          const AstTypeRef* args = ctx->specialized_funcs[emitted_idx2].concrete_args;
          emitted_idx2++;
          if (f->is_extern) continue;
          const char* mangled = rae_mangle_specialized_function(ctx, f, args);
          const AstIdentifierPart* gp = f->generic_params;
          if (!gp && f->generic_template && f->generic_template->kind == AST_DECL_FUNC)
              gp = f->generic_template->as.func_decl.generic_params;
          CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .generic_params = gp, .generic_args = args};
          fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&tctx, f), mangled);
          emit_param_list(&tctx, f->params, out, false);
          fprintf(out, ");\n");
          emit_specialized_function(ctx, module, f, args, out, registry, false);
      }
  }

  fclose(out); return true;
}
