#include "c_backend.h"
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

// Precedence levels for C expressions
// Based on C operator precedence
enum {
  PREC_LOWEST = 0,
  PREC_COMMA,
  PREC_ASSIGN,
  PREC_TERNARY,
  PREC_LOGICAL_OR,
  PREC_LOGICAL_AND,
  PREC_BITWISE_OR,
  PREC_BITWISE_XOR,
  PREC_BITWISE_AND,
  PREC_EQUALITY,    // == !=
  PREC_RELATIONAL,  // < > <= >=
  PREC_SHIFT,
  PREC_ADD,         // + -
  PREC_MUL,         // * / %
  PREC_UNARY,       // ! - + * &
  PREC_CALL,        // () [] . ->
  PREC_ATOMIC
};

typedef struct {
  const struct AstBlock* block;
  int scope_depth;
} CDeferEntry;

typedef struct {
  CDeferEntry entries[64];
  int count;
} CDeferStack;

typedef struct {
    const char** items;
    size_t count;
    size_t capacity;
} EmittedTypeList;

static bool emitted_list_contains(EmittedTypeList* list, const char* name) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], name) == 0) return true;
    }
    return false;
}

static void emitted_list_add(EmittedTypeList* list, const char* name) {
    if (list->count < list->capacity) {
        list->items[list->count++] = name;
    }
}

typedef struct {
  CompilerContext* compiler_ctx;
  const AstModule* module;
  const AstFuncDecl* func_decl;
  const AstParam* params;
  const AstIdentifierPart* generic_params;
  const AstTypeRef* generic_args;
  const char* return_type_name;
  Str locals[256];
  Str local_types[256];
  const AstTypeRef* local_type_refs[256];
  bool local_is_ptr[256];
  bool local_is_mod[256];
  size_t local_count;
  bool returns_value;
  size_t temp_counter;
  AstTypeRef expected_type;
  bool has_expected_type;
  const struct VmRegistry* registry;
  bool uses_raylib;
  bool is_main;
  int scope_depth;
  CDeferStack defer_stack;
} CFuncContext;

// Forward declarations
static const AstDecl* find_type_decl(CFuncContext* ctx, const AstModule* module, Str name);
static bool has_property(const AstProperty* props, const char* name);
static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out, bool skip_ptr);
static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue, bool suppress_deref);
static bool emit_function(CompilerContext* compiler_ctx, const AstModule* module, const AstFuncDecl* func, FILE* out, const struct VmRegistry* registry, bool uses_raylib);
static bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray);
static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out);
static void pop_defers(CFuncContext* ctx, int depth);
static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_string_literal(FILE* out, Str literal);
static bool is_mod_type(CFuncContext* ctx, Str name);
static bool is_generic_param(const AstIdentifierPart* params, Str name);
static Str get_local_type_name(CFuncContext* ctx, Str name);
static const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name);

