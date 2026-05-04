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
  bool suppress_opt_unbox; // when true, skip emit_opt_unbox_suffix (e.g. inside `... is none`)
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

static void emit_type_info_as_c_type(CFuncContext* ctx, TypeInfo* t, FILE* out) {
    if (!t) { fprintf(out, "RaeAny"); return; }
    AstTypeRef tmp = {0};
    tmp.resolved_type = t;
    emit_type_ref_as_c_type(ctx, &tmp, out, false);
}
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

static bool emit_auto_init(CFuncContext* ctx, const AstTypeRef* type, FILE* out);
static bool emit_struct_auto_init(CFuncContext* ctx, const AstDecl* decl, const AstTypeRef* tr, FILE* out);
static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count, bool is_method, const AstExpr* call_expr);
static bool is_primitive_ref(CFuncContext* ctx, const AstTypeRef* tr) {
    if (!tr || !(tr->is_view || tr->is_mod)) return false;
    Str base = get_base_type_name(tr);
    // Buffer and List are already pointers — no wrapper struct
    if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List") || str_eq_cstr(base, "Any")) return false;
    if (str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float64") ||
        str_eq_cstr(base, "Bool") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32") || str_eq_cstr(base, "String") ||
        tr->is_id || tr->is_key) return true;
    return false;
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

static void discover_specializations_expr(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_CALL: {
            const AstExpr* callee = expr->as.call.callee;
            if (callee->kind == AST_EXPR_IDENT) {
                uint16_t param_count = 0; for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                // Receiver-aware overload selection: when the callee has a "this" first
                // param and we have a first arg, pick the overload whose first param's
                // base type matches the receiver's base type. This avoids picking the
                // wrong overload for `set(map, k, v)` where multiple `set` overloads
                // exist (List/StringMap/IntMap). Mirrors the emit_call_expr logic.
                const AstFuncDecl* d = NULL;
                {
                    Str receiver_base = {0};
                    if (expr->as.call.args) {
                        const AstTypeRef* recv_tr = infer_expr_type_ref(ctx, expr->as.call.args->value);
                        if (recv_tr && ctx->generic_params && ctx->generic_args) {
                            recv_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, recv_tr);
                        }
                        if (recv_tr) receiver_base = get_base_type_name(recv_tr);
                    }
                    const AstFuncDecl* generic_fallback = NULL;
                    for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count && !d; i++) {
                        const AstDecl* dd = ctx->compiler_ctx->all_decls[i];
                        if (dd->kind != AST_DECL_FUNC || !str_eq(dd->as.func_decl.name, callee->as.ident)) continue;
                        uint16_t pc = 0; for (const AstParam* pp = dd->as.func_decl.params; pp; pp = pp->next) pc++;
                        if (pc != param_count) continue;
                        // Prefer matching receiver base when first param is "this".
                        if (dd->as.func_decl.params && str_eq_cstr(dd->as.func_decl.params->name, "this") && receiver_base.len > 0) {
                            Str param_base = get_base_type_name(dd->as.func_decl.params->type);
                            if (str_eq(param_base, receiver_base)) { d = &dd->as.func_decl; break; }
                        }
                        if (!generic_fallback) generic_fallback = &dd->as.func_decl;
                    }
                    if (!d) d = generic_fallback;
                }
                if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, false, expr);
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
                    } else if (d->params) {
                        // Try inference from each param/arg pair (not just `this`).
                        const AstParam* p = d->params;
                        const AstCallArg* a = expr->as.call.args;
                        while (!inferred_args && p && a) {
                            const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                            if (arg_tr) {
                                if (ctx->generic_params && ctx->generic_args)
                                    arg_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg_tr);
                                AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, p->type, arg_tr);
                                if (inferred) inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                            }
                            p = p->next; a = a->next;
                        }
                    }
                    // Try return-type inference from expected type (let x: Type = genericFunc(...))
                    if (!inferred_args && ctx->has_expected_type && d->returns && d->returns->type) {
                        AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->returns->type, &ctx->expected_type);
                        if (inferred) inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
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
                // When discovering inside a generic body, `this`'s type is the template
                // (e.g. `view List(T)` with literal T). Substitute through the current
                // generic context so inference can bind T to the concrete arg.
                if (receiver_type && ctx->generic_params && ctx->generic_args)
                    receiver_type = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, receiver_type);
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
        case AST_EXPR_INTERP: {
            for (const AstInterpPart* p = expr->as.interp.parts; p; p = p->next) discover_specializations_expr(ctx, p->value);
            break;
        }
        case AST_EXPR_BOX:
        case AST_EXPR_UNBOX:
            // Sema wraps args being coerced to/from RaeAny; the inner expression may
            // contain method calls whose specialisations must still be discovered.
            discover_specializations_expr(ctx, expr->as.unary.operand);
            break;
        case AST_EXPR_COLLECTION_LITERAL: {
            for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) discover_specializations_expr(ctx, e->value);
            // Register createList and add specializations from expected type
            if (ctx->has_expected_type && ctx->expected_type.generic_args) {
                const AstTypeRef* elem_type = ctx->expected_type.generic_args;
                const AstFuncDecl* create_fd = NULL;
                const AstFuncDecl* add_fd = NULL;
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind != AST_DECL_FUNC) continue;
                    if (str_eq_cstr(d->as.func_decl.name, "createList") && d->as.func_decl.generic_params) create_fd = &d->as.func_decl;
                    if (str_eq_cstr(d->as.func_decl.name, "add") && d->as.func_decl.generic_params) add_fd = &d->as.func_decl;
                }
                if (create_fd) register_function_specialization(ctx->compiler_ctx, create_fd, elem_type);
                if (add_fd) register_function_specialization(ctx->compiler_ctx, add_fd, elem_type);
                register_generic_type(ctx->compiler_ctx, &ctx->expected_type);
            }
            break;
        }
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
            case AST_STMT_RET: if (s->as.ret_stmt.values && s->as.ret_stmt.values->value) discover_specializations_expr(ctx, s->as.ret_stmt.values->value); break;
            case AST_STMT_MATCH: {
                if (s->as.match_stmt.subject) discover_specializations_expr(ctx, s->as.match_stmt.subject);
                for (const AstMatchCase* mc = s->as.match_stmt.cases; mc; mc = mc->next) {
                    if (mc->pattern) discover_specializations_expr(ctx, mc->pattern);
                    if (mc->block) discover_specializations_stmt(ctx, mc->block->first);
                }
                break;
            }
            default: break;
        }
    }
}

