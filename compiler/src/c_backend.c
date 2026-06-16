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

void emit_type_info_as_c_type(CFuncContext* ctx, TypeInfo* t, FILE* out) {
    if (!t) { fprintf(out, "RaeAny"); return; }
    AstTypeRef tmp = {0};
    tmp.resolved_type = t;
    emit_type_ref_as_c_type(ctx, &tmp, out, false);
}

bool emit_type_recursive(CompilerContext* ctx, const AstModule* m, const AstTypeRef* type, FILE* out, EmittedTypeList* emitted, EmittedTypeList* visiting, bool ray) {
    if (!type) return true;
    
    if (type->resolved_type) {
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
    // Buffer and List are already pointers — no wrapper struct
    if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List") || str_eq_cstr(base, "Any")) return false;
    // Stage 6: for plain numeric/bool/char primitives, `view T` lowers
    // to the same pass-by-value machine code as bare T — the source-
    // level `view` is semantic intent only. Only `mod T` keeps a true
    // reference wrapper, because the callee must write back through
    // the pointer. String stays a ref under view/mod because it is
    // heap-owning at the language level (the ref avoids deep copies).
    bool is_num_prim = str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64") ||
        str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float64") ||
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
    // Don't register types with void generic args (spurious specializations)
    for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
        Str ab = get_base_type_name(a);
        if (str_eq_cstr(ab, "void") || (a->resolved_type && a->resolved_type->kind == TYPE_VOID)) return;
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
      default: { if ((unsigned char)c < 32 || (unsigned char)c > 126) fprintf(out, "\\x%02x", (unsigned char)c); else fputc(c, out); break; }
    }
  }
  fprintf(out, "\", %lld}", (long long)literal.len); return true;
}

bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out, bool skip_ptr) {
  if (!type) { fprintf(out, "int64_t"); return true; }
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

      if (t->kind == TYPE_INT) { if (is_ptr) fprintf(out, "rae_%s_Int64", type->is_mod ? "Mod" : "View"); else fprintf(out, "int64_t"); return true; }
      if (t->kind == TYPE_FLOAT) { if (is_ptr) fprintf(out, "rae_%s_Float64", type->is_mod ? "Mod" : "View"); else fprintf(out, "double"); return true; }
      if (t->kind == TYPE_BOOL) { if (is_ptr) fprintf(out, "rae_%s_Bool", type->is_mod ? "Mod" : "View"); else fprintf(out, "rae_Bool"); return true; }
      if (t->kind == TYPE_CHAR) { if (is_ptr) fprintf(out, "rae_%s_Char", type->is_mod ? "Mod" : "View"); else fprintf(out, "uint32_t"); return true; }
      if (t->kind == TYPE_STRING) { if (is_ptr) fprintf(out, "rae_%s_String", type->is_mod ? "Mod" : "View"); else fprintf(out, "rae_String"); return true; }
      if (t->kind == TYPE_ANY || t->kind == TYPE_OPT) { fprintf(out, "RaeAny"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_BUFFER) {
          if (type->is_view) fprintf(out, "const ");
          if (t->as.buffer.base->kind == TYPE_ANY || t->as.buffer.base->kind == TYPE_VOID) fprintf(out, "void*");
          else { AstTypeRef tmp = { .resolved_type = t->as.buffer.base }; emit_type_ref_as_c_type(ctx, &tmp, out, false); fprintf(out, "*"); }
          return true;
      }
      if (t->kind == TYPE_STRUCT) {
          if (type->is_view) fprintf(out, "const ");
          // Check if it's a raylib c_struct type — emit bare name
          if (is_raylib_builtin_type(t->name)) {
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
  if (type->is_opt) { fprintf(out, "RaeAny"); if (is_ptr) fprintf(out, "*"); return true; }
  Str base = type->parts->text; bool is_mod = type->is_mod;
  if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int")) { if (is_ptr) fprintf(out, "rae_%s_Int64", is_mod ? "Mod" : "View"); else fprintf(out, "int64_t"); return true; }
  if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) { if (is_ptr) fprintf(out, "rae_%s_Float64", is_mod ? "Mod" : "View"); else fprintf(out, "double"); return true; }
  if (str_eq_cstr(base, "Bool")) { if (is_ptr) fprintf(out, "rae_%s_Bool", is_mod ? "Mod" : "View"); else fprintf(out, "rae_Bool"); return true; }
  if (str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) { if (is_ptr) fprintf(out, "rae_%s_Char%s", is_mod ? "Mod" : "View", str_eq_cstr(base, "Char32") ? "32" : ""); else fprintf(out, "uint32_t"); return true; }
  if (str_eq_cstr(base, "String")) { if (is_ptr) fprintf(out, "rae_%s_String", is_mod ? "Mod" : "View"); else fprintf(out, "rae_String"); return true; }
  if (str_eq_cstr(base, "Any")) { if (is_ptr) fprintf(out, "%sRaeAny*", type->is_view ? "const " : ""); else fprintf(out, "RaeAny"); return true; }
  if (str_eq_cstr(base, "Buffer") && type->generic_args) {
        if (type->is_view) fprintf(out, "const ");
        Str arg_base = get_base_type_name(type->generic_args); if (str_eq_cstr(arg_base, "Any") || arg_base.len == 0) { fprintf(out, "void*"); return true; }
        emit_type_ref_as_c_type(ctx, type->generic_args, out, false); fprintf(out, "*"); return true; 
  }
  if (ctx && ctx->generic_params && ctx->generic_args) {
      const AstIdentifierPart* gp = ctx->generic_params; const AstTypeRef* arg = ctx->generic_args;
      while (gp && arg) { if (str_eq(gp->text, base)) { emit_type_ref_as_c_type(ctx, arg, out, false); return true; } gp = gp->next; arg = arg->next; }
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
    AstTypeRef* tr = func->returns->type; if (tr->is_opt) return "RaeAny";
    bool is_view = tr->is_view, is_mod = tr->is_mod, is_ptr = is_view || is_mod;
    Str base = get_base_type_name(tr);
    // Check if return type is an enum — emit as int64_t
    if (ctx && ctx->module) {
        const AstDecl* ed = find_enum_decl(ctx, ctx->module, base);
        if (ed) return is_ptr ? "int64_t*" : "int64_t";
    }
    if (is_primitive_type(base)) {
        if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int")) return is_ptr ? (is_mod ? "rae_Mod_Int64" : "rae_View_Int64") : "int64_t";
        if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) return is_ptr ? (is_mod ? "rae_Mod_Float64" : "rae_View_Float64") : "double";
        if (str_eq_cstr(base, "Bool")) return is_ptr ? (is_mod ? "rae_Mod_Bool" : "rae_View_Bool") : "rae_Bool";
        if (str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) return is_ptr ? (is_mod ? "rae_Mod_Char32" : "rae_View_Char32") : "uint32_t";
        if (str_eq_cstr(base, "String")) return is_ptr ? (is_mod ? "rae_Mod_String" : "rae_View_String") : "rae_String";
    }
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
    if (type->is_opt) { fprintf(out, "rae_any_none()"); return true; }
    Str base = get_base_type_name(type);
    if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int32") || str_eq_cstr(base, "UInt64") || str_eq_cstr(base, "UInt32") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, "0");
    else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) fprintf(out, "0.0");
    else if (str_eq_cstr(base, "Bool")) fprintf(out, "false");
    else if (str_eq_cstr(base, "String")) fprintf(out, "(rae_String){0}");
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

const AstTypeRef* infer_expr_type_ref(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return NULL;
    // Cache primitive literal type-refs in static storage so callers can hold a
    // pointer past the function return.
    static AstIdentifierPart kInt_part = { .text = { .data = (uint8_t*)"Int", .len = 3 } };
    static AstTypeRef kInt_tr = { .parts = &kInt_part };
    static AstIdentifierPart kFloat_part = { .text = { .data = (uint8_t*)"Float", .len = 5 } };
    static AstTypeRef kFloat_tr = { .parts = &kFloat_part };
    static AstIdentifierPart kBool_part = { .text = { .data = (uint8_t*)"Bool", .len = 4 } };
    static AstTypeRef kBool_tr = { .parts = &kBool_part };
    static AstIdentifierPart kString_part = { .text = { .data = (uint8_t*)"String", .len = 6 } };
    static AstTypeRef kString_tr = { .parts = &kString_part };
    switch (expr->kind) {
        case AST_EXPR_INTEGER: return &kInt_tr;
        case AST_EXPR_FLOAT: return &kFloat_tr;
        case AST_EXPR_BOOL: return &kBool_tr;
        case AST_EXPR_STRING: return &kString_tr;
        case AST_EXPR_IDENT: return get_local_type_ref(ctx, expr->as.ident);
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
        case AST_EXPR_CALL:
        case AST_EXPR_METHOD_CALL: if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) return expr->decl_link->as.func_decl.returns ? expr->decl_link->as.func_decl.returns->type : NULL; break;
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
      bool elem_is_string = str_eq_cstr(ebase, "String");
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
        const char* elem_mangled = rae_mangle_type_specialized(ctx, NULL, NULL, elem);
        bool elem_is_container = str_eq_cstr(ebase, "List") || str_eq_cstr(ebase, "StringMap") || str_eq_cstr(ebase, "IntMap");
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
          if (elem_is_string) {
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
            if (elem_is_string) {
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
  if (f->body) { for (AstStmt* s = f->body->first; s; s = s->next) emit_stmt(&tctx, s, out); }
  emit_implicit_drops_for_body(&tctx, out, first_let_idx);
  emit_implicit_drops_for_own_params(&tctx, out, first_let_idx);
  fprintf(out, "  rae_string_pool_flush(__rae_spm_func);\n");
  fprintf(out, "}\n\n"); return true;
}

bool c_backend_emit_module(CompilerContext* ctx, const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {
  if (!module) return false;
  g_emitted_spec_func_count = 0; // Reset dedup for this compilation
  ctx->all_decl_count = 0; collect_decls_from_module(ctx, module); ctx->current_module = (AstModule*)module;

  // Discover generic specializations by walking all function bodies
  collect_type_refs_module(ctx);

  FILE* out = fopen(out_path, "w"); if (!out) return false;
  fprintf(out, "#include \"rae_runtime.h\"\n\n");
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

  // Generate toJson/fromJson for non-generic user struct types
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
      if (has_property(d->as.type_decl.properties, "c_struct")) continue;
      const AstTypeDecl* td = &d->as.type_decl;
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
          if (str_eq_cstr(base, "String")) {
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
          if (str_eq_cstr(base, "String")) {
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
      const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = td->name}});
      fprintf(out, "RAE_UNUSED static rae_String rae_to_str_%s_(const %s* this);\n", mangled, mangled);
  }
  fprintf(out, "\n");
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind != AST_DECL_TYPE || d->as.type_decl.generic_params) continue;
      if (has_property(d->as.type_decl.properties, "c_struct")) continue;
      const AstTypeDecl* td = &d->as.type_decl;
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
          if (is_user_struct && !is_opt_field && !has_generic_args) {
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
  //   * skip leaf containers (List/StringMap/IntMap/Buffer/Box/Opt)
  //     — they have their own per-T drop overload mechanism that
  //     Pass C's field dispatch wires up.
  //   * skip c_struct
  //   * require the template to be a user generic struct
  //   * require the SUBSTITUTED fields to cascade (avoids emitting
  //     a helper for `Wrapper(Int)` whose only field is a primitive).
  //   * dedup by mangled name to handle duplicate AstTypeRef
  //     entries surfacing the same spec.
  for (size_t gi = 0;
       gi < ctx->generic_type_count && drop_entry_count < 512;
       gi++) {
    const AstTypeRef* gt = ctx->generic_types[gi];
    if (!gt || gt->is_view || gt->is_mod) continue;
    if (!gt->generic_args) continue;
    Str gb = get_base_type_name(gt);
    if (str_eq_cstr(gb, "List")
        || str_eq_cstr(gb, "StringMap")
        || str_eq_cstr(gb, "IntMap")
        || str_eq_cstr(gb, "Buffer")
        || str_eq_cstr(gb, "Box")
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
    if (!gt || gt->is_view || gt->is_mod) continue;
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
    const char* elem_mangled = rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)elem);
    bool elem_is_string = str_eq_cstr(ebase, "String");
    bool elem_is_list = str_eq_cstr(ebase, "List");
    bool elem_is_smap = str_eq_cstr(ebase, "StringMap");
    bool elem_is_imap = str_eq_cstr(ebase, "IntMap");
    bool elem_is_container = elem_is_list || elem_is_smap || elem_is_imap;
    bool elem_needs_deep = elem_is_string || elem_is_container ||
        type_needs_deep_copy(ctx, module, (AstTypeRef*)elem, 0);

    fprintf(out, "RAE_UNUSED static void rae_deep_copy_%s(%s* dst, const %s* src) {\n",
            e->mangled, e->mangled, e->mangled);

    if (e->kind == 0) {
      // List(E): allocate buffer, copy elements.
      fprintf(out, "  dst->length = src->length;\n");
      fprintf(out, "  dst->cap = src->cap;\n");
      fprintf(out, "  if (src->cap > 0) {\n");
      fprintf(out, "    dst->data = (%s*)rae_ext_rae_buf_alloc(src->cap, sizeof(%s));\n",
              elem_mangled, elem_mangled);
      if (!elem_needs_deep) {
        // POD path — bulk copy.
        fprintf(out, "    if (src->length > 0) memcpy(dst->data, src->data, (size_t)src->length * sizeof(%s));\n",
                elem_mangled);
      } else {
        fprintf(out, "    for (int64_t __i = 0; __i < src->length; __i++) {\n");
        if (elem_is_string) {
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
      if (elem_is_string) {
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
          if (d->as.let_decl.value) emit_expr(&gctx, d->as.let_decl.value, out, PREC_LOWEST, false, false);
          else emit_auto_init(&gctx, d->as.let_decl.type, out);
          fprintf(out, ";\n");
      }
      fprintf(out, "\n");
  }

  // Forward declarations for user extern functions (not in runtime header)
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && d->as.func_decl.is_extern && !d->as.func_decl.generic_params) {
          const char* mangled = rae_mangle_function(ctx, &d->as.func_decl);
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