static bool emit_type_recursive(CompilerContext* ctx, const AstModule* m, const AstTypeRef* type, FILE* out, EmittedTypeList* emitted, EmittedTypeList* visiting, bool ray) {
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
    
    const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, type);
    if (emitted_list_contains(emitted, mangled)) return true;
    if (emitted_list_contains(visiting, mangled)) return true;
    
    emitted_list_add(visiting, mangled);
    
    // Find the declaration
    if (str_eq_cstr(base, "List") || str_eq_cstr(base, "Buffer")) {
        // Built-in List/Buffer
        fprintf(out, "typedef struct %s %s;\n", mangled, mangled);
        fprintf(out, "struct %s {\n", mangled);
        CFuncContext tctx = {0}; tctx.compiler_ctx = ctx; tctx.module = m; tctx.uses_raylib = ray;
        fprintf(out, "  ");
        emit_type_ref_as_c_type(&tctx, type->generic_args, out, false);
        fprintf(out, "* data;\n  int64_t length;\n  int64_t capacity;\n};\n\n");
    } else {
        const AstDecl* d = find_type_decl(NULL, m, base);
        if (d && d->kind == AST_DECL_TYPE) {
            const AstTypeDecl* td = &d->as.type_decl;
            const AstIdentifierPart* params = td->generic_params;
            const AstTypeRef* args = type->generic_args;
            if (!params && d->as.type_decl.generic_template) params = d->as.type_decl.generic_template->as.type_decl.generic_params;
            
            // Dependencies
            for (const AstTypeField* f = td->fields; f; f = f->next) {
                if (!f->type || f->type->is_view || f->type->is_mod) continue;
                AstTypeRef* sub = substitute_type_ref(ctx, params, args, f->type);
                emit_type_recursive(ctx, m, sub, out, emitted, visiting, ray);
            }
            
            if (!has_property(td->properties, "c_struct")) {
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

static bool emit_auto_init(CFuncContext* ctx, const AstTypeRef* type, FILE* out);
static bool emit_struct_auto_init(CFuncContext* ctx, const AstDecl* decl, const AstTypeRef* tr, FILE* out);
static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count, bool is_method, const AstExpr* call_expr);
static bool is_primitive_ref(CFuncContext* ctx, const AstTypeRef* tr) {
    if (!tr || !(tr->is_view || tr->is_mod)) return false;
    Str base = get_base_type_name(tr);
    if (str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float64") || 
        str_eq_cstr(base, "Bool") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32") || str_eq_cstr(base, "String") ||
        tr->is_id || tr->is_key) return true;
    return is_primitive_type(base) || (ctx->uses_raylib && is_raylib_builtin_type(base));
}


static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    fprintf(out, "  if (");
    emit_expr(ctx, stmt->as.if_stmt.condition, out, PREC_LOWEST, false, false);
    fprintf(out, ") {\n");
    if (stmt->as.if_stmt.then_block) {
        for (const AstStmt* s = stmt->as.if_stmt.then_block->first; s; s = s->next) emit_stmt(ctx, s, out);
    }
    fprintf(out, "  }");
    if (stmt->as.if_stmt.else_block) {
        fprintf(out, " else {\n");
        for (const AstStmt* s = stmt->as.if_stmt.else_block->first; s; s = s->next) emit_stmt(ctx, s, out);
        fprintf(out, "  }\n");
    } else {
        fprintf(out, "\n");
    }
    return true;
}

static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    fprintf(out, "  for (");
    if (stmt->as.loop_stmt.init) {
        // Init stmt usually doesn't have a newline/indent in for loop
        if (stmt->as.loop_stmt.init->kind == AST_STMT_LET) {
            const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.loop_stmt.init->as.let_stmt.type);
            fprintf(out, "%s %.*s = ", tn, (int)stmt->as.loop_stmt.init->as.let_stmt.name.len, stmt->as.loop_stmt.init->as.let_stmt.name.data);
            emit_expr(ctx, stmt->as.loop_stmt.init->as.let_stmt.value, out, PREC_LOWEST, false, false);
        } else {
            emit_expr(ctx, stmt->as.loop_stmt.init->as.expr_stmt, out, PREC_LOWEST, false, false);
        }
    }
    fprintf(out, "; ");
    if (stmt->as.loop_stmt.condition) emit_expr(ctx, stmt->as.loop_stmt.condition, out, PREC_LOWEST, false, false);
    fprintf(out, "; ");
    if (stmt->as.loop_stmt.increment) emit_expr(ctx, stmt->as.loop_stmt.increment, out, PREC_LOWEST, false, false);
    fprintf(out, ") {\n");
    if (stmt->as.loop_stmt.body) {
        for (const AstStmt* s = stmt->as.loop_stmt.body->first; s; s = s->next) emit_stmt(ctx, s, out);
    }
    fprintf(out, "  }\n");
    return true;
}

static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    (void)ctx; (void)stmt; (void)out;
    fprintf(stderr, "warning: match stmt not yet implemented in unified C backend\n");
    return true;
}

static bool func_has_return_value(const AstFuncDecl* func);

static const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func);
static const AstTypeRef* infer_expr_type_ref(CFuncContext* ctx, const AstExpr* expr);
static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr);

static bool has_property(const AstProperty* props, const char* name) {
  while (props) { if (str_eq_cstr(props->name, name)) return true; props = props->next; }
  return false;
}

static const AstModule* g_find_module_stack[64];
static size_t g_find_module_stack_count = 0;

static bool types_match(Str a, Str b) {
  if (str_eq(a, b)) return true;
  if (str_eq_cstr(a, "String") && (str_eq_cstr(b, "const char*") || str_eq_cstr(b, "rae_String"))) return true;
  if (str_eq_cstr(b, "String") && (str_eq_cstr(a, "const char*") || str_eq_cstr(a, "rae_String"))) return true;
  if (str_eq_cstr(a, "String") && str_eq_cstr(b, "const_char_p")) return true;
  if (str_eq_cstr(b, "String") && str_eq_cstr(a, "const_char_p")) return true;
  return false;
}

static const AstDecl* find_type_decl(CFuncContext* ctx, const AstModule* module, Str name) {
  if (ctx && ctx->compiler_ctx) {
      for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
          const AstDecl* decl = ctx->compiler_ctx->all_decls[i];
          if (decl->kind == AST_DECL_TYPE && types_match(decl->as.type_decl.name, name)) return decl;
      }
  }
  if (!module) return NULL;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) { if (decl->kind == AST_DECL_TYPE && types_match(decl->as.type_decl.name, name)) return decl; }
  for (size_t i = 0; i < g_find_module_stack_count; i++) if (g_find_module_stack[i] == module) return NULL;
  if (g_find_module_stack_count >= 64) return NULL;
  g_find_module_stack[g_find_module_stack_count++] = module;
  const AstDecl* found = NULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) { found = find_type_decl(ctx, imp->module, name); if (found) break; }
  g_find_module_stack_count--; return found;
}