static void discover_specializations_module(CompilerContext* ctx, const AstModule* module) {
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        const AstDecl* d = ctx->all_decls[i];
        if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params && !d->as.func_decl.specialization_args) {
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = &d->as.func_decl};
            // Pre-populate params as locals so infer_expr_type_ref can resolve `this`
            // and other parameters when walking the body.
            for (const AstParam* p = d->as.func_decl.params; p; p = p->next) {
                if (fctx.local_count < 256) {
                    fctx.locals[fctx.local_count] = p->name;
                    fctx.local_type_refs[fctx.local_count] = p->type;
                    const char* tn = rae_mangle_type_specialized(ctx, NULL, NULL, p->type);
                    fctx.local_types[fctx.local_count] = str_from_cstr(tn);
                    fctx.local_count++;
                }
            }
            if (d->as.func_decl.body) discover_specializations_stmt(&fctx, d->as.func_decl.body->first);
        }
    }
    size_t discovered = 0;
    while (discovered < ctx->specialized_func_count) {
        size_t limit = ctx->specialized_func_count;
        for (size_t i = discovered; i < limit; i++) {
            const AstFuncDecl* f = ctx->specialized_funcs[i].decl; const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
            if (!f) continue;
            const AstIdentifierPart* disc_gp = f->generic_params;
            if (!disc_gp && f->generic_template && f->generic_template->kind == AST_DECL_FUNC) disc_gp = f->generic_template->as.func_decl.generic_params;
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = f, .generic_params = disc_gp, .generic_args = args};
            // Populate locals from params so infer_expr_type_ref works for 'this' etc.
            for (const AstParam* p = f->params; p; p = p->next) {
                if (fctx.local_count < 256) {
                    fctx.locals[fctx.local_count] = p->name;
                    fctx.local_type_refs[fctx.local_count] = p->type;
                    const char* tn = rae_mangle_type_specialized(ctx, disc_gp, args, p->type);
                    fctx.local_types[fctx.local_count] = str_from_cstr(tn);
                    fctx.local_count++;
                }
            }
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
      // Special case: string equality — use rae_ext_rae_str_eq instead of ==
      if (expr->as.binary.op == AST_BIN_IS) {
          const AstTypeRef* lhs_tr = infer_expr_type_ref(ctx, expr->as.binary.lhs);
          Str lhs_base = get_base_type_name(lhs_tr);
          bool lhs_is_string = str_eq_cstr(lhs_base, "String") || str_eq_cstr(lhs_base, "rae_String");
          bool rhs_is_string_lit = expr->as.binary.rhs->kind == AST_EXPR_STRING;
          // Also detect toString() calls — they always return String
          bool lhs_is_tostring = expr->as.binary.lhs->kind == AST_EXPR_METHOD_CALL &&
              str_eq_cstr(expr->as.binary.lhs->as.method_call.method_name, "toString");
          if (lhs_is_string || rhs_is_string_lit || lhs_is_tostring) {
              fprintf(out, "(bool)rae_ext_rae_str_eq(");
              emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
              fprintf(out, ", ");
              emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
              fprintf(out, ")");
              break;
          }
      }
      // `x is none` / `none is x`: keep the opt result as RaeAny so the comparison
      // against rae_any_none() type-checks. Otherwise auto-unbox would yield a
      // primitive on one side and RaeAny on the other.
      bool none_compare = (expr->as.binary.op == AST_BIN_IS) &&
          (expr->as.binary.rhs->kind == AST_EXPR_NONE || expr->as.binary.lhs->kind == AST_EXPR_NONE);
      bool saved_unbox = ctx->suppress_opt_unbox;
      if (none_compare) ctx->suppress_opt_unbox = true;
      // Float modulo: emit fmod(a, b) instead of a % b
      if (expr->as.binary.op == AST_BIN_MOD) {
          bool lhs_float = expr->as.binary.lhs->kind == AST_EXPR_FLOAT;
          bool rhs_float = expr->as.binary.rhs->kind == AST_EXPR_FLOAT;
          if (!lhs_float && !rhs_float) {
              const AstTypeRef* ltr = infer_expr_type_ref(ctx, expr->as.binary.lhs);
              Str lb = get_base_type_name(ltr);
              if (str_eq_cstr(lb, "Float64") || str_eq_cstr(lb, "Float") || str_eq_cstr(lb, "Float32") || str_eq_cstr(lb, "double")) lhs_float = true;
          }
          if (lhs_float || rhs_float) {
              fprintf(out, "fmod(");
              emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
              fprintf(out, ", ");
              emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
              fprintf(out, ")");
              ctx->suppress_opt_unbox = saved_unbox;
              break;
          }
      }
      // For arithmetic/comparison ops on primitives, propagate the side that has a
      // known primitive type as the expected type for both sides. This lets calls
      // returning opt T auto-unbox when used in `g.grid.get(i) > 0` etc.
      bool is_arith_or_cmp = (expr->as.binary.op >= AST_BIN_ADD && expr->as.binary.op <= AST_BIN_GE) ||
                             expr->as.binary.op == AST_BIN_IS;
      bool had_exp_bin = ctx->has_expected_type;
      AstTypeRef saved_exp_bin = ctx->expected_type;
      if (is_arith_or_cmp && !none_compare) {
          const AstTypeRef* lhs_ti = infer_expr_type_ref(ctx, expr->as.binary.lhs);
          const AstTypeRef* rhs_ti = infer_expr_type_ref(ctx, expr->as.binary.rhs);
          const AstTypeRef* picked = NULL;
          // Prefer whichever side has a non-opt primitive type.
          if (lhs_ti && !lhs_ti->is_opt) {
              Str b = get_base_type_name(lhs_ti);
              if (is_primitive_type(b) && !str_eq_cstr(b, "Any")) picked = lhs_ti;
          }
          if (!picked && rhs_ti && !rhs_ti->is_opt) {
              Str b = get_base_type_name(rhs_ti);
              if (is_primitive_type(b) && !str_eq_cstr(b, "Any")) picked = rhs_ti;
          }
          if (picked) { ctx->expected_type = *picked; ctx->has_expected_type = true; }
      }
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
      if (prec < parent_prec) fprintf(out, ")"); if (is_bool_op) fprintf(out, ")");
      ctx->has_expected_type = had_exp_bin;
      ctx->expected_type = saved_exp_bin;
      ctx->suppress_opt_unbox = saved_unbox;
      break;
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
        // Built-in method: toString() → rae_ext_rae_str(object)
        if (str_eq_cstr(expr->as.method_call.method_name, "toString") && !expr->as.method_call.args) {
            fprintf(out, "rae_ext_rae_str((");
            emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, false);
            fprintf(out, "))");
            break;
        }
        // Built-in method: toJson() → rae_toJson_TYPE_(&object)
        if (str_eq_cstr(expr->as.method_call.method_name, "toJson") && !expr->as.method_call.args) {
            const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.method_call.object);
            Str obj_base = get_base_type_name(obj_tr);
            const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, obj_tr);
            fprintf(out, "rae_toJson_%s_(&", mangled);
            emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, true, false);
            fprintf(out, ")");
            break;
        }
        // Built-in static method: Type.fromJson(json: str) → rae_fromJson_TYPE_(str)
        if (str_eq_cstr(expr->as.method_call.method_name, "fromJson")) {
            Str type_name = {0};
            if (expr->as.method_call.object->kind == AST_EXPR_IDENT) type_name = expr->as.method_call.object->as.ident;
            if (type_name.len > 0) {
                fprintf(out, "rae_fromJson_rae_%.*s_(", (int)type_name.len, type_name.data);
                if (expr->as.method_call.args) emit_expr(ctx, expr->as.method_call.args->value, out, PREC_LOWEST, false, false);
                fprintf(out, ")");
                break;
            }
        }
        // Module-qualified call: `sys.fn(args)` where `sys` is an imported module
        // name. The c_backend flattens imports into ctx->all_decls and clears
        // module->imports, so detect it by: object is an IDENT, the ident has
        // no local binding and no inferable type, and a function `method_name`
        // exists in the project.
        if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
            Str obj_name = expr->as.method_call.object->as.ident;
            const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.method_call.object);
            bool obj_has_value = obj_tr != NULL || is_pointer_type(ctx, obj_name);
            bool fn_exists = false;
            if (!obj_has_value) {
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, expr->as.method_call.method_name)) {
                        fn_exists = true; break;
                    }
                }
            }
            if (!obj_has_value && fn_exists) {
                AstExpr call = { .kind = AST_EXPR_CALL, .line = expr->line, .column = expr->column, .decl_link = expr->decl_link };
                call.as.call.callee = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstExpr));
                call.as.call.callee->kind = AST_EXPR_IDENT;
                call.as.call.callee->as.ident = expr->as.method_call.method_name;
                call.as.call.args = expr->as.method_call.args;
                call.as.call.generic_args = expr->as.method_call.generic_args;
                emit_call_expr(ctx, &call, out); break;
            }
        }
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
        bool use_arrow = (obj_tr && (obj_tr->is_view || obj_tr->is_mod));
        emit_expr(ctx, expr->as.member.object, out, PREC_CALL, true, false);
        fprintf(out, "%s%.*s", use_arrow ? "->" : ".", (int)expr->as.member.member.len, expr->as.member.member.data);
        break;
    }
    case AST_EXPR_INDEX: {
        // List(T) is a struct, not a raw array; lower `xs[i]` to a typed
        // buffer access on the data pointer.
        const AstTypeRef* tgt_tr = infer_expr_type_ref(ctx, expr->as.index.target);
        Str tgt_base = get_base_type_name(tgt_tr);
        if (str_eq_cstr(tgt_base, "List")) {
            const AstTypeRef* elem_tr = tgt_tr ? tgt_tr->generic_args : NULL;
            AstTypeRef* sub = elem_tr ? substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, elem_tr) : NULL;
            fprintf(out, "(*(");
            if (sub) emit_type_ref_as_c_type(ctx, sub, out, false); else fprintf(out, "RaeAny");
            fprintf(out, "*)( (char*)((");
            emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false);
            fprintf(out, ").data) + (");
            emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false, false);
            fprintf(out, ") * sizeof(");
            if (sub) emit_type_ref_as_c_type(ctx, sub, out, false); else fprintf(out, "RaeAny");
            fprintf(out, ") ))");
            break;
        }
        emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false); fprintf(out, "["); emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false, false); fprintf(out, "]"); break;
    }
    case AST_EXPR_BOX: {
        // For primitive refs, pass wrapper directly so rae_any picks mod/view variant
        const AstTypeRef* box_tr = infer_expr_type_ref(ctx, expr->as.unary.operand);
        bool box_suppress = box_tr && (box_tr->is_view || box_tr->is_mod) && is_primitive_type(get_base_type_name(box_tr));
        fprintf(out, "rae_any(("); emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, box_suppress); fprintf(out, "))");
        break;
    }
    case AST_EXPR_UNBOX: {
        if (expr->resolved_type) {
            // Check if the operand already returns the concrete type (not RaeAny)
            // This happens when an extern function returns e.g. rae_String directly
            // but sema inserted UNBOX because the Rae decl says ret opt String
            bool skip_unbox = false;
            const AstExpr* inner = expr->as.unary.operand;
            // Skip through nested BOX to find the actual call
            if (inner->kind == AST_EXPR_BOX) inner = inner->as.unary.operand;
            // Check via decl_link
            if (inner->kind == AST_EXPR_CALL && inner->decl_link && inner->decl_link->kind == AST_DECL_FUNC) {
                const AstFuncDecl* ifd = &inner->decl_link->as.func_decl;
                if (ifd->is_extern) skip_unbox = true;
            }
            // Also check: if inner is a call whose callee name starts with rae_ext_ or is an extern
            // (handles fallback path where decl_link is NULL)
            if (!skip_unbox && inner->kind == AST_EXPR_CALL && inner->as.call.callee &&
                inner->as.call.callee->kind == AST_EXPR_IDENT) {
                Str cname = inner->as.call.callee->as.ident;
                if (str_starts_with_cstr(cname, "rae_ext_") || str_starts_with_cstr(cname, "rae_sys_") ||
                    str_starts_with_cstr(cname, "rae_io_") || str_starts_with_cstr(cname, "rae_crypto_")) {
                    skip_unbox = true;
                }
                // Also look up function by name — if it's extern, skip
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count && !skip_unbox; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, cname) && d->as.func_decl.is_extern)
                        skip_unbox = true;
                }
            }
            if (skip_unbox) {
                emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
            } else {
                TypeInfo* t = expr->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                fprintf(out, "("); emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
                if (t->kind == TYPE_INT || t->kind == TYPE_CHAR) fprintf(out, ").as.i"); else if (t->kind == TYPE_FLOAT) fprintf(out, ").as.f"); else if (t->kind == TYPE_BOOL) fprintf(out, ").as.b"); else if (t->kind == TYPE_STRING) fprintf(out, ").as.s"); else fprintf(out, ").as.ptr");
            }
        } else emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
        break;
    }
    case AST_EXPR_OBJECT: {
        // Resolve the literal's struct type so we can propagate per-field expected
        // types into each value. This lets generic calls infer args from the
        // surrounding field type (e.g. `grid: createList(initialCap: 200)`
        // where the field's declared type is `List(Int)`).
        const AstTypeRef* obj_tr = expr->as.object_literal.type;
        if (!obj_tr && ctx->has_expected_type) obj_tr = &ctx->expected_type;
        const AstDecl* struct_decl = NULL;
        if (obj_tr) {
            Str obj_base = get_base_type_name(obj_tr);
            struct_decl = find_type_decl(ctx, ctx->module, obj_base);
        }
        if (expr->as.object_literal.type) {
            fprintf(out, "(");
            emit_type_ref_as_c_type(ctx, expr->as.object_literal.type, out, false);
            fprintf(out, ")");
        } else if (ctx->has_expected_type) {
            fprintf(out, "(");
            emit_type_ref_as_c_type(ctx, &ctx->expected_type, out, false);
            fprintf(out, ")");
        }
        fprintf(out, "{ ");
        bool saved_has_exp = ctx->has_expected_type;
        AstTypeRef saved_exp = ctx->expected_type;
        for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
            fprintf(out, ".%.*s = ", (int)f->name.len, f->name.data);
            // Look up the field's declared type and use it as expected_type.
            const AstTypeRef* field_tr = NULL;
            if (struct_decl && struct_decl->kind == AST_DECL_TYPE) {
                for (const AstTypeField* td = struct_decl->as.type_decl.fields; td; td = td->next) {
                    if (str_eq(td->name, f->name)) { field_tr = td->type; break; }
                }
            }
            if (field_tr) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx,
                    struct_decl->as.type_decl.generic_params,
                    obj_tr ? obj_tr->generic_args : NULL, field_tr);
                ctx->expected_type = *sub;
                ctx->has_expected_type = true;
            } else {
                ctx->has_expected_type = false;
            }
            emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
            if (f->next) fprintf(out, ", ");
        }
        ctx->has_expected_type = saved_has_exp;
        ctx->expected_type = saved_exp;
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
    case AST_EXPR_MATCH: {
        // Match expression: emit as ternary chain
        // match x { case 1 => 10, case 2 => 20, default => 30 }
        // -> (x == 1) ? 10 : (x == 2) ? 20 : 30
        const AstMatchArm* arm = expr->as.match_expr.arms;
        fprintf(out, "(");
        while (arm) {
            if (!arm->pattern) {
                // default arm
                emit_expr(ctx, arm->value, out, PREC_LOWEST, false, false);
            } else {
                // Check if string comparison needed
                const AstTypeRef* subj_tr = infer_expr_type_ref(ctx, expr->as.match_expr.subject);
                Str subj_base = get_base_type_name(subj_tr);
                bool is_string = str_eq_cstr(subj_base, "String") || str_eq_cstr(subj_base, "rae_String");
                if (is_string) {
                    fprintf(out, "rae_ext_rae_str_eq(");
                    emit_expr(ctx, expr->as.match_expr.subject, out, PREC_LOWEST, false, false);
                    fprintf(out, ", ");
                    emit_expr(ctx, arm->pattern, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                } else {
                    fprintf(out, "(");
                    emit_expr(ctx, expr->as.match_expr.subject, out, PREC_LOWEST, false, false);
                    fprintf(out, " == ");
                    emit_expr(ctx, arm->pattern, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                }
                fprintf(out, " ? ");
                emit_expr(ctx, arm->value, out, PREC_LOWEST, false, false);
                fprintf(out, " : ");
            }
            arm = arm->next;
        }
        fprintf(out, ")");
        break;
    }
    case AST_EXPR_NONE: fprintf(out, "rae_any_none()"); break;
    default: break;
  }
  return true;
}

