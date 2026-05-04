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
        fprintf(out, "* data;\n  int64_t length;\n  int64_t capacity;\n};\n\n");
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
    if (str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float64") ||
        str_eq_cstr(base, "Bool") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32") || str_eq_cstr(base, "String") ||
        tr->is_id || tr->is_key) return true;
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
  // Identity types: always emit underlying primitive regardless of resolved_type
  if (type->is_id) { bool is_ptr = (type->is_view || type->is_mod) && !skip_ptr; fprintf(out, "int64_t"); if (is_ptr) fprintf(out, "*"); return true; }
  if (type->is_key) { bool is_ptr = (type->is_view || type->is_mod) && !skip_ptr; fprintf(out, "rae_String"); if (is_ptr) fprintf(out, "*"); return true; }
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
  if (type->is_id) { fprintf(out, "int64_t"); if (is_ptr) fprintf(out, "*"); return true; }
  if (type->is_key) { fprintf(out, "rae_String"); if (is_ptr) fprintf(out, "*"); return true; }
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
    if (tr->is_id) return is_ptr ? "int64_t*" : "int64_t";
    if (tr->is_key) return is_ptr ? "rae_String*" : "rae_String";
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
    else { const AstDecl* d = find_type_decl(ctx, ctx->module, base); if (d && d->kind == AST_DECL_TYPE) emit_struct_auto_init(ctx, d, type, out); else fprintf(out, "{0}"); }
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
  CFuncContext tctx = {.compiler_ctx = ctx, .module = m, .func_decl = f, .uses_raylib = ray, .registry = r};
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
          tctx.local_count++; 
      } 
  }
  
  if (f->body) { for (AstStmt* s = f->body->first; s; s = s->next) emit_stmt(&tctx, s, out); }

  // Emit any remaining defers at function end
  if (tctx.defer_stack.count > 0) emit_defers(&tctx, 0, out);

  if (is_main) fprintf(out, "  return 0;\n}\n\n");
  else fprintf(out, "}\n\n"); 
  return true;
}

// Track emitted specialized functions to avoid redefinitions
const char* g_emitted_spec_funcs[4096];
static size_t g_emitted_spec_func_count = 0;

bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray) {
  const AstIdentifierPart* gp_src = f->generic_params; if (!gp_src && f->generic_template) gp_src = f->generic_template->as.func_decl.generic_params;
  CFuncContext tctx = {.compiler_ctx = ctx, .module = m, .func_decl = f, .uses_raylib = ray, .registry = r, .generic_params = gp_src, .generic_args = args};
  const char* rt = c_return_type(&tctx, f); const char* mangled = rae_mangle_specialized_function(ctx, f, args);
  // Dedup check: skip if already emitted
  for (size_t i = 0; i < g_emitted_spec_func_count; i++) {
      if (strcmp(g_emitted_spec_funcs[i], mangled) == 0) return true;
  }
  if (g_emitted_spec_func_count < 4096) g_emitted_spec_funcs[g_emitted_spec_func_count++] = mangled;
  fprintf(out, "RAE_UNUSED static %s %s(", rt, mangled); emit_param_list(&tctx, f->params, out, false); fprintf(out, ") {\n");
  for (const AstParam* p = f->params; p; p = p->next) { if (tctx.local_count < 256) { tctx.locals[tctx.local_count] = p->name; tctx.local_type_refs[tctx.local_count] = p->type; tctx.local_types[tctx.local_count] = str_from_cstr(rae_mangle_type_specialized(ctx, gp_src, args, p->type)); tctx.local_count++; } }
  if (f->body) { for (AstStmt* s = f->body->first; s; s = s->next) emit_stmt(&tctx, s, out); }
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

  // Prototypes for specialized functions
  for (size_t i = 0; i < ctx->specialized_func_count; i++) {
      const AstFuncDecl* f = ctx->specialized_funcs[i].decl; const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
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
  for (size_t i = 0; i < ctx->specialized_func_count; i++) {
      const AstFuncDecl* pf = ctx->specialized_funcs[i].decl;
      const AstTypeRef* pa = ctx->specialized_funcs[i].concrete_args;
      const char* pm = rae_mangle_specialized_function(ctx, pf, pa);
      // Check if already prototyped (avoid duplicates)
      bool already_proto = false;
      for (size_t j = 0; j < i; j++) {
          const char* prev = rae_mangle_specialized_function(ctx, ctx->specialized_funcs[j].decl, ctx->specialized_funcs[j].concrete_args);
          if (strcmp(pm, prev) == 0) { already_proto = true; break; }
      }
      if (already_proto) continue;
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
              const char* mangled = rae_mangle_specialized_function(ctx, f, args);
              const AstIdentifierPart* gp = f->generic_params;
              if (!gp && f->generic_template && f->generic_template->kind == AST_DECL_FUNC)
                  gp = f->generic_template->as.func_decl.generic_params;
              CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .generic_params = gp, .generic_args = args};
              fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&tctx, f), mangled);
              emit_param_list(&tctx, f->params, out, false);
              fprintf(out, ");\n");
              prototyped_count++;
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
          // Prototype
          const AstFuncDecl* f = ctx->specialized_funcs[emitted_idx2].decl;
          const AstTypeRef* args = ctx->specialized_funcs[emitted_idx2].concrete_args;
          const char* mangled = rae_mangle_specialized_function(ctx, f, args);
          const AstIdentifierPart* gp = f->generic_params;
          if (!gp && f->generic_template && f->generic_template->kind == AST_DECL_FUNC)
              gp = f->generic_template->as.func_decl.generic_params;
          CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .generic_params = gp, .generic_args = args};
          fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&tctx, f), mangled);
          emit_param_list(&tctx, f->params, out, false);
          fprintf(out, ");\n");
          // Body
          emit_specialized_function(ctx, module, f, args, out, registry, false);
          emitted_idx2++;
      }
  }

  fclose(out); return true;
}