static const AstDecl* find_enum_decl(CFuncContext* ctx, const AstModule* module, Str name) {
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

static void register_decl(CompilerContext* ctx, const AstDecl* decl) {
    if (!decl) return;
    for (size_t i = 0; i < ctx->all_decl_count; i++) { if (ctx->all_decls[i] == decl) return; }
    if (ctx->all_decl_count < ctx->all_decl_cap) ctx->all_decls[ctx->all_decl_count++] = decl;
}

static void collect_decls_from_module(CompilerContext* ctx, const AstModule* module) {
    if (!module) return;
    if (module->decls) { for (size_t i = 0; i < ctx->all_decl_count; i++) { if (ctx->all_decls[i] == module->decls) return; } }
    for (const AstDecl* decl = module->decls; decl; decl = decl->next) register_decl(ctx, decl);
    for (const AstImport* imp = module->imports; imp; imp = imp->next) collect_decls_from_module(ctx, imp->module);
}

static int g_type_equal_depth = 0;
static bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b) {
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

static bool is_concrete_type(const AstTypeRef* type) {
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

static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count, bool is_method, const AstExpr* call_expr) {
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

static void discover_specializations_expr(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_CALL: {
            const AstExpr* callee = expr->as.call.callee;
            if (callee->kind == AST_EXPR_IDENT) {
                uint16_t param_count = 0; for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                const AstFuncDecl* d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, false, expr);
                if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, true, expr);
                if ((str_eq_cstr(callee->as.ident, "sizeof") || str_eq_cstr(callee->as.ident, "__buf_alloc") || str_eq_cstr(callee->as.ident, "__buf_free") || str_eq_cstr(callee->as.ident, "__buf_copy")) && expr->as.call.generic_args) {
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args); register_generic_type(ctx->compiler_ctx, sub);
                }
                if (d && d->generic_params) {
                    AstTypeRef* inferred_args = NULL;
                    if (expr->as.call.generic_args) {
                        AstTypeRef* concrete_args = NULL; AstTypeRef* last_arg = NULL;
                        for (const AstTypeRef* arg = expr->as.call.generic_args; arg; arg = arg->next) {
                            AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg);
                            if (!concrete_args) concrete_args = sub; else last_arg->next = sub; last_arg = sub;
                        }
                        inferred_args = concrete_args;
                    } else if (d->params && str_eq_cstr(d->params->name, "this")) {
                        const AstCallArg* first_arg = expr->as.call.args;
                        if (first_arg) {
                            const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, first_arg->value);
                            AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->params->type, receiver_type);
                            if (inferred) inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                        }
                    }
                    if (inferred_args) register_function_specialization(ctx->compiler_ctx, d, inferred_args);
                }
            }
            for (const AstCallArg* a = expr->as.call.args; a; a = a->next) discover_specializations_expr(ctx, a->value);
            break;
        }
        case AST_EXPR_METHOD_CALL: {
            uint16_t param_count = 1; for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) param_count++;
            Str obj_type = infer_expr_type(ctx, expr->as.method_call.object);
            const AstFuncDecl* d = find_function_overload(ctx->module, ctx, expr->as.method_call.method_name, &obj_type, param_count, true, expr);
            if (d && d->generic_params) {
                const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, expr->as.method_call.object);
                AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->params->type, receiver_type);
                if (inferred) { AstTypeRef* concrete = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred); register_function_specialization(ctx->compiler_ctx, d, concrete); }
            }
            discover_specializations_expr(ctx, expr->as.method_call.object); for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) discover_specializations_expr(ctx, a->value);
            break;
        }
        case AST_EXPR_BINARY: discover_specializations_expr(ctx, expr->as.binary.lhs); discover_specializations_expr(ctx, expr->as.binary.rhs); break;
        case AST_EXPR_UNARY: discover_specializations_expr(ctx, expr->as.unary.operand); break;
        case AST_EXPR_MEMBER: discover_specializations_expr(ctx, expr->as.member.object); break;
        case AST_EXPR_INDEX: discover_specializations_expr(ctx, expr->as.index.target); discover_specializations_expr(ctx, expr->as.index.index); break;
        case AST_EXPR_OBJECT: for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) discover_specializations_expr(ctx, f->value); break;
        case AST_EXPR_COLLECTION_LITERAL: for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) discover_specializations_expr(ctx, e->value); break;
        default: break;
    }
}

static void discover_specializations_stmt(CFuncContext* ctx, const AstStmt* stmt) {
    for (const AstStmt* s = stmt; s; s = s->next) {
        switch (s->kind) {
            case AST_STMT_LET: 
                if (s->as.let_stmt.value) {
                    const AstTypeRef* type = s->as.let_stmt.type ? s->as.let_stmt.type : infer_expr_type_ref(ctx, s->as.let_stmt.value);
                    if (type) { ctx->expected_type = *type; ctx->has_expected_type = true; }
                    discover_specializations_expr(ctx, s->as.let_stmt.value); ctx->has_expected_type = false;
                    if (ctx->local_count < 256) {
                        ctx->locals[ctx->local_count] = s->as.let_stmt.name; ctx->local_type_refs[ctx->local_count] = type;
                        const char* mn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
                        ctx->local_types[ctx->local_count] = str_from_cstr(mn); ctx->local_count++;
                    }
                }
                break;
            case AST_STMT_ASSIGN: 
                discover_specializations_expr(ctx, s->as.assign_stmt.target);
                {
                    const AstTypeRef* tr = infer_expr_type_ref(ctx, s->as.assign_stmt.target);
                    if (tr) { ctx->expected_type = *tr; ctx->has_expected_type = true; }
                    discover_specializations_expr(ctx, s->as.assign_stmt.value); ctx->has_expected_type = false;
                }
                break;
            case AST_STMT_EXPR: discover_specializations_expr(ctx, s->as.expr_stmt); break;
            case AST_STMT_IF: discover_specializations_expr(ctx, s->as.if_stmt.condition); if (s->as.if_stmt.then_block) discover_specializations_stmt(ctx, s->as.if_stmt.then_block->first); if (s->as.if_stmt.else_block) discover_specializations_stmt(ctx, s->as.if_stmt.else_block->first); break;
            case AST_STMT_LOOP: if (s->as.loop_stmt.init) discover_specializations_stmt(ctx, s->as.loop_stmt.init); if (s->as.loop_stmt.condition) discover_specializations_expr(ctx, s->as.loop_stmt.condition); if (s->as.loop_stmt.increment) discover_specializations_expr(ctx, s->as.loop_stmt.increment); if (s->as.loop_stmt.body) discover_specializations_stmt(ctx, s->as.loop_stmt.body->first); break;
            default: break;
        }
    }
}