// Emit unbox suffix for opt T return types: .as.s, .as.i, .as.f, .as.b
static void emit_opt_unbox_suffix(CFuncContext* ctx, const AstFuncDecl* fd, const AstTypeRef* call_concrete, FILE* out) {
    if (!fd->returns || !fd->returns->type || !fd->returns->type->is_opt) return;

    // c_return_type always emits "RaeAny" for opt T (even on specialised clones),
    // so unboxing is needed regardless of whether the call resolves to a template
    // or a sema-generated specialisation.
    if (fd->is_extern) return;

    // Only unbox when the call's result is being consumed as a concrete primitive type
    // (e.g. `let s: String = get(...)`). When the consumer expects RaeAny (log args,
    // interpolation, none comparisons), keep the RaeAny so the runtime can format it.
    if (!ctx->has_expected_type) return;
    Str expected_base = get_base_type_name(&ctx->expected_type);
    bool expected_concrete = str_eq_cstr(expected_base, "Int") || str_eq_cstr(expected_base, "Int64") ||
        str_eq_cstr(expected_base, "Float") || str_eq_cstr(expected_base, "Float64") ||
        str_eq_cstr(expected_base, "Bool") || str_eq_cstr(expected_base, "String") ||
        str_eq_cstr(expected_base, "Char") || str_eq_cstr(expected_base, "Char32");
    if (!expected_concrete) return;

    AstTypeRef* ret_tr = fd->returns->type;
    // Pick the substitution context: prefer the call site's concrete args (when fd is
    // a generic template), then the spec's own args, finally the surrounding generic ctx.
    const AstIdentifierPart* gp = NULL;
    const AstTypeRef* ga = NULL;
    if (fd->generic_params && call_concrete) { gp = fd->generic_params; ga = call_concrete; }
    else if (fd->specialization_args && fd->generic_template && fd->generic_template->kind == AST_DECL_FUNC) {
        gp = fd->generic_template->as.func_decl.generic_params;
        ga = fd->specialization_args;
    } else { gp = ctx->generic_params; ga = ctx->generic_args; }
    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, gp, ga, ret_tr);
    Str base = get_base_type_name(sub);
    if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, ".as.i");
    else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) fprintf(out, ".as.f");
    else if (str_eq_cstr(base, "Bool")) fprintf(out, ".as.b");
    else if (str_eq_cstr(base, "String")) fprintf(out, ".as.s");
    // For other types (structs, Any), no unbox needed — RaeAny is the right type
}