static void discover_specializations_module(CompilerContext* ctx, const AstModule* module) {
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        const AstDecl* d = ctx->all_decls[i];
        if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params) {
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = &d->as.func_decl};
            if (d->as.func_decl.body) discover_specializations_stmt(&fctx, d->as.func_decl.body->first);
        }
    }
    size_t discovered = 0;
    while (discovered < ctx->specialized_func_count) {
        size_t limit = ctx->specialized_func_count;
        for (size_t i = discovered; i < limit; i++) {
            const AstFuncDecl* f = ctx->specialized_funcs[i].decl; const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
            if (!f) continue;
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = f, .generic_params = f->generic_params, .generic_args = args};
            if (f->body) discover_specializations_stmt(&fctx, f->body->first);
        }
        discovered = limit;
    }
}

static void collect_type_refs_module(CompilerContext* ctx) {
    size_t last_generic_count = 0; size_t last_func_count = 0;
    do {
        last_generic_count = ctx->generic_type_count; last_func_count = ctx->specialized_func_count;
        discover_specializations_module(ctx, ctx->current_module);
    } while (ctx->generic_type_count > last_generic_count || ctx->specialized_func_count > last_func_count);
}

static int binary_op_precedence(AstBinaryOp op) {
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



static bool emit_string_literal(FILE* out, Str literal) {
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

static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out, bool skip_ptr) {
  if (!type) { fprintf(out, "int64_t"); return true; }
  if (type->resolved_type) {
      TypeInfo* t = type->resolved_type; bool is_ptr = (type->is_view || type->is_mod) && !skip_ptr;
      if (t->kind == TYPE_INT) { fprintf(out, "int64_t"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_FLOAT) { fprintf(out, "double"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_BOOL) { fprintf(out, "rae_Bool"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_CHAR) { fprintf(out, "uint32_t"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_STRING) { fprintf(out, "rae_String"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_ANY || t->kind == TYPE_OPT) { fprintf(out, "RaeAny"); if (is_ptr) fprintf(out, "*"); return true; }
      if (t->kind == TYPE_BUFFER) {
          if (type->is_view) fprintf(out, "const ");
          if (t->as.buffer.base->kind == TYPE_ANY || t->as.buffer.base->kind == TYPE_VOID) fprintf(out, "void*");
          else { AstTypeRef tmp = { .resolved_type = t->as.buffer.base }; emit_type_ref_as_c_type(ctx, &tmp, out, false); fprintf(out, "*"); }
          return true;
      }
      if (t->kind == TYPE_STRUCT) {
          if (type->is_view) fprintf(out, "const ");
          const char* name = type_mangle_name(ctx->compiler_ctx->ast_arena, t).data;
          fprintf(out, "%s", name);
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
  const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
  if (ctx && ctx->uses_raylib && is_raylib_builtin_type(base)) {
        const AstDecl* td = find_type_decl(NULL, ctx->module, base);
        if (td && td->kind == AST_DECL_TYPE && has_property(td->as.type_decl.properties, "c_struct")) fprintf(out, "%.*s", (int)base.len, base.data);
        else if (!td) fprintf(out, "%.*s", (int)base.len, base.data); else fprintf(out, "rae_%.*s", (int)base.len, base.data);
  } else fprintf(out, "%s", mangled);
  if (is_ptr) fprintf(out, "*");
  return true;
}

static bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out, bool is_extern) {
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

static const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func) {
  if (str_eq_cstr(func->name, "rae_ext_rae_buf_alloc") || str_eq_cstr(func->name, "__buf_alloc") || str_eq_cstr(func->name, "rae_ext_rae_buf_resize") || str_eq_cstr(func->name, "__buf_resize") || str_eq_cstr(func->name, "rae_ext_rae_str_to_cstr") || str_eq_cstr(func->name, "toCStr")) return "void*";
  if (str_eq_cstr(func->name, "rae_ext_rae_buf_free") || str_eq_cstr(func->name, "__buf_free") || str_eq_cstr(func->name, "rae_ext_rae_buf_copy") || str_eq_cstr(func->name, "__buf_copy")) return "void";
  if (func->returns && func->returns->type) {
    AstTypeRef* tr = func->returns->type; if (tr->is_opt) return "RaeAny";
    bool is_view = tr->is_view, is_mod = tr->is_mod, is_ptr = is_view || is_mod;
    if (tr->is_id) return is_ptr ? "int64_t*" : "int64_t";
    if (tr->is_key) return is_ptr ? "rae_String*" : "rae_String";
    Str base = get_base_type_name(tr);
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

static bool func_has_return_value(const AstFuncDecl* func) { return func->returns != NULL; }
static Str get_local_type_name(CFuncContext* ctx, Str name) { for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_types[i]; return (Str){0}; }
static const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name) { for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_type_refs[i]; return NULL; }

static bool emit_auto_init(CFuncContext* ctx, const AstTypeRef* type, FILE* out) {
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

static bool emit_struct_auto_init(CFuncContext* ctx, const AstDecl* decl, const AstTypeRef* tr, FILE* out) {
    fprintf(out, "{ ");
    for (const AstTypeField* f = decl->as.type_decl.fields; f; f = f->next) {
        fprintf(out, ".%.*s = ", (int)f->name.len, f->name.data);
        AstTypeRef* field_tr = substitute_type_ref(ctx->compiler_ctx, decl->as.type_decl.generic_params, tr->generic_args, f->type);
        emit_auto_init(ctx, field_tr, out); if (f->next) fprintf(out, ", ");
    }
    fprintf(out, " }"); return true;
}

static bool is_pointer_type(CFuncContext* ctx, Str name) {
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

static bool is_generic_param(const AstIdentifierPart* params, Str name) { const AstIdentifierPart* p = params; while (p) { if (str_eq(p->text, name)) return true; p = p->next; } return false; }

static const AstTypeRef* infer_expr_type_ref(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return NULL;
    switch (expr->kind) {
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
        case AST_EXPR_CALL: if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) return expr->decl_link->as.func_decl.returns ? expr->decl_link->as.func_decl.returns->type : NULL; break;
        default: break;
    }
    return NULL;
}

static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return (Str){0};
    const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
    if (tr) return str_from_cstr(rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr));
    return (Str){0};
}

static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue, bool suppress_deref) {
  if (!expr) return true;
  switch (expr->kind) {
    case AST_EXPR_INTEGER: fprintf(out, "((int64_t)%.*sLL)", (int)expr->as.integer.len, expr->as.integer.data); break;
    case AST_EXPR_FLOAT: fprintf(out, "%.*s", (int)expr->as.floating.len, expr->as.floating.data); break;
    case AST_EXPR_BOOL: fprintf(out, "(bool)%s", expr->as.boolean ? "true" : "false"); break;
    case AST_EXPR_STRING: emit_string_literal(out, expr->as.string_lit); break;
    case AST_EXPR_CHAR: fprintf(out, "(uint32_t)%uU", (uint32_t)expr->as.char_value); break;
    case AST_EXPR_IDENT: {
        const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
        bool is_prim_ref = is_primitive_ref(ctx, tr);
        bool is_ptr = is_pointer_type(ctx, expr->as.ident);
        
        if (is_prim_ref && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s.ptr)", (int)expr->as.ident.len, expr->as.ident.data);
        } else if (is_ptr && !is_lvalue && !suppress_deref) {
            // Check if it's a Buffer or List - they are pointers but shouldn't be dereferenced here 
            // if we are just passing them or accessing members via ->
            Str base = get_base_type_name(tr);
            if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List")) {
                fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
            } else {
                fprintf(out, "(*%.*s)", (int)expr->as.ident.len, expr->as.ident.data);
            }
        } else {
            fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
        }
        break;
    }
    case AST_EXPR_BINARY: {
      int prec = binary_op_precedence(expr->as.binary.op); bool is_bool_op = expr->as.binary.op >= AST_BIN_LT && expr->as.binary.op <= AST_BIN_OR;
      if (is_bool_op) fprintf(out, "(bool)("); if (prec < parent_prec) fprintf(out, "(");
      emit_expr(ctx, expr->as.binary.lhs, out, prec, false, false);
      switch (expr->as.binary.op) {
        case AST_BIN_ADD: fprintf(out, " + "); break; case AST_BIN_SUB: fprintf(out, " - "); break;
        case AST_BIN_MUL: fprintf(out, " * "); break; case AST_BIN_DIV: fprintf(out, " / "); break;
        case AST_BIN_MOD: fprintf(out, " %% "); break; case AST_BIN_LT: fprintf(out, " < "); break;
        case AST_BIN_GT: fprintf(out, " > "); break; case AST_BIN_LE: fprintf(out, " <= "); break;
        case AST_BIN_GE: fprintf(out, " >= "); break; case AST_BIN_IS: fprintf(out, " == "); break;
        case AST_BIN_AND: fprintf(out, " && "); break; case AST_BIN_OR: fprintf(out, " || "); break;
      }
      emit_expr(ctx, expr->as.binary.rhs, out, prec, false, false);
      if (prec < parent_prec) fprintf(out, ")"); if (is_bool_op) fprintf(out, ")"); break;
    }
    case AST_EXPR_UNARY: {
        switch (expr->as.unary.op) {
            case AST_UNARY_NOT: fprintf(out, "((bool)!("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, "))"); break;
            case AST_UNARY_NEG: fprintf(out, "-("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, ")"); break;
            case AST_UNARY_VIEW: case AST_UNARY_MOD: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); break;
            case AST_UNARY_PRE_INC: fprintf(out, "++"); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); break;
            case AST_UNARY_PRE_DEC: fprintf(out, "--"); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); break;
            case AST_UNARY_POST_INC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "++"); break;
            case AST_UNARY_POST_DEC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "--"); break;
            default: break;
        }
        break;
    }
    case AST_EXPR_CALL: emit_call_expr(ctx, expr, out); break;
    case AST_EXPR_METHOD_CALL: {
        AstExpr call = { .kind = AST_EXPR_CALL, .line = expr->line, .column = expr->column, .decl_link = expr->decl_link };
        call.as.call.callee = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstExpr)); call.as.call.callee->kind = AST_EXPR_IDENT; call.as.call.callee->as.ident = expr->as.method_call.method_name;
        AstCallArg* first_arg = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstCallArg)); first_arg->name = str_from_cstr("this"); first_arg->value = expr->as.method_call.object; first_arg->next = expr->as.method_call.args;
        call.as.call.args = first_arg; call.as.call.generic_args = expr->as.method_call.generic_args;
        emit_call_expr(ctx, &call, out); break;
    }
    case AST_EXPR_MEMBER: {
        // Check if this is an enum access (e.g. Color.Green)
        if (expr->as.member.object->kind == AST_EXPR_IDENT) {
            const AstDecl* ed = find_enum_decl(ctx, ctx->module, expr->as.member.object->as.ident);
            if (ed) {
                fprintf(out, "%.*s_%.*s", (int)expr->as.member.object->as.ident.len, expr->as.member.object->as.ident.data,
                    (int)expr->as.member.member.len, expr->as.member.member.data);
                break;
            }
        }
        const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.member.object);
        Str obj_base = get_base_type_name(obj_tr);
        bool use_arrow = (obj_tr && (obj_tr->is_view || obj_tr->is_mod)) ||
                         str_eq_cstr(obj_base, "List") || str_eq_cstr(obj_base, "Buffer");
        emit_expr(ctx, expr->as.member.object, out, PREC_CALL, true, false);
        fprintf(out, "%s%.*s", use_arrow ? "->" : ".", (int)expr->as.member.member.len, expr->as.member.member.data);
        break;
    }
    case AST_EXPR_INDEX: {
        emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false); fprintf(out, "["); emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false, false); fprintf(out, "]"); break;
    }
    case AST_EXPR_BOX: fprintf(out, "rae_any(("); emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false); fprintf(out, "))"); break;
    case AST_EXPR_UNBOX: {
        if (expr->resolved_type) {
            TypeInfo* t = expr->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
            fprintf(out, "("); emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
            if (t->kind == TYPE_INT || t->kind == TYPE_CHAR) fprintf(out, ").as.i"); else if (t->kind == TYPE_FLOAT) fprintf(out, ").as.f"); else if (t->kind == TYPE_BOOL) fprintf(out, ").as.b"); else if (t->kind == TYPE_STRING) fprintf(out, ").as.s"); else fprintf(out, ").as.ptr");
        } else emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
        break;
    }
    case AST_EXPR_OBJECT: {
        bool needs_cast = true;
        if (expr->as.object_literal.type) {
            fprintf(out, "(");
            emit_type_ref_as_c_type(ctx, expr->as.object_literal.type, out, false);
            fprintf(out, ")");
        }
        fprintf(out, "{ ");
        for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
            fprintf(out, ".%.*s = ", (int)f->name.len, f->name.data);
            emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
            if (f->next) fprintf(out, ", ");
        }
        fprintf(out, " }");
        break;
    }
    case AST_EXPR_LIST: {
        for (const AstExprList* item = expr->as.list; item; item = item->next) {
            emit_expr(ctx, item->value, out, PREC_LOWEST, false, false);
            if (item->next) fprintf(out, ", ");
        }
        break;
    }
    case AST_EXPR_INTERP: {
        AstInterpPart* part = expr->as.interp.parts;
        if (!part) { fprintf(out, "(rae_String){(uint8_t*)\"\", 0}"); break; }
        // Count parts to determine nesting
        int count = 0;
        for (AstInterpPart* p = part; p; p = p->next) count++;
        if (count == 1) {
            // Single part - just emit as string
            if (part->value->kind == AST_EXPR_STRING) {
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
            } else {
                fprintf(out, "rae_ext_rae_str((");
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
                fprintf(out, "))");
            }
        } else {
            // Multiple parts - nest rae_ext_rae_str_concat calls
            // Build: concat(concat(concat(part1, str(part2)), str(part3)), str(part4))
            int opens = 0;
            bool first = true;
            for (AstInterpPart* p = part; p; p = p->next) {
                if (first) { first = false; continue; }
                fprintf(out, "rae_ext_rae_str_concat(");
                opens++;
            }
            // Emit first part
            if (part->value->kind == AST_EXPR_STRING) {
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
            } else {
                fprintf(out, "rae_ext_rae_str((");
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
                fprintf(out, "))");
            }
            // Emit remaining parts
            for (AstInterpPart* p = part->next; p; p = p->next) {
                fprintf(out, ", ");
                if (p->value->kind == AST_EXPR_STRING) {
                    emit_expr(ctx, p->value, out, PREC_LOWEST, false, false);
                } else {
                    fprintf(out, "rae_ext_rae_str((");
                    emit_expr(ctx, p->value, out, PREC_LOWEST, false, false);
                    fprintf(out, "))");
                }
                fprintf(out, ")");
            }
        }
        break;
    }
    default: break;
  }
  return true;
}