static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
    Str name = {0};
    if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
        name = expr->as.call.callee->as.ident;
    } else if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) {
        name = expr->decl_link->as.func_decl.name;
        if (expr->decl_link->as.func_decl.generic_template && expr->decl_link->as.func_decl.generic_template->kind == AST_DECL_FUNC) {
            name = expr->decl_link->as.func_decl.generic_template->as.func_decl.name;
        }
    }

    // -- INTRINSICS / SPECIAL CASES --
    if (str_eq_cstr(name, "sizeof")) {
        const AstTypeRef* tr = expr->as.call.generic_args;
        if (!tr && expr->as.call.args) tr = infer_expr_type_ref(ctx, expr->as.call.args->value);
        if (tr) { fprintf(out, "sizeof("); emit_type_ref_as_c_type(ctx, tr, out, false); fprintf(out, ")"); }
        else fprintf(out, "sizeof(RaeAny)");
        return true;
    }

    // Buffer Operations (Intrinsics)
    bool is_buf_get = str_eq_cstr(name, "rae_ext___buf_get") || str_eq_cstr(name, "__buf_get") || str_eq_cstr(name, "rae_ext_rae_buf_get");
    bool is_buf_set = str_eq_cstr(name, "rae_ext___buf_set") || str_eq_cstr(name, "__buf_set") || str_eq_cstr(name, "rae_ext_rae_buf_set");
    bool is_buf_copy = str_eq_cstr(name, "rae_ext___buf_copy") || str_eq_cstr(name, "__buf_copy") || str_eq_cstr(name, "rae_ext_rae_buf_copy");

    // Helpers: emit the buffer element type (prefer AstTypeRef if available, fall back to TypeInfo).
    #define EMIT_ELEM_TYPE() do { \
        if (elem_tr) emit_type_ref_as_c_type(ctx, elem_tr, out, false); \
        else emit_type_info_as_c_type(ctx, elem_t, out); \
    } while (0)
    if (is_buf_get) {
        const AstCallArg* arg = expr->as.call.args;
        if (!arg || !arg->next) return false;
        TypeInfo* elem_t = NULL;
        AstTypeRef* elem_tr = NULL;
        // Primary path: walk the AST type of the buffer arg and substitute through the
        // current generic context. This handles compound element types like
        // `Buffer(StringMapEntry(V))` that sema may leave shallow as `Buffer(V)`.
        {
            const AstTypeRef* buf_tr = infer_expr_type_ref(ctx, arg->value);
            if (buf_tr) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, buf_tr);
                Str base = get_base_type_name(sub);
                if (str_eq_cstr(base, "Buffer") && sub->generic_args) {
                    elem_tr = sub->generic_args;
                }
            }
        }
        // Secondary: use the resolved_type if AST inference failed (works for primitive T).
        if (!elem_tr && arg->value->resolved_type) {
            TypeInfo* bt = arg->value->resolved_type;
            while (bt->kind == TYPE_REF) bt = bt->as.ref.base;
            if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base;
        }
        if (elem_t && elem_t->kind == TYPE_GENERIC_PARAM && ctx->generic_params && ctx->generic_args) {
            const AstIdentifierPart* gp = ctx->generic_params; const AstTypeRef* concrete = ctx->generic_args;
            while (gp && concrete) {
                if (str_eq(gp->text, elem_t->as.generic_param.param_name)) {
                    elem_t = concrete->resolved_type
                        ? concrete->resolved_type
                        : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)concrete);
                    break;
                }
                gp = gp->next; concrete = concrete->next;
            }
        }
        if (!elem_tr && !elem_t && ctx->generic_args) {
            elem_t = ctx->generic_args->resolved_type
                ? ctx->generic_args->resolved_type
                : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)ctx->generic_args);
        }
        fprintf(out, "(*("); EMIT_ELEM_TYPE();
        fprintf(out, "*)( (char*)("); emit_expr(ctx, arg->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") + ("); emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") * sizeof("); EMIT_ELEM_TYPE(); fprintf(out, ") ))");
        return true;
    }
    if (is_buf_set) {
        const AstCallArg* arg = expr->as.call.args;
        if (!arg || !arg->next || !arg->next->next) return false;
        TypeInfo* elem_t = NULL;
        AstTypeRef* elem_tr = NULL;
        // Primary: AST-based inference (matches buf_get path above).
        {
            const AstTypeRef* buf_tr = infer_expr_type_ref(ctx, arg->value);
            if (buf_tr) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, buf_tr);
                Str base = get_base_type_name(sub);
                if (str_eq_cstr(base, "Buffer") && sub->generic_args) {
                    elem_tr = sub->generic_args;
                }
            }
        }
        if (!elem_tr && arg->value->resolved_type) {
            TypeInfo* bt = arg->value->resolved_type;
            while (bt->kind == TYPE_REF) bt = bt->as.ref.base;
            if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base;
        }
        if (elem_t && elem_t->kind == TYPE_GENERIC_PARAM && ctx->generic_params && ctx->generic_args) {
            const AstIdentifierPart* gp = ctx->generic_params; const AstTypeRef* concrete = ctx->generic_args;
            while (gp && concrete) {
                if (str_eq(gp->text, elem_t->as.generic_param.param_name)) {
                    elem_t = concrete->resolved_type
                        ? concrete->resolved_type
                        : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)concrete);
                    break;
                }
                gp = gp->next; concrete = concrete->next;
            }
        }
        if (!elem_tr && !elem_t && ctx->generic_args) {
            elem_t = ctx->generic_args->resolved_type
                ? ctx->generic_args->resolved_type
                : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)ctx->generic_args);
        }
        fprintf(out, "(*("); EMIT_ELEM_TYPE();
        fprintf(out, "*)( (char*)("); emit_expr(ctx, arg->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") + ("); emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") * sizeof("); EMIT_ELEM_TYPE(); fprintf(out, ") )) = ");
        const AstExpr* val_expr = arg->next->next->value;
        bool target_is_any = (elem_t && elem_t->kind == TYPE_ANY)
            || (elem_t && str_eq_cstr(elem_t->name, "Any"))
            || (elem_tr && str_eq_cstr(get_base_type_name(elem_tr), "Any"));
        if (target_is_any) {
            if (val_expr->kind != AST_EXPR_BOX && val_expr->kind != AST_EXPR_UNBOX) {
                fprintf(out, "rae_any(("); emit_expr(ctx, val_expr, out, PREC_LOWEST, false, false); fprintf(out, "))");
            } else {
                emit_expr(ctx, val_expr, out, PREC_LOWEST, false, false);
            }
        } else {
            // Struct literal needs a compound literal cast: (Type){ .x = ... }
            bool needs_struct_cast = val_expr->kind == AST_EXPR_OBJECT &&
                ((elem_t && elem_t->kind == TYPE_STRUCT) ||
                 (elem_tr && get_base_type_name(elem_tr).len > 0 &&
                  !is_primitive_type(get_base_type_name(elem_tr))));
            if (needs_struct_cast) {
                fprintf(out, "("); EMIT_ELEM_TYPE(); fprintf(out, ")");
            }
            emit_expr(ctx, val_expr, out, PREC_LOWEST, false, false);
        }
        return true;
    }

    if (is_buf_copy) {
        const AstCallArg* src_arg = expr->as.call.args;
        const AstCallArg* src_off_arg = src_arg ? src_arg->next : NULL;
        const AstCallArg* dst_arg = src_off_arg ? src_off_arg->next : NULL;
        const AstCallArg* dst_off_arg = dst_arg ? dst_arg->next : NULL;
        const AstCallArg* len_arg = dst_off_arg ? dst_off_arg->next : NULL;
        const AstCallArg* elem_size_arg = len_arg ? len_arg->next : NULL;
        if (src_arg && src_off_arg && dst_arg && dst_off_arg && len_arg) {
            fprintf(out, "memcpy((char*)("); emit_expr(ctx, dst_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") + ("); emit_expr(ctx, dst_off_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") * ");
            if (elem_size_arg) emit_expr(ctx, elem_size_arg->value, out, PREC_LOWEST, false, false);
            else {
                TypeInfo* elem_t = NULL;
                if (src_arg->value->resolved_type) { TypeInfo* bt = src_arg->value->resolved_type; if (bt->kind == TYPE_REF) bt = bt->as.ref.base; if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base; }
                fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, elem_t, out); fprintf(out, ")");
            }
            fprintf(out, ", (char*)("); emit_expr(ctx, src_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") + ("); emit_expr(ctx, src_off_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") * ");
            if (elem_size_arg) emit_expr(ctx, elem_size_arg->value, out, PREC_LOWEST, false, false);
            else {
                TypeInfo* elem_t = NULL;
                if (src_arg->value->resolved_type) { TypeInfo* bt = src_arg->value->resolved_type; if (bt->kind == TYPE_REF) bt = bt->as.ref.base; if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base; }
                fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, elem_t, out); fprintf(out, ")");
            }
            fprintf(out, ", ("); emit_expr(ctx, len_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") * ");
            if (elem_size_arg) emit_expr(ctx, elem_size_arg->value, out, PREC_LOWEST, false, false);
            else {
                TypeInfo* elem_t = NULL;
                if (src_arg->value->resolved_type) { TypeInfo* bt = src_arg->value->resolved_type; if (bt->kind == TYPE_REF) bt = bt->as.ref.base; if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base; }
                fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, elem_t, out); fprintf(out, ")");
            }
            fprintf(out, ")"); return true;
        }
    }

    // -- REGULAR FUNCTION RESOLUTION --
    const AstFuncDecl* fd = NULL;
    if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) {
        fd = &expr->decl_link->as.func_decl;
    } else if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
        Str callee_name = expr->as.call.callee->as.ident;
        uint16_t call_arg_count = 0;
        for (const AstCallArg* ca = expr->as.call.args; ca; ca = ca->next) call_arg_count++;
        // Treat the call as a method-style invocation whenever there is a first arg
        // (the call may have been written as `set(this, ...)` with `this` as a positional
        // ident, so we cannot rely on `args->name == "this"`).
        Str receiver_base = {0};
        if (expr->as.call.args) {
            const AstTypeRef* recv_tr = infer_expr_type_ref(ctx, expr->as.call.args->value);
            if (recv_tr && ctx->generic_params && ctx->generic_args)
                recv_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, recv_tr);
            if (recv_tr) receiver_base = get_base_type_name(recv_tr);
        }
        const AstFuncDecl* generic_fallback = NULL;
        const AstFuncDecl* receiver_match = NULL;
        for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
            const AstDecl* d = ctx->compiler_ctx->all_decls[i];
            if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, callee_name)) {
                uint16_t param_count = 0; for (const AstParam* pp = d->as.func_decl.params; pp; pp = pp->next) param_count++;
                if (param_count != call_arg_count) continue;
                if (!d->as.func_decl.generic_params) { fd = &d->as.func_decl; break; }
                if (d->as.func_decl.params && str_eq_cstr(d->as.func_decl.params->name, "this") && receiver_base.len > 0) {
                    Str param_base = get_base_type_name(d->as.func_decl.params->type);
                    if (str_eq(param_base, receiver_base)) { receiver_match = &d->as.func_decl; continue; }
                }
                if (!generic_fallback) generic_fallback = &d->as.func_decl;
            }
        }
        if (!fd) fd = receiver_match ? receiver_match : generic_fallback;
    }

    if (fd) {
        if ((str_eq_cstr(fd->name, "log") || str_eq_cstr(fd->name, "logS")) && expr->as.call.args) {
            const AstExpr* arg_val = expr->as.call.args->value; if (arg_val->kind == AST_EXPR_BOX) arg_val = arg_val->as.unary.operand;
            const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, arg_val);
            Str arg_base = get_base_type_name(arg_tr);
            if (arg_tr && str_eq_cstr(arg_base, "List") && !arg_tr->is_view && !arg_tr->is_mod) {
                bool is_log = str_eq_cstr(fd->name, "log");
                // Pick element-kind tag for the typed runtime helper. Lists are
                // monomorphised so the buffer holds concrete elements; we cannot
                // cast `data` to `RaeAny*`.
                Str elem_base = {0};
                const AstTypeRef* elem_tr = arg_tr->generic_args;
                if (elem_tr) {
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, elem_tr);
                    elem_base = get_base_type_name(sub);
                }
                int elem_kind = 0; // RAE_LIST_ELEM_ANY
                if (str_eq_cstr(elem_base, "Int") || str_eq_cstr(elem_base, "Int64")) elem_kind = 1;
                else if (str_eq_cstr(elem_base, "Float") || str_eq_cstr(elem_base, "Float64")) elem_kind = 2;
                else if (str_eq_cstr(elem_base, "Bool")) elem_kind = 3;
                else if (str_eq_cstr(elem_base, "Char") || str_eq_cstr(elem_base, "Char32")) elem_kind = 4;
                else if (str_eq_cstr(elem_base, "String")) elem_kind = 5;
                fprintf(out, "rae_ext_rae_%s_list_typed((void*)(", is_log ? "log" : "log_stream");
                emit_expr(ctx, arg_val, out, PREC_LOWEST, false, false);
                fprintf(out, ").data, ("); emit_expr(ctx, arg_val, out, PREC_LOWEST, false, false);
                fprintf(out, ").length, ("); emit_expr(ctx, arg_val, out, PREC_LOWEST, false, false);
                fprintf(out, ").capacity, %d)", elem_kind); return true;
            }
        }

        const char* call_name = NULL; AstTypeRef* concrete = NULL;
        if (expr->as.call.generic_args) {
            concrete = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
        } else if (fd->generic_params && ctx->generic_params && ctx->generic_args) {
            AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
            for (const AstIdentifierPart* gp = fd->generic_params; gp; gp = gp->next) {
                AstTypeRef tmp = {0}; AstIdentifierPart part = {0}; part.text = gp->text; tmp.parts = &part;
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &tmp);
                AstTypeRef* node = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef)); *node = *sub; node->next = NULL;
                if (!head) head = node; else tail->next = node; tail = node;
            }
            concrete = head;
        } else if (fd->generic_params) {
            // Try inference from each param/arg pair (not just `this`) — this handles
            // top-level calls like `setValue(b, val: 100)` where the first param is
            // not named "this" but its type still binds the generic arg.
            const AstParam* p = fd->params;
            const AstCallArg* a = expr->as.call.args;
            while (!concrete && p && a) {
                const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                if (arg_tr && p->type) {
                    if (ctx->generic_params && ctx->generic_args)
                        arg_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg_tr);
                    concrete = infer_generic_args(ctx->compiler_ctx, fd, p->type, arg_tr);
                }
                p = p->next; a = a->next;
            }
            if (!concrete && ctx->has_expected_type && fd->returns && fd->returns->type) {
                concrete = infer_generic_args(ctx->compiler_ctx, fd, fd->returns->type, &ctx->expected_type);
            }
        }
        
        if (concrete) {
            const AstFuncDecl* gen_fd = fd->generic_template ? &fd->generic_template->as.func_decl : fd;
            call_name = rae_mangle_specialized_function(ctx->compiler_ctx, gen_fd, concrete);
            register_function_specialization(ctx->compiler_ctx, gen_fd, concrete);
        } else if (fd->specialization_args) {
            call_name = rae_mangle_specialized_function(ctx->compiler_ctx, fd, fd->specialization_args);
        } else {
            call_name = rae_mangle_function(ctx->compiler_ctx, fd);
        }

        fprintf(out, "%s(", call_name);
        const AstCallArg* a = expr->as.call.args; const AstParam* p = fd->params;
        while (a) {
            bool needs_addr = false; bool needs_deref = false; bool needs_prim_wrap = false; bool needs_box = false;
            if (p && p->type && (p->type->is_view || p->type->is_mod)) {
                const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                if (!(arg_tr && (arg_tr->is_view || arg_tr->is_mod))) {
                    Str base = get_base_type_name(p->type);
                    if (is_primitive_type(base) && !str_eq_cstr(base, "Buffer") && !str_eq_cstr(base, "Any")) needs_prim_wrap = true;
                    else if (!str_eq_cstr(base, "Buffer") && !str_eq_cstr(base, "Any")) needs_addr = true;
                }
            }
            if (p && p->type && fd->is_extern && !(p->type->is_view || p->type->is_mod)) {
                Str pbase = get_base_type_name(p->type);
                if (!is_primitive_type(pbase) && pbase.len > 0) {
                    const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                    if ((arg_tr && (arg_tr->is_view || arg_tr->is_mod)) || (a->value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, a->value->as.ident))) needs_deref = true;
                }
            }
            if (p && p->type && a->value->kind != AST_EXPR_BOX) {
                Str pbase_check = get_base_type_name(p->type);
                bool param_is_any = str_eq_cstr(pbase_check, "Any");
                // For generic param T that resolves to Any (e.g. List(Any) → T=Any),
                // also detect via the call's `concrete` substitution.
                if (!param_is_any && fd->generic_params && concrete) {
                    const AstIdentifierPart* gp = fd->generic_params; const AstTypeRef* ga = concrete;
                    while (gp && ga) {
                        if (str_eq(gp->text, pbase_check)) {
                            Str ga_base = get_base_type_name(ga);
                            if (str_eq_cstr(ga_base, "Any")) param_is_any = true;
                            break;
                        }
                        gp = gp->next; ga = ga->next;
                    }
                }
                if (param_is_any) needs_box = true;
            }

            if (needs_prim_wrap) {
                fprintf(out, "("); emit_type_ref_as_c_type(ctx, p->type, out, false);
                fprintf(out, "){ .ptr = ("); emit_type_ref_as_c_type(ctx, p->type, out, true); fprintf(out, "[]){");
            }
            bool had_exp = ctx->has_expected_type; AstTypeRef saved_exp = ctx->expected_type;
            if (p && p->type) {
                // Substitute generic params in the param type so opt-unbox can detect
                // concrete primitive types when the callee is a generic template.
                AstTypeRef* p_substituted = p->type;
                if (fd->generic_params && concrete) {
                    p_substituted = substitute_type_ref(ctx->compiler_ctx, fd->generic_params, concrete, p->type);
                } else if (fd->specialization_args && fd->generic_template && fd->generic_template->kind == AST_DECL_FUNC) {
                    p_substituted = substitute_type_ref(ctx->compiler_ctx,
                        fd->generic_template->as.func_decl.generic_params,
                        fd->specialization_args, p->type);
                }
                ctx->expected_type = *p_substituted; ctx->has_expected_type = true;
            }
            if (needs_addr) fprintf(out, "&");
            if (needs_deref) fprintf(out, "(*");
            if (needs_box) {
                const AstTypeRef* arg_tr2 = infer_expr_type_ref(ctx, a->value);
                bool is_prim_ref = arg_tr2 && (arg_tr2->is_view || arg_tr2->is_mod) && is_primitive_type(get_base_type_name(arg_tr2));
                fprintf(out, "rae_any(("); emit_expr(ctx, a->value, out, PREC_LOWEST, false, is_prim_ref); fprintf(out, "))");
            } else emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
            if (needs_deref) fprintf(out, ")");
            if (needs_prim_wrap) fprintf(out, "} }");
            ctx->has_expected_type = had_exp; ctx->expected_type = saved_exp;
            if (a->next) fprintf(out, ", ");
            a = a->next; if (p) p = p->next;
        }
        fprintf(out, ")");
        if (!fd->is_extern && !ctx->suppress_opt_unbox) emit_opt_unbox_suffix(ctx, fd, concrete, out);
        return true;
    }

    if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
        Str callee_name = expr->as.call.callee->as.ident;
        if (str_starts_with_cstr(callee_name, "__buf_")) fprintf(out, "rae_ext_%.*s(", (int)callee_name.len, callee_name.data);
        else fprintf(out, "rae_%.*s(", (int)callee_name.len, callee_name.data);
        const AstCallArg* a = expr->as.call.args;
        while (a) { emit_expr(ctx, a->value, out, PREC_LOWEST, false, false); if (a->next) fprintf(out, ", "); a = a->next; }
        fprintf(out, ")"); return true;
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
                    // Check if the value is a function call returning a ref type
                    // (can't take address of rvalue — assign directly)
                    bool value_returns_ref = false;
                    if (stmt->as.let_stmt.value && (stmt->as.let_stmt.value->kind == AST_EXPR_CALL || stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL)) {
                        const AstExpr* val = stmt->as.let_stmt.value;
                        const AstFuncDecl* vfd = val->decl_link ? &val->decl_link->as.func_decl : NULL;
                        if (vfd && vfd->returns && vfd->returns->type && (vfd->returns->type->is_view || vfd->returns->type->is_mod))
                            value_returns_ref = true;
                    }
                    if (value_returns_ref) {
                        // Function already returns ref wrapper — assign directly
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    } else {
                        // Primitive ref: rae_Mod_Int64 r = { .ptr = &x };
                        fprintf(out, "{ .ptr = &");
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, true, true);
                        fprintf(out, " }");
                    }
                } else {
                    bool value_returns_ref = false;
                    if (stmt->as.let_stmt.value && (stmt->as.let_stmt.value->kind == AST_EXPR_CALL || stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL)) {
                        const AstExpr* val = stmt->as.let_stmt.value;
                        const AstFuncDecl* vfd = val->decl_link ? &val->decl_link->as.func_decl : NULL;
                        if (vfd && vfd->returns && vfd->returns->type && (vfd->returns->type->is_view || vfd->returns->type->is_mod))
                            value_returns_ref = true;
                    }
                    if (value_returns_ref) {
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    } else {
                        fprintf(out, "&");
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, true);
                    }
                }
            } else if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == AST_EXPR_COLLECTION_LITERAL) {
                // Collection literal: let x: List(Int) = { 10, 20, 30 }
                // Emit as: createList(count) followed by add() calls
                const AstTypeRef* list_type = stmt->as.let_stmt.type;
                const AstTypeRef* elem_type = list_type ? list_type->generic_args : NULL;
                int count = 0;
                for (const AstCollectionElement* e = stmt->as.let_stmt.value->as.collection.elements; e; e = e->next) count++;

                // Find createList and add functions
                const AstFuncDecl* create_fd = NULL;
                const AstFuncDecl* add_fd = NULL;
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind != AST_DECL_FUNC) continue;
                    if (str_eq_cstr(d->as.func_decl.name, "createList") && d->as.func_decl.generic_params) create_fd = &d->as.func_decl;
                    if (str_eq_cstr(d->as.func_decl.name, "add") && d->as.func_decl.generic_params) add_fd = &d->as.func_decl;
                }

                if (create_fd && add_fd && elem_type) {
                    // Register specializations
                    register_function_specialization(ctx->compiler_ctx, create_fd, elem_type);
                    register_function_specialization(ctx->compiler_ctx, add_fd, elem_type);
                    // Emit: Type name = createList_T_(count);
                    const char* create_name = rae_mangle_specialized_function(ctx->compiler_ctx, create_fd, elem_type);
                    fprintf(out, "%s(((int64_t)%dLL));\n", create_name, count);
                    // Emit add calls
                    const char* add_name = rae_mangle_specialized_function(ctx->compiler_ctx, add_fd, elem_type);
                    Str var_name = stmt->as.let_stmt.name;
                    Str et_base = get_base_type_name(elem_type);
                    bool elem_is_any = str_eq_cstr(et_base, "Any") || str_eq_cstr(et_base, "RaeAny");
                    for (const AstCollectionElement* e = stmt->as.let_stmt.value->as.collection.elements; e; e = e->next) {
                        fprintf(out, "  %s(&%.*s, ", add_name, (int)var_name.len, var_name.data);
                        if (elem_is_any) fprintf(out, "rae_any((");
                        emit_expr(ctx, e->value, out, PREC_LOWEST, false, false);
                        if (elem_is_any) fprintf(out, "))");
                        fprintf(out, ");\n");
                    }
                    // Register generic type for struct emission
                    register_generic_type(ctx->compiler_ctx, list_type);
                } else {
                    fprintf(out, "{0};\n");
                }
                // Skip the trailing ";\n" since we already emitted it
                const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.let_stmt.type);
                if (ctx->local_count < 256) { ctx->locals[ctx->local_count] = stmt->as.let_stmt.name; ctx->local_types[ctx->local_count] = str_from_cstr(tn); ctx->local_type_refs[ctx->local_count] = stmt->as.let_stmt.type; ctx->local_count++; }
                break;
            } else if (stmt->as.let_stmt.value) {
                // Set expected type so generic call resolution can infer from let type
                if (stmt->as.let_stmt.type) { ctx->expected_type = *stmt->as.let_stmt.type; ctx->has_expected_type = true; }
                // If declared type is `opt T` and the value's inferred type is the
                // concrete T, wrap with rae_any() so RaeAny holds the boxed value.
                bool needs_box = false;
                if (stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_opt) {
                    const AstTypeRef* val_tr = infer_expr_type_ref(ctx, stmt->as.let_stmt.value);
                    if (val_tr && !val_tr->is_opt) {
                        Str val_base = get_base_type_name(val_tr);
                        // Don't box if value is already RaeAny (e.g. another opt result)
                        if (!str_eq_cstr(val_base, "Any") && val_base.len > 0) needs_box = true;
                    }
                }
                if (needs_box) fprintf(out, "rae_any((");
                emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                if (needs_box) fprintf(out, "))");
                ctx->has_expected_type = false;
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
            // Check if assigning to a mod ref variable (e.g. rx = 10 where rx is rae_Mod_Int64)
            const AstTypeRef* target_tr = infer_expr_type_ref(ctx, stmt->as.assign_stmt.target);
            bool is_mod_ref = target_tr && target_tr->is_mod;
            bool is_prim_mod_ref = is_mod_ref && is_primitive_type(get_base_type_name(target_tr));

            if (is_prim_mod_ref) {
                // *rx.ptr = value
                fprintf(out, "*");
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, true);
                fprintf(out, ".ptr = ");
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
            } else if (is_mod_ref) {
                // *r = value (for non-primitive mod refs like mod Point)
                fprintf(out, "*");
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, true);
                fprintf(out, " = ");
                // Add compound literal cast for struct literals
                if (stmt->as.assign_stmt.value->kind == AST_EXPR_OBJECT &&
                    !stmt->as.assign_stmt.value->as.object_literal.type && target_tr) {
                    fprintf(out, "(");
                    emit_type_ref_as_c_type(ctx, target_tr, out, true);
                    fprintf(out, ")");
                }
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
            } else {
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, false);
                fprintf(out, " = ");
                // Propagate target type as expected so opt T returns get unboxed
                // (e.g. `total = total + l.get(...)` where total: Int).
                bool had_exp = ctx->has_expected_type;
                AstTypeRef saved_exp = ctx->expected_type;
                if (target_tr) { ctx->expected_type = *target_tr; ctx->has_expected_type = true; }
                // For struct literal assignments, add compound literal cast if missing
                if (stmt->as.assign_stmt.value->kind == AST_EXPR_OBJECT &&
                    !stmt->as.assign_stmt.value->as.object_literal.type && target_tr) {
                    fprintf(out, "(");
                    emit_type_ref_as_c_type(ctx, target_tr, out, true);
                    fprintf(out, ")");
                }
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
                ctx->has_expected_type = had_exp;
                ctx->expected_type = saved_exp;
            }
            fprintf(out, ";\n");
            break;
        }
        case AST_STMT_RET: {
            if (ctx->defer_stack.count > 0) {
                // Has defers — emit them before returning
                if (stmt->as.ret_stmt.values) {
                    // Store return value in temp, emit defers, then return temp
                    const char* rt = c_return_type(ctx, ctx->func_decl);
                    fprintf(out, "  %s __ret_val = ", rt);
                    const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                    bool is_ref_return = ret_type && (ret_type->is_view || ret_type->is_mod);
                    bool is_prim_ref_return = is_ref_return && is_primitive_type(get_base_type_name(ret_type));
                    if (is_prim_ref_return) {
                        fprintf(out, "("); emit_type_ref_as_c_type(ctx, ret_type, out, false);
                        fprintf(out, "){ .ptr = &"); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, true, true);
                        fprintf(out, " }");
                    } else {
                        bool needs_any_wrap = ret_type && (ret_type->is_opt || str_eq_cstr(get_base_type_name(ret_type), "Any"));
                        bool val_is_box = stmt->as.ret_stmt.values->value->kind == AST_EXPR_BOX;
                        if (needs_any_wrap && !val_is_box) { fprintf(out, "rae_any(("); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false); fprintf(out, "))"); }
                        else emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                    }
                    fprintf(out, ";\n");
                    emit_defers(ctx, 0, out);
                    fprintf(out, "  return __ret_val;\n");
                } else {
                    // Bare return
                    emit_defers(ctx, 0, out);
                    fprintf(out, "  return ");
                    if (ctx->func_decl && str_eq_cstr(ctx->func_decl->name, "main")) fprintf(out, "0");
                    else {
                        const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                        if (ret_type) {
                            if (ret_type->is_opt) fprintf(out, "rae_any_none()");
                            else emit_auto_init(ctx, ret_type, out);
                        }
                    }
                    fprintf(out, ";\n");
                }
            } else {
                // No defers — direct return
                fprintf(out, "  return ");
                if (stmt->as.ret_stmt.values) {
                    const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                    bool is_ref_return = ret_type && (ret_type->is_view || ret_type->is_mod);
                    bool is_prim_ref_return = is_ref_return && is_primitive_type(get_base_type_name(ret_type));
                    if (is_prim_ref_return) {
                        fprintf(out, "("); emit_type_ref_as_c_type(ctx, ret_type, out, false);
                        fprintf(out, "){ .ptr = &"); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, true, true);
                        fprintf(out, " }");
                    } else {
                        bool needs_any_wrap = ret_type && (ret_type->is_opt || str_eq_cstr(get_base_type_name(ret_type), "Any"));
                        bool val_is_box = stmt->as.ret_stmt.values->value->kind == AST_EXPR_BOX;
                        if (needs_any_wrap && !val_is_box) { fprintf(out, "rae_any(("); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false); fprintf(out, "))"); }
                        else emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                    }
                } else {
                    if (ctx->func_decl && str_eq_cstr(ctx->func_decl->name, "main")) fprintf(out, "0");
                    else {
                        const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                        if (ret_type) {
                            if (ret_type->is_opt) fprintf(out, "rae_any_none()");
                            else emit_auto_init(ctx, ret_type, out);
                        }
                    }
                }
                fprintf(out, ";\n");
            }
            break;
        } 
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
                    // When the pattern is `none`, the subject is a RaeAny — use the
                    // runtime tag check rather than `==` (RaeAny is a struct).
                    if (c->pattern->kind == AST_EXPR_NONE) {
                        bool saved = ctx->suppress_opt_unbox;
                        ctx->suppress_opt_unbox = true;
                        fprintf(out, "rae_any_is_none(");
                        emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
                        fprintf(out, ")");
                        ctx->suppress_opt_unbox = saved;
                    } else {
                        emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
                        fprintf(out, " == ");
                        emit_expr(ctx, c->pattern, out, PREC_LOWEST, false, false);
                    }
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
        case AST_STMT_DEFER: {
            // Push defer block onto stack — will be emitted before returns and at function end
            if (ctx->defer_stack.count < 64) {
                ctx->defer_stack.entries[ctx->defer_stack.count].block = stmt->as.defer_stmt.block;
                ctx->defer_stack.entries[ctx->defer_stack.count].scope_depth = 0;
                ctx->defer_stack.count++;
            }
            break;
        }
        default: break;
    }
    return true;
}

static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out) {
    // Emit deferred blocks in reverse order (LIFO)
    for (int i = ctx->defer_stack.count - 1; i >= min_depth; i--) {
        const AstBlock* block = ctx->defer_stack.entries[i].block;
        if (block) {
            for (AstStmt* s = block->first; s; s = s->next)
                emit_stmt(ctx, s, out);
        }
    }
    return true;
}
static void pop_defers(CFuncContext* ctx, int depth) {
    while (ctx->defer_stack.count > depth) ctx->defer_stack.count--;
}

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

  // Emit any remaining defers at function end
  if (tctx.defer_stack.count > 0) emit_defers(&tctx, 0, out);

  if (is_main) fprintf(out, "  return 0;\n}\n\n");
  else fprintf(out, "}\n\n"); 
  return true;
}

// Track emitted specialized functions to avoid redefinitions
static const char* g_emitted_spec_funcs[4096];
static size_t g_emitted_spec_func_count = 0;

static bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray) {
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