static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
    if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) {
        const AstFuncDecl* fd = &expr->decl_link->as.func_decl;
        Str name = fd->name;
        if (str_eq_cstr(name, "sizeof")) {
            fprintf(out, "sizeof(");
            if (expr->as.call.generic_args) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
                emit_type_ref_as_c_type(ctx, sub, out, false);
            } else {
                fprintf(out, "void");
            }
            fprintf(out, ")");
            return true;
        }
        fprintf(out, "%s(", rae_mangle_function(ctx->compiler_ctx, fd));
        const AstCallArg* a = expr->as.call.args;
        const AstParam* p = fd->params;
        while (a) {
            bool needs_addr = false;
            if (p && (p->type->is_view || p->type->is_mod)) {
                const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                if (arg_tr && !arg_tr->is_view && !arg_tr->is_mod) {
                    Str base = get_base_type_name(p->type);
                    if (!is_primitive_type(base)) needs_addr = true;
                }
            }
            
            // Auto-box if param is Any and arg is not already boxed
            bool needs_box = false;
            if (p && p->type && a->value->kind != AST_EXPR_BOX) {
                Str param_base = get_base_type_name(p->type);
                if (str_eq_cstr(param_base, "Any")) needs_box = true;
            }

            if (needs_addr) fprintf(out, "&");
            if (needs_box) fprintf(out, "rae_any((");
            emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
            if (needs_box) fprintf(out, "))");

            if (a->next) fprintf(out, ", ");
            a = a->next; if (p) p = p->next;
        }
        fprintf(out, ")"); return true;
    }
    // Fallback: look up function by name when decl_link is missing
    if (expr->as.call.callee && expr->as.call.callee->kind == AST_EXPR_IDENT) {
        Str callee_name = expr->as.call.callee->as.ident;
        // Search for the function declaration by name
        const AstFuncDecl* found_fd = NULL;
        for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
            const AstDecl* d = ctx->compiler_ctx->all_decls[i];
            if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, callee_name)) {
                found_fd = &d->as.func_decl;
                break;
            }
        }
        if (found_fd) {
            fprintf(out, "%s(", rae_mangle_function(ctx->compiler_ctx, found_fd));
            const AstCallArg* a = expr->as.call.args;
            const AstParam* p = found_fd->params;
            while (a) {
                bool needs_box = false;
                if (p && p->type && a->value->kind != AST_EXPR_BOX) {
                    Str param_base = get_base_type_name(p->type);
                    if (str_eq_cstr(param_base, "Any")) needs_box = true;
                }
                if (needs_box) fprintf(out, "rae_any((");
                emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
                if (needs_box) fprintf(out, "))");
                if (a->next) fprintf(out, ", ");
                a = a->next; if (p) p = p->next;
            }
            fprintf(out, ")");
            return true;
        }
        // Last resort: emit as rae_<name>
        fprintf(out, "rae_%.*s(", (int)callee_name.len, callee_name.data);
        const AstCallArg* a = expr->as.call.args;
        while (a) {
            emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
            if (a->next) fprintf(out, ", ");
            a = a->next;
        }
        fprintf(out, ")");
        return true;
    }
    return false;
}

static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    if (!stmt) return true;
    switch (stmt->kind) {
        case AST_STMT_EXPR: emit_expr(ctx, stmt->as.expr_stmt, out, PREC_LOWEST, false, false); fprintf(out, ";\n"); break;
        case AST_STMT_LET: {
            fprintf(out, "  ");
            emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
            fprintf(out, " %.*s = ", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
            bool is_ref_bind = stmt->as.let_stmt.is_bind && stmt->as.let_stmt.type &&
                               (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod);
            if (is_ref_bind) {
                Str base = get_base_type_name(stmt->as.let_stmt.type);
                if (is_primitive_type(base)) {
                    // Primitive ref: rae_Mod_Int64 r = { .ptr = &x };
                    fprintf(out, "{ .ptr = &");
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, true, true);
                    fprintf(out, " }");
                } else {
                    fprintf(out, "&");
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, true);
                }
            } else if (stmt->as.let_stmt.value) {
                emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
            } else {
                // Auto-init: let x: Type (no initializer)
                emit_auto_init(ctx, stmt->as.let_stmt.type, out);
            }
            fprintf(out, ";\n");
            const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.let_stmt.type);
            if (ctx->local_count < 256) { ctx->locals[ctx->local_count] = stmt->as.let_stmt.name; ctx->local_types[ctx->local_count] = str_from_cstr(tn); ctx->local_type_refs[ctx->local_count] = stmt->as.let_stmt.type; ctx->local_count++; }
            break;
        }
        case AST_STMT_ASSIGN: {
            fprintf(out, "  ");
            emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, false);
            fprintf(out, " = ");
            // For struct literal assignments, add compound literal cast if missing
            if (stmt->as.assign_stmt.value->kind == AST_EXPR_OBJECT &&
                !stmt->as.assign_stmt.value->as.object_literal.type) {
                const AstTypeRef* target_tr = infer_expr_type_ref(ctx, stmt->as.assign_stmt.target);
                if (target_tr) {
                    fprintf(out, "(");
                    emit_type_ref_as_c_type(ctx, target_tr, out, true);
                    fprintf(out, ")");
                }
            }
            emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
            fprintf(out, ";\n");
            break;
        }
        case AST_STMT_RET: 
            fprintf(out, "  return "); 
            if (stmt->as.ret_stmt.values) {
                if (stmt->as.ret_stmt.values->value->kind == AST_EXPR_LIST) {
                    emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                } else {
                    emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                }
            }
            fprintf(out, ";\n"); 
            break;
        case AST_STMT_IF: emit_if(ctx, stmt, out); break;
        case AST_STMT_LOOP: emit_loop(ctx, stmt, out); break;
        case AST_STMT_MATCH: {
            const AstExpr* subject = stmt->as.match_stmt.subject;
            bool first = true;
            for (const AstMatchCase* c = stmt->as.match_stmt.cases; c; c = c->next) {
                if (!c->pattern) {
                    // default case
                    if (!first) fprintf(out, " else {\n");
                    else fprintf(out, "  {\n");
                } else {
                    fprintf(out, first ? "  if (" : " else if (");
                    emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
                    fprintf(out, " == ");
                    emit_expr(ctx, c->pattern, out, PREC_LOWEST, false, false);
                    fprintf(out, ") {\n");
                }
                first = false;
                if (c->block) {
                    for (AstStmt* s = c->block->first; s; s = s->next)
                        emit_stmt(ctx, s, out);
                }
                fprintf(out, "  }");
            }
            fprintf(out, "\n");
            break;
        }
        default: break;
    }
    return true;
}

static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out) { (void)ctx; (void)min_depth; (void)out; return true; }
static void pop_defers(CFuncContext* ctx, int depth) { (void)ctx; (void)depth; }

static bool emit_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, FILE* out, const struct VmRegistry* r, bool ray) {
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
  
  if (is_main) fprintf(out, "  return 0;\n}\n\n");
  else fprintf(out, "}\n\n"); 
  return true;
}

static bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray) {
  const AstIdentifierPart* gp_src = f->generic_params; if (!gp_src && f->generic_template) gp_src = f->generic_template->as.func_decl.generic_params;
  CFuncContext tctx = {.compiler_ctx = ctx, .module = m, .func_decl = f, .uses_raylib = ray, .registry = r, .generic_params = gp_src, .generic_args = args};
  const char* rt = c_return_type(&tctx, f); const char* mangled = rae_mangle_specialized_function(ctx, f, args);
  fprintf(out, "RAE_UNUSED static %s %s(", rt, mangled); emit_param_list(&tctx, f->params, out, false); fprintf(out, ") {\n");
  for (const AstParam* p = f->params; p; p = p->next) { if (tctx.local_count < 256) { tctx.locals[tctx.local_count] = p->name; tctx.local_type_refs[tctx.local_count] = p->type; tctx.local_types[tctx.local_count] = str_from_cstr(rae_mangle_type_specialized(ctx, gp_src, args, p->type)); tctx.local_count++; } }
  if (f->body) { for (AstStmt* s = f->body->first; s; s = s->next) emit_stmt(&tctx, s, out); }
  fprintf(out, "}\n\n"); return true;
}

bool c_backend_emit_module(CompilerContext* ctx, const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {
  if (!module) return false;
  ctx->all_decl_count = 0; collect_decls_from_module(ctx, module); ctx->current_module = (AstModule*)module;
  
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

  // Prototypes for non-generic functions
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params && !d->as.func_decl.is_extern && !str_eq_cstr(d->as.func_decl.name, "main")) {
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
      CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .generic_params = f->generic_params, .generic_args = args};
      fprintf(out, "RAE_UNUSED static %s %s(", c_return_type(&tctx, f), mangled); emit_param_list(&tctx, f->params, out, false); fprintf(out, ");\n");
  }
  
  // Bodies for non-generic functions
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params && !d->as.func_decl.is_extern && !str_eq_cstr(d->as.func_decl.name, "main")) {
          emit_function(ctx, module, &d->as.func_decl, out, registry, false);
      }
  }

  // Bodies for specialized functions
  for (size_t i = 0; i < ctx->specialized_func_count; i++) emit_specialized_function(ctx, module, ctx->specialized_funcs[i].decl, ctx->specialized_funcs[i].concrete_args, out, registry, false);
  
  // Finally emit main
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC && str_eq_cstr(d->as.func_decl.name, "main")) {
          emit_function(ctx, module, &d->as.func_decl, out, registry, false);
      }
  }

  fclose(out); return true;
}
