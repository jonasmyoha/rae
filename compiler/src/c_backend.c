#include "c_backend.h"
#include "mangler.h"

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
static const AstDecl* find_enum_decl(CFuncContext* ctx, const AstModule* module, Str name);
static bool has_property(const AstProperty* props, const char* name);
static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue, bool suppress_deref);
static bool emit_function(CompilerContext* compiler_ctx, const AstModule* module, const AstFuncDecl* func, FILE* out, const struct VmRegistry* registry, bool uses_raylib);
static bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray);
static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_spawn_wrapper(CFuncContext* ctx, const AstFuncDecl* func, FILE* out);
static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out);
static void pop_defers(CFuncContext* ctx, int depth);
static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_log_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline);
static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_string_literal(FILE* out, Str literal);
static bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out, bool is_extern);
static bool emit_single_struct_def(CompilerContext* ctx, const AstModule* m, const AstDecl* d, FILE* out, Str* et, size_t* ec, bool ray);
static bool is_pointer_type(CFuncContext* ctx, Str name);
static bool is_mod_type(CFuncContext* ctx, Str name);
static bool is_generic_param(const AstIdentifierPart* params, Str name);
static Str get_local_type_name(CFuncContext* ctx, Str name);
static const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name);
static Str get_base_type_name(const AstTypeRef* type);
static bool emit_auto_init(CFuncContext* ctx, const AstTypeRef* type, FILE* out);
static bool emit_struct_auto_init(CFuncContext* ctx, const AstDecl* decl, const AstTypeRef* tr, FILE* out);
static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count, bool is_method);
static bool is_primitive_ref(CFuncContext* ctx, const AstTypeRef* tr) {
    if (!tr || !(tr->is_view || tr->is_mod)) return false;
    Str base = get_base_type_name(tr);
    if (str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float64") || 
        str_eq_cstr(base, "Bool") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32") || str_eq_cstr(base, "String") ||
        tr->is_id || tr->is_key) return true;
    return is_primitive_type(base) || (ctx->uses_raylib && is_raylib_builtin_type(base));
}

static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out, bool skip_ptr);
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool is_wildcard_pattern(const AstExpr* expr);
static bool is_string_literal_expr(const AstExpr* expr);
static bool match_cases_use_string(const AstMatchCase* cases, bool* out_use_string);
static bool match_arms_use_string(const AstMatchArm* arms, bool* out_use_string);
static bool func_has_return_value(const AstFuncDecl* func);
static const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func);
static const AstTypeRef* infer_expr_type_ref(CFuncContext* ctx, const AstExpr* expr);
static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr);

static Str get_base_type_name(const AstTypeRef* type) {
    if (!type || !type->parts) return (Str){0};
    return type->parts->text;
}

static bool has_property(const AstProperty* props, const char* name) {
  while (props) {
    if (str_eq_cstr(props->name, name)) return true;
    props = props->next;
  }
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
  
  if (str_starts_with_cstr(a, "rae_")) {
      // Strip rae_ prefix and generic suffix
      char* raw = str_to_cstr(a);
      char* start = raw + 4;
      char* end = strchr(start, '_');
      if (end) *end = '\0';
      bool match = str_eq_cstr(b, start);
      free(raw);
      if (match) return true;
  }
  if (str_starts_with_cstr(b, "rae_")) {
      // Strip rae_ prefix and generic suffix
      char* raw = str_to_cstr(b);
      char* start = raw + 4;
      char* end = strchr(start, '_');
      if (end) *end = '\0';
      bool match = str_eq_cstr(a, start);
      free(raw);
      if (match) return true;
  }
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
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_TYPE && types_match(decl->as.type_decl.name, name)) {
      return decl;
    }
  }
  for (size_t i = 0; i < g_find_module_stack_count; i++) if (g_find_module_stack[i] == module) return NULL;
  if (g_find_module_stack_count >= 64) return NULL;
  g_find_module_stack[g_find_module_stack_count++] = module;
  const AstDecl* found = NULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) {
    found = find_type_decl(ctx, imp->module, name);
    if (found) break;
  }
  g_find_module_stack_count--;
  return found;
}

static const AstDecl* find_enum_decl(CFuncContext* ctx, const AstModule* module, Str name) {
  if (ctx && ctx->compiler_ctx) {
      for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
          const AstDecl* decl = ctx->compiler_ctx->all_decls[i];
          if (decl->kind == AST_DECL_ENUM && types_match(decl->as.enum_decl.name, name)) return decl;
      }
  }
  if (!module) return NULL;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_ENUM && types_match(decl->as.enum_decl.name, name)) {
      return decl;
    }
  }
  for (size_t i = 0; i < g_find_module_stack_count; i++) if (g_find_module_stack[i] == module) return NULL;
  if (g_find_module_stack_count >= 64) return NULL;
  g_find_module_stack[g_find_module_stack_count++] = module;
  const AstDecl* found = NULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) {
    found = find_enum_decl(ctx, imp->module, name);
    if (found) break;
  }
  g_find_module_stack_count--;
  return found;
}

static void register_decl(CompilerContext* ctx, const AstDecl* decl) {
    if (!decl) return;
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        if (ctx->all_decls[i] == decl) return;
    }
    if (ctx->all_decl_count < ctx->all_decl_cap) {
        ctx->all_decls[ctx->all_decl_count++] = decl;
    }
}

static void collect_decls_from_module(CompilerContext* ctx, const AstModule* module) {
    if (!module) return;
    static const AstModule* visited[128];
    static size_t visited_count = 0;
    if (visited_count > 100) visited_count = 0; 

    for (size_t i = 0; i < visited_count; i++) {
        if (visited[i] == module) return;
    }
    if (visited_count < 128) {
        visited[visited_count++] = module;
    }

    for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
        register_decl(ctx, decl);
    }
    for (const AstImport* imp = module->imports; imp; imp = imp->next) {
        collect_decls_from_module(ctx, imp->module);
    }
}

static bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->parts && b->parts) {
        if (!str_eq(a->parts->text, b->parts->text)) return false;
    } else if (a->parts != b->parts) return false;
    
    if (a->is_opt != b->is_opt || a->is_view != b->is_view || a->is_mod != b->is_mod) return false;
    
    const AstTypeRef* arg_a = a->generic_args;
    const AstTypeRef* arg_b = b->generic_args;
    while (arg_a && arg_b) {
        if (!type_refs_equal(arg_a, arg_b)) return false;
        arg_a = arg_a->next;
        arg_b = arg_b->next;
    }
    return arg_a == arg_b;
}

static bool is_concrete_type(const AstTypeRef* type) {
    if (!type) return true;
    if (type->parts) {
        Str base = type->parts->text;
        if (base.len == 1 && base.data[0] >= 'A' && base.data[0] <= 'Z') return false;
    }
    if (type->generic_args) {
        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
            if (!is_concrete_type(a)) return false;
        }
    }
    return true;
}

static AstTypeRef* substitute_type_ref(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type) {
    if (!type) return NULL;
    if (!type->parts) return (AstTypeRef*)type;
    
    Str base = type->parts->text;
    
    // Check if it's a generic param that needs substitution
    if (generic_params && concrete_args) {
        const AstIdentifierPart* gp = generic_params;
        const AstTypeRef* arg = concrete_args;
        while (gp && arg) {
            if (str_eq(gp->text, base)) {
                AstTypeRef* result = (AstTypeRef*)arg;
                if (arg->generic_args) {
                    result = substitute_type_ref(ctx, NULL, NULL, arg);
                }
                
                // Copy flags from the original type ref (e.g. if it was mod V)
                if (type->is_view || type->is_mod) {
                    AstTypeRef* copy = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                    *copy = *result;
                    if (type->is_view) copy->is_view = true;
                    if (type->is_mod) copy->is_mod = true;
                    result = copy;
                }
                return result;
            }
            gp = gp->next; arg = arg->next;
        }
    }
    
    // If it has generic args, substitute them recursively
    if (type->generic_args) {
        AstTypeRef* new_type = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
        *new_type = *type;
        AstTypeRef* sub_args = NULL;
        AstTypeRef* last_sub = NULL;
        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
            AstTypeRef* sub = substitute_type_ref(ctx, generic_params, concrete_args, a);
            if (!sub_args) sub_args = sub;
            else last_sub->next = sub;
            last_sub = sub;
        }
        new_type->generic_args = sub_args;
        // Ensure next pointer is null for the new chain
        if (last_sub) last_sub->next = NULL;
        return new_type;
    }
    
    return (AstTypeRef*)type;
}

static AstTypeRef* infer_generic_args(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef* pattern, const AstTypeRef* concrete_type) {
    if (!func || !func->generic_params || !pattern || !concrete_type) return NULL;
    
    // Check if base types match (e.g. List vs List)
    Str pattern_base = get_base_type_name(pattern);
    Str receiver_base = get_base_type_name(concrete_type);
    if (!str_eq(pattern_base, receiver_base)) return NULL;
    
    AstTypeRef* inferred_list = NULL;
    AstTypeRef* last_inferred = NULL;
    
    // Iterate over func generic params (e.g. T)
    for (const AstIdentifierPart* gp = func->generic_params; gp; gp = gp->next) {
        AstTypeRef* match = NULL;
        
        // Find usage of T in pattern->generic_args
        const AstTypeRef* p_arg = pattern->generic_args;
        const AstTypeRef* r_arg = concrete_type->generic_args;
        
        while (p_arg && r_arg) {
            Str p_name = get_base_type_name(p_arg);
            if (str_eq(p_name, gp->text)) {
                match = (AstTypeRef*)r_arg; // Found match!
                break;
            }
            p_arg = p_arg->next;
            r_arg = r_arg->next;
        }
        
        if (match) {
            AstTypeRef* copy = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
            *copy = *match;
            copy->next = NULL;
            if (!inferred_list) inferred_list = copy;
            else last_inferred->next = copy;
            last_inferred = copy;
        } else {
            // Could not infer this param
            return NULL; 
        }
    }
    
    return inferred_list;
}

static void register_function_specialization(CompilerContext* ctx, const AstFuncDecl* decl, const AstTypeRef* concrete_args) {
    if (!decl || !concrete_args) return;
    
    // Ensure all args are concrete
    for (const AstTypeRef* arg = concrete_args; arg; arg = arg->next) {
        if (!is_concrete_type(arg)) return;
    }

    // Check if already registered
    for (size_t i = 0; i < ctx->specialized_func_count; i++) {
        if (ctx->specialized_funcs[i].decl == decl) {
            // Check if generic args match
            const AstTypeRef* a = ctx->specialized_funcs[i].concrete_args;
            const AstTypeRef* b = concrete_args;
            bool match = true;
            while (a && b) {
                if (!type_refs_equal(a, b)) { match = false; break; }
                a = a->next; b = b->next;
            }
            if (match && !a && !b) return;
        }
    }
    
    if (ctx->specialized_func_count < ctx->specialized_func_cap) {
        ctx->specialized_funcs[ctx->specialized_func_count].decl = decl;
        ctx->specialized_funcs[ctx->specialized_func_count].concrete_args = concrete_args;
        ctx->specialized_func_count++;
    }
}

static void register_generic_type(CompilerContext* ctx, const AstTypeRef* type) {
    if (!type || !type->generic_args || !is_concrete_type(type)) return;
    
    Str base = type->parts->text;
    bool is_buffer = str_eq_cstr(base, "Buffer");

    if (!is_buffer) {
        for (size_t i = 0; i < ctx->generic_type_count; i++) {
            if (type_refs_equal(ctx->generic_types[i], type)) goto scan_args;
        }
        if (ctx->generic_type_count < ctx->generic_type_cap) {
            ctx->generic_types[ctx->generic_type_count++] = type;
        }
    }

scan_args:
    // Also register any generic types used in arguments of this type
    for (const AstTypeRef* arg = type->generic_args; arg; arg = arg->next) {
        register_generic_type(ctx, arg);
    }

    // Also register any generic types used in fields of this type
    if (is_buffer || str_eq_cstr(base, "List") || str_eq_cstr(base, "Box")) {
        // Primitive generics: scanned args above
    } else {
        const AstDecl* d = NULL;
        // Search in all collected declarations first (most comprehensive)
        for (size_t i = 0; i < ctx->all_decl_count; i++) {
            const AstDecl* ad = ctx->all_decls[i];
            if (ad->kind == AST_DECL_TYPE && types_match(ad->as.type_decl.name, base)) {
                d = ad;
                break;
            }
        }
        
        // Fallback: search in core module explicitly
        if (!d && ctx->current_module) {
            d = find_type_decl(NULL, ctx->current_module, base);
        }

        if (d && d->kind == AST_DECL_TYPE) {
            for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                if (f->type) {
                    AstTypeRef* sub = substitute_type_ref(ctx, d->as.type_decl.generic_params, type->generic_args, f->type);
                    register_generic_type(ctx, sub);
                }
            }
        }
    }
}

static void collect_type_refs_expr(CompilerContext* ctx, const AstExpr* expr);
static void collect_type_refs_stmt(CompilerContext* ctx, const AstStmt* stmt);
static bool emit_rae_any_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out);

static void collect_type_refs_expr(CompilerContext* ctx, const AstExpr* expr) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_OBJECT:
            register_generic_type(ctx, expr->as.object_literal.type);
            for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) collect_type_refs_expr(ctx, f->value);
            break;
        case AST_EXPR_COLLECTION_LITERAL:
            register_generic_type(ctx, expr->as.collection.type);
            for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) {
                collect_type_refs_expr(ctx, e->value);
            }
            break;
        case AST_EXPR_CALL:
            collect_type_refs_expr(ctx, expr->as.call.callee);
            for (const AstCallArg* a = expr->as.call.args; a; a = a->next) collect_type_refs_expr(ctx, a->value);
            break;
        case AST_EXPR_BINARY:
            collect_type_refs_expr(ctx, expr->as.binary.lhs);
            collect_type_refs_expr(ctx, expr->as.binary.rhs);
            break;
        case AST_EXPR_UNARY:
            collect_type_refs_expr(ctx, expr->as.unary.operand);
            break;
        case AST_EXPR_MEMBER:
            collect_type_refs_expr(ctx, expr->as.member.object);
            break;
        case AST_EXPR_INDEX:
            collect_type_refs_expr(ctx, expr->as.index.target);
            collect_type_refs_expr(ctx, expr->as.index.index);
            break;
        default: break;
    }
}

static void collect_type_refs_stmt(CompilerContext* ctx, const AstStmt* stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case AST_STMT_LET:
            register_generic_type(ctx, stmt->as.let_stmt.type);
            collect_type_refs_expr(ctx, stmt->as.let_stmt.value);
            break;
        case AST_STMT_ASSIGN:
            collect_type_refs_expr(ctx, stmt->as.assign_stmt.target);
            collect_type_refs_expr(ctx, stmt->as.assign_stmt.value);
            break;
        case AST_STMT_EXPR:
            collect_type_refs_expr(ctx, stmt->as.expr_stmt);
            break;
        case AST_STMT_IF:
            collect_type_refs_expr(ctx, stmt->as.if_stmt.condition);
            collect_type_refs_stmt(ctx, (const AstStmt*)stmt->as.if_stmt.then_block);
            collect_type_refs_stmt(ctx, (const AstStmt*)stmt->as.if_stmt.else_block);
            break;
        case AST_STMT_LOOP:
            collect_type_refs_stmt(ctx, stmt->as.loop_stmt.init);
            collect_type_refs_expr(ctx, stmt->as.loop_stmt.condition);
            collect_type_refs_expr(ctx, stmt->as.loop_stmt.increment);
            collect_type_refs_stmt(ctx, (const AstStmt*)stmt->as.loop_stmt.body);
            break;
        default: break;
    }
}

static void discover_specializations_expr(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_CALL: {
            const AstExpr* callee = expr->as.call.callee;
            if (callee->kind == AST_EXPR_IDENT) {
                uint16_t param_count = 0;
                for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                
                const AstFuncDecl* d = NULL;
                d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, false);
                if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, true);

                if ((str_eq_cstr(callee->as.ident, "sizeof") || 
                     str_eq_cstr(callee->as.ident, "__buf_alloc") || 
                     str_eq_cstr(callee->as.ident, "__buf_free") || 
                     str_eq_cstr(callee->as.ident, "__buf_copy")) && expr->as.call.generic_args) {
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
                    register_generic_type(ctx->compiler_ctx, sub);
                }

                if (d && d->generic_params) {
                    AstTypeRef* inferred_args = NULL;
                    if (expr->as.call.generic_args) {
                        AstTypeRef* concrete_args = NULL;
                        AstTypeRef* last_arg = NULL;
                        for (const AstTypeRef* arg = expr->as.call.generic_args; arg; arg = arg->next) {
                            AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg);
                            if (!concrete_args) concrete_args = sub;
                            else last_arg->next = sub;
                            last_arg = sub;
                        }
                        inferred_args = concrete_args;
                    } else {
                        if (!inferred_args && d->params && str_eq_cstr(d->params->name, "this")) {
                            const AstCallArg* first_arg = expr->as.call.args;
                            if (first_arg) {
                                const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, first_arg->value);
                                AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->params->type, receiver_type);
                                if (inferred) inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                            }
                        }
                        
                        if (!inferred_args && ctx->generic_args) {
                            inferred_args = (AstTypeRef*)ctx->generic_args;
                        }
                        
                        // Try inferring from expected return type
                        if (!inferred_args && ctx->has_expected_type && d->returns) {
                            AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->returns->type, &ctx->expected_type);
                            if (inferred) inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                        }
                    }
                    if (inferred_args) {
                        register_function_specialization(ctx->compiler_ctx, d, inferred_args);
                    }
                }
            }
            for (const AstCallArg* a = expr->as.call.args; a; a = a->next) discover_specializations_expr(ctx, a->value);
            break;
        }
        case AST_EXPR_METHOD_CALL: {
            uint16_t param_count = 1;
            for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) param_count++;
            
            const AstFuncDecl* d = NULL;
            Str obj_type = infer_expr_type(ctx, expr->as.method_call.object);
            d = find_function_overload(ctx->module, ctx, expr->as.method_call.method_name, &obj_type, param_count, true);

            if (d && d->generic_params) {
                const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, expr->as.method_call.object);
                AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->params->type, receiver_type);
                if (inferred) {
                    AstTypeRef* concrete = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                    register_function_specialization(ctx->compiler_ctx, d, concrete);
                }
            }
            discover_specializations_expr(ctx, expr->as.method_call.object);
            for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) discover_specializations_expr(ctx, a->value);
            break;
        }
        case AST_EXPR_BINARY: discover_specializations_expr(ctx, expr->as.binary.lhs); discover_specializations_expr(ctx, expr->as.binary.rhs); break;
        case AST_EXPR_UNARY: discover_specializations_expr(ctx, expr->as.unary.operand); break;
        case AST_EXPR_MEMBER: discover_specializations_expr(ctx, expr->as.member.object); break;
        case AST_EXPR_INDEX: discover_specializations_expr(ctx, expr->as.index.target); discover_specializations_expr(ctx, expr->as.index.index); break;
        case AST_EXPR_OBJECT: for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) discover_specializations_expr(ctx, f->value); break;
        case AST_EXPR_COLLECTION_LITERAL: for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) discover_specializations_expr(ctx, e->value); break;
        case AST_EXPR_MATCH: discover_specializations_expr(ctx, expr->as.match_expr.subject); break;
        case AST_EXPR_LIST: { const AstExprList* item = expr->as.list; while (item) { discover_specializations_expr(ctx, item->value); item = item->next; } break; }
        case AST_EXPR_INTERP:
            for (const AstInterpPart* p = expr->as.interp.parts; p; p = p->next) {
                discover_specializations_expr(ctx, p->value);
            }
            break;
        default: break;
    }
}

static void discover_specializations_stmt(CFuncContext* ctx, const AstStmt* stmt) {
    for (const AstStmt* s = stmt; s; s = s->next) {
        switch (s->kind) {
            case AST_STMT_LET: 
                if (s->as.let_stmt.value) {
                    const AstTypeRef* type = s->as.let_stmt.type;
                    if (!type) {
                        type = infer_expr_type_ref(ctx, s->as.let_stmt.value);
                    }
                    
                    if (type) { ctx->expected_type = *type; ctx->has_expected_type = true; }
                    else ctx->has_expected_type = false;
                    discover_specializations_expr(ctx, s->as.let_stmt.value);
                    ctx->has_expected_type = false;
                    
                    if (ctx->local_count < 256) {
                        ctx->locals[ctx->local_count] = s->as.let_stmt.name;
                        ctx->local_type_refs[ctx->local_count] = type;
                        const char* mn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
                        ctx->local_types[ctx->local_count] = str_from_cstr(mn);
                        ctx->local_count++;
                    }
                } else {
                    if (ctx->local_count < 256) {
                        ctx->locals[ctx->local_count] = s->as.let_stmt.name;
                        ctx->local_type_refs[ctx->local_count] = s->as.let_stmt.type;
                        const char* mn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, s->as.let_stmt.type);
                        ctx->local_types[ctx->local_count] = str_from_cstr(mn);
                        ctx->local_count++;
                    }
                }
                break;
            case AST_STMT_ASSIGN: 
                discover_specializations_expr(ctx, s->as.assign_stmt.target); 
                {
                    const AstTypeRef* tr = infer_expr_type_ref(ctx, s->as.assign_stmt.target);
                    if (tr) { ctx->expected_type = *tr; ctx->has_expected_type = true; }
                    else ctx->has_expected_type = false;
                    discover_specializations_expr(ctx, s->as.assign_stmt.value); 
                    ctx->has_expected_type = false;
                    
                    // If it's a bind (destructuring), we need to register locals just like emit_stmt does
                    if (s->as.assign_stmt.is_bind) {
                        if (s->as.assign_stmt.target->kind == AST_EXPR_IDENT) {
                            if (ctx->local_count < 256) {
                                ctx->locals[ctx->local_count] = s->as.assign_stmt.target->as.ident;
                                AstTypeRef* t = infer_expr_type_ref(ctx, s->as.assign_stmt.value);
                                ctx->local_type_refs[ctx->local_count] = t;
                                const char* mn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, t);
                                ctx->local_types[ctx->local_count] = str_from_cstr(mn);
                                ctx->local_count++;
                            }
                        }
                    }
                }
                break;
            case AST_STMT_EXPR: discover_specializations_expr(ctx, s->as.expr_stmt); break;
            case AST_STMT_RET: { AstReturnArg* v = s->as.ret_stmt.values; while (v) { discover_specializations_expr(ctx, v->value); v = v->next; } break; }
            case AST_STMT_IF: discover_specializations_expr(ctx, s->as.if_stmt.condition); if (s->as.if_stmt.then_block) discover_specializations_stmt(ctx, s->as.if_stmt.then_block->first); if (s->as.if_stmt.else_block) discover_specializations_stmt(ctx, s->as.if_stmt.else_block->first); break;
            case AST_STMT_LOOP: if (s->as.loop_stmt.init) discover_specializations_stmt(ctx, s->as.loop_stmt.init); if (s->as.loop_stmt.condition) discover_specializations_expr(ctx, s->as.loop_stmt.condition); if (s->as.loop_stmt.increment) discover_specializations_expr(ctx, s->as.loop_stmt.increment); if (s->as.loop_stmt.body) discover_specializations_stmt(ctx, s->as.loop_stmt.body->first); break;
            case AST_STMT_DEFER: if (s->as.defer_stmt.block) discover_specializations_stmt(ctx, s->as.defer_stmt.block->first); break;
            default: break;
        }
    }
}

static void discover_specializations_module(CompilerContext* ctx, const AstModule* module) {
    // Phase 1: Discover from non-generic functions and globals
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        const AstDecl* d = ctx->all_decls[i];
        if (d->kind == AST_DECL_GLOBAL_LET) {
            register_generic_type(ctx, d->as.let_decl.type);
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module};
            discover_specializations_expr(&fctx, d->as.let_decl.value);
        } else if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params) {
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = &d->as.func_decl};
            for (const AstParam* p = d->as.func_decl.params; p; p = p->next) {
                if (fctx.local_count < 256) {
                    fctx.locals[fctx.local_count] = p->name;
                    fctx.local_type_refs[fctx.local_count] = p->type;
                    fctx.local_count++;
                }
            }
            if (d->as.func_decl.body) discover_specializations_stmt(&fctx, d->as.func_decl.body->first);
        }
    }

    // Phase 2: Discover from specialized functions recursively
    size_t discovered = 0;
    while (discovered < ctx->specialized_func_count) {
        size_t limit = ctx->specialized_func_count;
        for (size_t i = discovered; i < limit; i++) {
            const AstFuncDecl* f = ctx->specialized_funcs[i].decl;
            const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = f, .generic_params = f->generic_params, .generic_args = args};
            for (const AstParam* p = f->params; p; p = p->next) {
                if (fctx.local_count < 256) {
                    fctx.locals[fctx.local_count] = p->name;
                    fctx.local_type_refs[fctx.local_count] = substitute_type_ref(ctx, f->generic_params, args, p->type);
                    fctx.local_count++;
                }
            }
            if (f->body) discover_specializations_stmt(&fctx, f->body->first);
        }
        discovered = limit;
    }
}

static void collect_type_refs_module(CompilerContext* ctx) {
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        const AstDecl* d = ctx->all_decls[i];
        if (d->kind == AST_DECL_FUNC) {
            for (const AstParam* p = d->as.func_decl.params; p; p = p->next) register_generic_type(ctx, p->type);
            if (d->as.func_decl.returns) register_generic_type(ctx, d->as.func_decl.returns->type);
            if (d->as.func_decl.body) {
                for (const AstStmt* s = d->as.func_decl.body->first; s; s = s->next) collect_type_refs_stmt(ctx, s);
            }
        } else if (d->kind == AST_DECL_TYPE) {
            for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) register_generic_type(ctx, f->type);
        } else if (d->kind == AST_DECL_GLOBAL_LET) {
            register_generic_type(ctx, d->as.let_decl.type);
            collect_type_refs_expr(ctx, d->as.let_decl.value);
        }
    }
    
    // Recursive discovery: keep scanning as long as new generic types or specialized functions are found
    size_t last_generic_count = 0;
    size_t last_func_count = 0;
    do {
        last_generic_count = ctx->generic_type_count;
        last_func_count = ctx->specialized_func_count;
        discover_specializations_module(ctx, ctx->current_module);
    } while (ctx->generic_type_count > last_generic_count || ctx->specialized_func_count > last_func_count);
}

static bool emit_specialized_struct_def(CompilerContext* ctx, const AstModule* module, const AstTypeRef* type, FILE* out, bool uses_raylib) {
    (void)uses_raylib;
    if (!type || !type->generic_args) return true;
    for (size_t i = 0; i < ctx->emitted_generic_type_count; i++) {
        if (type_refs_equal(ctx->emitted_generic_types[i], type)) return true;
    }
    if (ctx->emitted_generic_type_count < ctx->emitted_generic_type_cap) {
        ctx->emitted_generic_types[ctx->emitted_generic_type_count++] = type;
    }

    Str base = type->parts->text;
    const char* mangled = rae_mangle_type(ctx, NULL, type);
    CFuncContext dummy_ctx = {0}; dummy_ctx.compiler_ctx = ctx; dummy_ctx.module = module; dummy_ctx.uses_raylib = uses_raylib;

    if (str_eq_cstr(base, "List") || str_eq_cstr(base, "Box")) {
        fprintf(out, "typedef struct %s %s;\nstruct %s {\n", mangled, mangled, mangled);
        fprintf(out, "  ");
        if (str_eq_cstr(base, "Box")) {
            emit_type_ref_as_c_type(&dummy_ctx, type->generic_args, out, false);
            fprintf(out, " value;\n};\n\n");
        } else {
            // Buffer(T) correctly emits T*
            // We want 'T* data;'
            emit_type_ref_as_c_type(&dummy_ctx, type->generic_args, out, false);
            fprintf(out, "* data;\n  int64_t length;\n  int64_t capacity;\n};\n\n");
        }
        return true;
    }

    const AstDecl* d = NULL;
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        if (ctx->all_decls[i]->kind == AST_DECL_TYPE && types_match(ctx->all_decls[i]->as.type_decl.name, base)) {
            d = ctx->all_decls[i];
            break;
        }
    }
    if (!d || d->kind != AST_DECL_TYPE) return true;

    fprintf(out, "typedef struct %s %s;\nstruct %s {\n", mangled, mangled, mangled);
    CFuncContext field_ctx = {0};
    field_ctx.compiler_ctx = ctx;
    field_ctx.module = module;
    field_ctx.uses_raylib = uses_raylib;
    field_ctx.generic_params = d->as.type_decl.generic_params;
    field_ctx.generic_args = type->generic_args;

    for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
        fprintf(out, "  ");
        emit_type_ref_as_c_type(&field_ctx, f->type, out, false);
        fprintf(out, " %.*s;\n", (int)f->name.len, f->name.data);
    }
    fprintf(out, "};\n\n");
    return true;
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
      case '"': fprintf(out, "\\\""); break;
      case '\\': fprintf(out, "\\\\"); break;
      case '\n': fprintf(out, "\\n"); break;
      case '\r': fprintf(out, "\\r"); break;
      case '\t': fprintf(out, "\\t"); break;
      default: {
          if ((unsigned char)c < 32 || (unsigned char)c > 126) {
              fprintf(out, "\\x%02x", (unsigned char)c);
          } else {
              fputc(c, out);
          }
          break;
      }
    }
  }
  fprintf(out, "\", %lld}", (long long)literal.len);
  return true;
}

static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out, bool skip_ptr) {
  if (!type || !type->parts) { fprintf(out, "int64_t"); return true; }
  bool is_ptr = (type->is_view || type->is_mod) && !skip_ptr;
  if (type->is_id) { fprintf(out, "int64_t"); if (is_ptr) fprintf(out, "*"); return true; }
  if (type->is_key) { fprintf(out, "const char*"); if (is_ptr) fprintf(out, "*"); return true; }
  if (type->is_opt) { fprintf(out, "RaeAny"); if (is_ptr) fprintf(out, "*"); return true; }

  Str base = type->parts->text;
  bool is_mod = type->is_mod;

  if (str_eq_cstr(base, "Int64")) { 
      if (is_ptr) fprintf(out, "rae_%s_Int", is_mod ? "Mod" : "View");
      else fprintf(out, "int64_t"); 
      return true; 
  }
  else if (str_eq_cstr(base, "Float64")) { 
      if (is_ptr) fprintf(out, "rae_%s_Float", is_mod ? "Mod" : "View");
      else fprintf(out, "double"); 
      return true; 
  }
  else if (str_eq_cstr(base, "Bool")) { 
      if (is_ptr) fprintf(out, "rae_%s_Bool", is_mod ? "Mod" : "View");
      else fprintf(out, "rae_Bool"); 
      return true; 
  }
  else if (str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) { 
      if (is_ptr) fprintf(out, "rae_%s_Char%s", is_mod ? "Mod" : "View", str_eq_cstr(base, "Char32") ? "32" : "");
      else fprintf(out, "uint32_t"); 
      return true; 
  }
  else if (str_eq_cstr(base, "String")) { 
      if (is_ptr) fprintf(out, "rae_%s_String", is_mod ? "Mod" : "View");
      else fprintf(out, "rae_String"); 
      return true; 
  }
  else if (str_eq_cstr(base, "Any")) { 
      if (is_ptr) fprintf(out, "RaeAny*"); 
      else fprintf(out, "RaeAny"); 
      return true; 
  }
  else if (str_eq_cstr(base, "Buffer") && type->generic_args) {
        Str arg_base = get_base_type_name(type->generic_args);
        if (str_eq_cstr(arg_base, "Any") || arg_base.len == 0) {
            fprintf(out, "void*");
            return true;
        }
        emit_type_ref_as_c_type(ctx, type->generic_args, out, false);
        fprintf(out, "*");
        return true; 
  }

  if (ctx && ctx->generic_params && ctx->generic_args) {
      const AstIdentifierPart* gp = ctx->generic_params;
      const AstTypeRef* arg = ctx->generic_args;
      while (gp && arg) {
          if (str_eq(gp->text, base)) {
              emit_type_ref_as_c_type(ctx, arg, out, false);
              return true;
          }
          gp = gp->next; arg = arg->next;
      }
  }

  const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
  
  if (ctx && ctx->uses_raylib && is_raylib_builtin_type(base)) {
      if (!find_type_decl(ctx, ctx->module, base)) {
          fprintf(out, "%.*s", (int)base.len, base.data);
      } else {
          fprintf(out, "rae_%.*s", (int)base.len, base.data);
      }
  } else {
      fprintf(out, "%s", mangled);
  }

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
        emit_type_ref_as_c_type(&p_ctx, &p_type, out, false);
        fprintf(out, " %.*s", (int)p->name.len, p->name.data);
    }
    index++;
  }
  if (index == 0) fprintf(out, "void");
  return true;
}

static const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func) {
  if (str_eq_cstr(func->name, "rae_ext_rae_buf_alloc") || str_eq_cstr(func->name, "__buf_alloc") ||
      str_eq_cstr(func->name, "rae_ext_rae_buf_resize") || str_eq_cstr(func->name, "__buf_resize") ||
      str_eq_cstr(func->name, "rae_ext_rae_str_to_cstr") || str_eq_cstr(func->name, "toCStr")) {
      return "void*";
  }
  if (str_eq_cstr(func->name, "rae_ext_rae_buf_free") || str_eq_cstr(func->name, "__buf_free") ||
      str_eq_cstr(func->name, "rae_ext_rae_buf_copy") || str_eq_cstr(func->name, "__buf_copy")) {
      return "void";
  }
  if (func->returns && func->returns->type) {
    AstTypeRef* tr = func->returns->type;
    if (tr->is_opt) return "RaeAny";
    bool is_view = tr->is_view, is_mod = tr->is_mod, is_ptr = is_view || is_mod;
    if (tr->is_id) return "int64_t";
    if (tr->is_key) return "const char*";
    
    Str base = get_base_type_name(tr);
    if (tr->is_opt && !tr->generic_args && !(ctx && ctx->generic_params)) return "RaeAny";

    if (tr->generic_args || (ctx && ctx->generic_params)) {
        const char* m = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
        // If it's still 'rae_Any' or starts with 'opt', map to concrete if possible
        if (strcmp(m, "RaeAny") == 0) return "RaeAny";
        
        // Handle substituted primitive names in specialized return types
        if (strcmp(m, "rae_Int64") == 0) return "int64_t";
        if (strcmp(m, "rae_Int32") == 0) return "int32_t";
        if (strcmp(m, "rae_UInt64") == 0) return "uint64_t";
        if (strcmp(m, "rae_UInt32") == 0) return "uint32_t";
        if (strcmp(m, "rae_Float64") == 0) return "double";
        if (strcmp(m, "rae_Float32") == 0) return "float";
        if (strcmp(m, "rae_Bool") == 0) return "rae_Bool";
        if (strcmp(m, "rae_Char") == 0 || strcmp(m, "rae_Char32") == 0 || strcmp(m, "uint32_t") == 0) return "uint32_t";
        if (strcmp(m, "rae_String") == 0) return "rae_String";
        
        if (is_ptr) { char* b = malloc(strlen(m) + 16); sprintf(b, "%s%s*", is_view ? "const " : "", m); return b; }
        return m;
    }
    const char* mapped = map_rae_type_to_c(base);
    if (mapped) {
        if (is_ptr) { char* b = malloc(strlen(mapped) + 16); sprintf(b, "%s%s*", (is_view && strcmp(mapped, "const char*") != 0) ? "const " : "", mapped); return b; }
        return mapped;
    }
    char* raw = str_to_cstr(base); char* res = malloc(strlen(raw) + 8); sprintf(res, "rae_%s", raw); free(raw);
    if (is_ptr) { char* b = malloc(strlen(res) + 16); sprintf(b, "%s%s*", is_view ? "const " : "", res); free(res); return b; }
    return res;
  }
  return func_has_return_value(func) ? "int64_t" : "void";
}

static bool func_has_return_value(const AstFuncDecl* func) { return func->returns != NULL; }
static Str get_local_type_name(CFuncContext* ctx, Str name) {
    for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_types[i];
    return (Str){0};
}
static const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name) {
    for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_type_refs[i];
    return NULL;
}
static bool emit_auto_init(CFuncContext* ctx, const AstTypeRef* type, FILE* out) {
    if (!type) {
        fprintf(out, "{0}");
        return true;
    }
    
    if (type->is_opt) {
        fprintf(out, "rae_any_none()");
        return true;
    }
    
    Str base = get_base_type_name(type);
    if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int32") || str_eq_cstr(base, "UInt64") || str_eq_cstr(base, "UInt32") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) {
        fprintf(out, "0");
    } else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float") || str_eq_cstr(base, "Float32")) {
        fprintf(out, "0.0");
    } else if (str_eq_cstr(base, "Bool")) {
        fprintf(out, "false");
    } else if (str_eq_cstr(base, "String")) {
        fprintf(out, "(rae_String){0}");
    } else {
        const AstDecl* d = find_type_decl(ctx, ctx->module, base);
        if (d && d->kind == AST_DECL_TYPE) {
            emit_struct_auto_init(ctx, d, type, out);
        } else {
            fprintf(out, "{0}");
        }
    }
    return true;
}

static bool emit_struct_auto_init(CFuncContext* ctx, const AstDecl* decl, const AstTypeRef* tr, FILE* out) {
    fprintf(out, "{ ");
    for (const AstTypeField* f = decl->as.type_decl.fields; f; f = f->next) {
        fprintf(out, ".%.*s = ", (int)f->name.len, f->name.data);
        const AstTypeRef* field_tr = substitute_type_ref(ctx->compiler_ctx, decl->as.type_decl.generic_params, tr->generic_args, f->type);
        emit_auto_init(ctx, field_tr, out);
        if (f->next) fprintf(out, ", ");
    }
    fprintf(out, " }");
    return true;
}

static bool is_pointer_type(CFuncContext* ctx, Str name) {
    for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_is_ptr[i];
    return false;
}

static bool is_mod_type(CFuncContext* ctx, Str name) {
    for (int i = (int)ctx->local_count - 1; i >= 0; i--) if (str_eq(ctx->locals[i], name)) return ctx->local_is_mod[i];
    return false;
}

static bool is_generic_param(const AstIdentifierPart* params, Str name) {
    const AstIdentifierPart* p = params;
    while (p) {
        if (str_eq(p->text, name)) return true;
        p = p->next;
    }
    return false;
}

static const AstTypeRef* infer_expr_type_ref(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return NULL;
    if (expr->kind == AST_EXPR_CALL && expr->as.call.callee->kind == AST_EXPR_IDENT) {
        Str name = expr->as.call.callee->as.ident;
        if (str_eq_cstr(name, "rae_ext_rae_buf_alloc") || str_eq_cstr(name, "__buf_alloc") ||
            str_eq_cstr(name, "rae_ext_rae_buf_resize") || str_eq_cstr(name, "__buf_resize")) {
            AstTypeRef* buf_tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
            buf_tr->parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart));
            buf_tr->parts->text = str_from_cstr("Buffer");
            buf_tr->generic_args = (AstTypeRef*)expr->as.call.generic_args;
            return buf_tr;
        }
    }
    switch (expr->kind) {
        case AST_EXPR_IDENT: return get_local_type_ref(ctx, expr->as.ident);
        case AST_EXPR_INTEGER: {
            AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
            *tr = (AstTypeRef){.parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart))};
            tr->parts->text = str_from_cstr("Int64");
            return tr;
        }
        case AST_EXPR_FLOAT: {
            AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
            *tr = (AstTypeRef){.parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart))};
            tr->parts->text = str_from_cstr("Float64");
            return tr;
        }
        case AST_EXPR_BOOL: {
            AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
            *tr = (AstTypeRef){.parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart))};
            tr->parts->text = str_from_cstr("Bool");
            return tr;
        }
        case AST_EXPR_STRING: {
            AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
            *tr = (AstTypeRef){.parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart))};
            tr->parts->text = str_from_cstr("String");
            return tr;
        }
        case AST_EXPR_MEMBER: {
            const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.member.object);
            Str obj_name = get_base_type_name(obj_tr);
            if (obj_name.len == 0) obj_name = infer_expr_type(ctx, expr->as.member.object);
            
            const AstDecl* d = find_type_decl(ctx, ctx->module, obj_name);
            if (d && d->kind == AST_DECL_TYPE) {
                for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                    if (str_eq(f->name, expr->as.member.member)) {
                        const AstTypeRef* concrete_args = (obj_tr && obj_tr->generic_args) ? obj_tr->generic_args : ctx->generic_args;
                        return substitute_type_ref(ctx->compiler_ctx, d->as.type_decl.generic_params, concrete_args, f->type);
                    }
                }
            }
            break;
        }
        case AST_EXPR_INDEX: {
            const AstTypeRef* tr = infer_expr_type_ref(ctx, expr->as.index.target);
            if (tr) {
                Str base = get_base_type_name(tr);
                if ((str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List")) && tr->generic_args) return tr->generic_args;
            }
            break;
        }
        case AST_EXPR_CALL: {
            if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
                uint16_t param_count = 0;
                for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                
                Str* param_types = NULL;
                if (param_count > 0) {
                    param_types = calloc(param_count, sizeof(Str));
                    const AstCallArg* a = expr->as.call.args;
                    for (uint16_t i = 0; i < param_count; i++) {
                        param_types[i] = infer_expr_type(ctx, a->value);
                        a = a->next;
                    }
                }

                const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, expr->as.call.callee->as.ident, param_types, param_count, false);
                if (!fd) fd = find_function_overload(ctx->module, ctx, expr->as.call.callee->as.ident, param_types, param_count, true);
                
                if (param_types) free(param_types);
                if (fd && fd->returns) {
                    AstTypeRef* inferred_args = NULL;
                    if (fd->generic_params && fd->params && str_eq_cstr(fd->params->name, "this")) {
                        const AstCallArg* first_arg = expr->as.call.args;
                        if (first_arg) {
                            const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, first_arg->value);
                            inferred_args = infer_generic_args(ctx->compiler_ctx, fd, fd->params->type, receiver_type);
                        }
                    }
                    
                    const AstTypeRef* base_ret = fd->returns->type;
                    AstTypeRef* result = (AstTypeRef*)base_ret;
                    if (inferred_args) result = substitute_type_ref(ctx->compiler_ctx, fd->generic_params, inferred_args, base_ret);
                    
                    if (base_ret->is_opt && !result->is_opt) {
                        AstTypeRef* copy = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
                        *copy = *result;
                        copy->is_opt = true;
                        result = copy;
                    }
                    return result;
                }
            } else if (expr->as.call.callee->kind == AST_EXPR_MEMBER) {
                Str member = expr->as.call.callee->as.member.member;
                Str obj_name = infer_expr_type(ctx, expr->as.call.callee->as.member.object);
                uint16_t param_count = 1; 
                for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, member, &obj_name, param_count, true);
                if (fd && fd->returns) return fd->returns->type;
            }
            break;
        }
        case AST_EXPR_METHOD_CALL: {
            Str member = expr->as.method_call.method_name;
            if (str_eq_cstr(member, "toString")) {
                AstTypeRef* tr = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
                *tr = (AstTypeRef){.parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart))};
                tr->parts->text = str_from_cstr("String");
                return tr;
            }
            Str obj_name = infer_expr_type(ctx, expr->as.method_call.object);
            uint16_t param_count = 1;
            for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) param_count++;
            
            Str* param_types = calloc(param_count, sizeof(Str));
            param_types[0] = obj_name;
            const AstCallArg* a = expr->as.method_call.args;
            for (uint16_t i = 1; i < param_count; i++) {
                param_types[i] = infer_expr_type(ctx, a->value);
                a = a->next;
            }

            const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, member, param_types, param_count, true);
            free(param_types);
            if (fd && fd->returns) {
                AstTypeRef* inferred_args = NULL;
                if (fd->generic_params) {
                    const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, expr->as.method_call.object);
                    inferred_args = infer_generic_args(ctx->compiler_ctx, fd, fd->params->type, receiver_type);
                }
                
                const AstTypeRef* base_ret = fd->returns->type;
                AstTypeRef* result = (AstTypeRef*)base_ret;
                if (inferred_args) result = substitute_type_ref(ctx->compiler_ctx, fd->generic_params, inferred_args, base_ret);
                
                if (base_ret->is_opt && !result->is_opt) {
                    AstTypeRef* copy = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
                    *copy = *result;
                    copy->is_opt = true;
                    result = copy;
                }
                return result;
            }
            break;
        }
        default: break;
    }
    return NULL;
}

static const AstExpr* g_infer_expr_stack[64];
static size_t g_infer_expr_stack_count = 0;
static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return (Str){0};
    if (expr->kind == AST_EXPR_CALL && expr->as.call.callee->kind == AST_EXPR_IDENT) {
        Str name = expr->as.call.callee->as.ident;
        if (str_eq_cstr(name, "rae_ext_rae_buf_alloc") || str_eq_cstr(name, "__buf_alloc") ||
            str_eq_cstr(name, "rae_ext_rae_buf_resize") || str_eq_cstr(name, "__buf_resize")) {
            return str_from_cstr("Buffer");
        }
    }
    for (size_t i = 0; i < g_infer_expr_stack_count; i++) if (g_infer_expr_stack[i] == expr) return (Str){0};
    if (g_infer_expr_stack_count >= 64) return (Str){0};
    g_infer_expr_stack[g_infer_expr_stack_count++] = expr;
    Str res = {0};
    switch (expr->kind) {
        case AST_EXPR_IDENT: res = get_local_type_name(ctx, expr->as.ident); break;
        case AST_EXPR_INTEGER: res = str_from_cstr("Int64"); break;
        case AST_EXPR_FLOAT: res = str_from_cstr("Float64"); break;
        case AST_EXPR_BOOL: res = str_from_cstr("Bool"); break;
        case AST_EXPR_STRING: res = str_from_cstr("String"); break;
        case AST_EXPR_CHAR: res = str_from_cstr("Char"); break;
        case AST_EXPR_BINARY: {
            if (expr->as.binary.op >= AST_BIN_LT && expr->as.binary.op <= AST_BIN_OR) res = str_from_cstr("Bool");
            else res = infer_expr_type(ctx, expr->as.binary.lhs);
            break;
        }
        case AST_EXPR_UNARY: {
            if (expr->as.unary.op == AST_UNARY_NOT) res = str_from_cstr("Bool");
            else res = infer_expr_type(ctx, expr->as.unary.operand);
            break;
        }
        case AST_EXPR_CALL: {
            if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
                uint16_t param_count = 0;
                for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                
                Str* param_types = NULL;
                if (param_count > 0) {
                    param_types = calloc(param_count, sizeof(Str));
                    const AstCallArg* a = expr->as.call.args;
                    for (uint16_t i = 0; i < param_count; i++) {
                        param_types[i] = infer_expr_type(ctx, a->value);
                        a = a->next;
                    }
                }

                const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, expr->as.call.callee->as.ident, param_types, param_count, false);
                if (!fd) fd = find_function_overload(ctx->module, ctx, expr->as.call.callee->as.ident, param_types, param_count, true);
                
                if (param_types) free(param_types);
                if (fd && fd->returns) {
                    const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, fd->returns->type);
                    res = str_from_cstr(tn);
                }
            } else if (expr->as.call.callee->kind == AST_EXPR_MEMBER) {
                Str member = expr->as.call.callee->as.member.member;
                Str obj_name = infer_expr_type(ctx, expr->as.call.callee->as.member.object);
                uint16_t param_count = 1;
                for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, member, &obj_name, param_count, true);
                if (fd && fd->returns) {
                    const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, fd->returns->type);
                    res = str_from_cstr(tn);
                }
            }
            break;
        }
        case AST_EXPR_INDEX: {
            const AstTypeRef* tr = infer_expr_type_ref(ctx, expr->as.index.target);
            if (tr) {
                if ((str_eq_cstr(tr->parts->text, "Buffer") || str_eq_cstr(tr->parts->text, "List")) && tr->generic_args) {
                    const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr->generic_args);
                    res = str_from_cstr(tn);
                }
            }
            break;
        }
        case AST_EXPR_MEMBER: {
            if (expr->as.member.object->kind == AST_EXPR_IDENT) {
                const AstDecl* ed = find_enum_decl(ctx, ctx->module, expr->as.member.object->as.ident);
                if (ed) { res = ed->as.enum_decl.name; break; }
            }
            const AstTypeRef* tr = infer_expr_type_ref(ctx, expr); 
            if (tr) {
                const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
                res = str_from_cstr(tn);
            }
            break;
        }
        case AST_EXPR_METHOD_CALL: {
            Str member = expr->as.method_call.method_name;
            if (str_eq_cstr(member, "toString")) {
                res = str_from_cstr("String");
                break;
            }
            Str obj_name = infer_expr_type(ctx, expr->as.method_call.object);
            uint16_t param_count = 1;
            for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) param_count++;
            
            Str* param_types = calloc(param_count, sizeof(Str));
            param_types[0] = obj_name;
            const AstCallArg* a = expr->as.method_call.args;
            for (uint16_t i = 1; i < param_count; i++) {
                param_types[i] = infer_expr_type(ctx, a->value);
                a = a->next;
            }

            const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, member, param_types, param_count, true);
            free(param_types);
            if (fd && fd->returns) {
                const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, fd->returns->type);
                res = str_from_cstr(tn);
            }
            break;
        }
        default: break;
    }
    g_infer_expr_stack_count--; return res;
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
        bool is_ptr = is_pointer_type(ctx, expr->as.ident);
        const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
        bool is_prim_ref = is_primitive_ref(ctx, tr);

        if (is_prim_ref && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s.ptr)", (int)expr->as.ident.len, expr->as.ident.data);
        } else if (is_ptr && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s)", (int)expr->as.ident.len, expr->as.ident.data);
        } else {
            fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
        }
        break;
    }
    case AST_EXPR_NONE: {
        if (ctx->has_expected_type) {
            Str base = get_base_type_name(&ctx->expected_type);
            if (str_eq_cstr(base, "String")) {
                fprintf(out, "(rae_String){0}");
                break;
            }
        }
        fprintf(out, "rae_any_none()");
        break;
    }
    case AST_EXPR_BINARY: {
      int prec = binary_op_precedence(expr->as.binary.op);
      bool is_bool_op = expr->as.binary.op >= AST_BIN_LT && expr->as.binary.op <= AST_BIN_OR;
      
      Str lhs_type = infer_expr_type(ctx, expr->as.binary.lhs);
      Str rhs_type = infer_expr_type(ctx, expr->as.binary.rhs);
      bool is_str_cmp = (types_match(lhs_type, str_from_cstr("String")) && types_match(rhs_type, str_from_cstr("String"))) && (expr->as.binary.op == AST_BIN_IS);

      if (is_str_cmp) {
          fprintf(out, "(bool)rae_ext_rae_str_eq(");
          emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
          fprintf(out, ", ");
          emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
          fprintf(out, ")");
          break;
      }

      if (is_bool_op) fprintf(out, "(bool)(");
      if (prec < parent_prec) fprintf(out, "(");
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
      if (prec < parent_prec) fprintf(out, ")");
      if (is_bool_op) fprintf(out, ")");
      break;
    }
    case AST_EXPR_UNARY: {
        switch (expr->as.unary.op) {
            case AST_UNARY_NOT: fprintf(out, "((bool)!("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, "))"); break;
            case AST_UNARY_NEG: fprintf(out, "-("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, ")"); break;
            case AST_UNARY_PRE_INC: fprintf(out, "++("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, ")"); break;
            case AST_UNARY_PRE_DEC: fprintf(out, "--("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, ")"); break;
            case AST_UNARY_POST_INC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "++"); break;
            case AST_UNARY_POST_DEC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "--"); break;
            case AST_UNARY_VIEW:
            case AST_UNARY_MOD: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); break;
            default: break;
        }
        break;
    }
    case AST_EXPR_CALL: {
        const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
        bool returns_any = tr && tr->is_opt;
        bool needs_unwrap = returns_any && ctx->has_expected_type;
        
        if (needs_unwrap) {
            Str base = get_base_type_name(&ctx->expected_type);
            fprintf(out, "(");
            emit_call_expr(ctx, expr, out);
            if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, ").as.i");
            else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) fprintf(out, ").as.f");
            else if (str_eq_cstr(base, "Bool")) fprintf(out, ").as.b");
            else if (str_eq_cstr(base, "String")) fprintf(out, ").as.s");
            else fprintf(out, ").as.ptr");
        } else {
            emit_call_expr(ctx, expr, out);
        }
        break;
    }
    case AST_EXPR_MEMBER: {
        const AstExpr* obj = expr->as.member.object;
        if (obj->kind == AST_EXPR_IDENT) {
            const AstDecl* ed = find_enum_decl(ctx, ctx->module, obj->as.ident);
            if (ed) {
                fprintf(out, "%.*s_%.*s", (int)obj->as.ident.len, obj->as.ident.data, (int)expr->as.member.member.len, expr->as.member.member.data);
                break;
            }
        }
        
        const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, obj);
        bool obj_is_ref = obj_tr && (obj_tr->is_view || obj_tr->is_mod);
        bool obj_is_ptr_var = obj->kind == AST_EXPR_IDENT && is_pointer_type(ctx, obj->as.ident);
        
        bool is_primitive_ref = false;
        if (obj_is_ref) {
            Str base = get_base_type_name(obj_tr);
            if (is_primitive_type(base) || str_eq_cstr(base, "String") || (ctx->uses_raylib && is_raylib_builtin_type(base))) {
                is_primitive_ref = true;
            }
        }

        if (is_primitive_ref) {
            emit_expr(ctx, obj, out, PREC_CALL, false, false);
            fprintf(out, ".ptr->%.*s", (int)expr->as.member.member.len, expr->as.member.member.data);
        } else if (obj_is_ptr_var || obj_is_ref) {
            emit_expr(ctx, obj, out, PREC_CALL, true, false); // Pass true for is_lvalue to avoid (*ptr)
            fprintf(out, "->%.*s", (int)expr->as.member.member.len, expr->as.member.member.data);
        } else {
            emit_expr(ctx, obj, out, PREC_CALL, false, false);
            fprintf(out, ".%.*s", (int)expr->as.member.member.len, expr->as.member.member.data);
        }
        break;
    }
    case AST_EXPR_OBJECT: {
      const AstTypeRef* type = expr->as.object_literal.type ? expr->as.object_literal.type : (ctx->has_expected_type ? &ctx->expected_type : NULL);
      
      // Fallback: try to infer from expression itself if no context type
      if (!type) type = infer_expr_type_ref(ctx, expr);

      if (type) {
          fprintf(out, "(");
          emit_type_ref_as_c_type(ctx, type, out, false);
          fprintf(out, ")");
      }
      fprintf(out, "{ ");
      for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
        fprintf(out, ".%.*s = ", (int)f->name.len, f->name.data);
        
        const AstTypeRef* field_type = NULL;
        AstTypeRef field_tr = {0};
        bool has_field_tr = false;

        if (type && type->parts) {
            Str base = type->parts->text;
            const AstDecl* td = find_type_decl(ctx, ctx->module, base);
            if (td && td->kind == AST_DECL_TYPE) {
                for (const AstTypeField* tf = td->as.type_decl.fields; tf; tf = tf->next) {
                    if (str_eq(tf->name, f->name)) {
                        AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, td->as.type_decl.generic_params, type->generic_args, tf->type);
                        if (sub) { field_tr = *sub; has_field_tr = true; field_type = &field_tr; }
                        register_generic_type(ctx->compiler_ctx, sub);
                        break;
                    }
                }
            }
        }
        
        AstTypeRef old_expected = ctx->expected_type;
        bool old_has = ctx->has_expected_type;
        if (has_field_tr) { ctx->expected_type = field_tr; ctx->has_expected_type = true; }
        else ctx->has_expected_type = false;

        emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
        ctx->expected_type = old_expected;
        ctx->has_expected_type = old_has;
        
        if (f->next) fprintf(out, ", ");
      }
      fprintf(out, " }");
      break;
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      const AstTypeRef* type = expr->as.collection.type ? expr->as.collection.type : (ctx->has_expected_type ? &ctx->expected_type : NULL);
      if (!type || !type->parts) { fprintf(out, "0"); break; }
      
      Str base = type->parts->text;
      bool is_buffer = str_eq_cstr(base, "Buffer");
      bool is_list = str_eq_cstr(base, "List");
      
      if (is_buffer) {
          // Emit as C array literal
          fprintf(out, "LIFT_TO_TEMP(");
          emit_type_ref_as_c_type(ctx, type->generic_args, out, false);
          fprintf(out, "[], {");
          for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) {
              emit_expr(ctx, e->value, out, PREC_LOWEST, false, false);
              if (e->next) fprintf(out, ", ");
          }
          fprintf(out, "})");
      } else if (is_list) {
          fprintf(out, "(rae_%.*s_", (int)base.len, base.data);
          if (type->generic_args) {
              const char* mangled_arg = rae_mangle_type(ctx->compiler_ctx, ctx->generic_params, type->generic_args);
              fprintf(out, "%s", mangled_arg);
          }
          fprintf(out, "){ .data = ");
          
          // Use statement expression to allocate and initialize heap buffer
          fprintf(out, "({ ");
          emit_type_ref_as_c_type(ctx, type->generic_args, out, false);
          fprintf(out, "* __tmp_buf = rae_ext_rae_buf_alloc(");
          
          int count = 0;
          for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) count++;
          
          fprintf(out, "%d, sizeof(", count);
          emit_type_ref_as_c_type(ctx, type->generic_args, out, false);
          fprintf(out, ")); ");
          
          if (count > 0) {
              fprintf(out, "static const ");
              emit_type_ref_as_c_type(ctx, type->generic_args, out, false);
              fprintf(out, " __stack_init[] = {");
              for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) {
                  emit_expr(ctx, e->value, out, PREC_LOWEST, false, false);
                  if (e->next) fprintf(out, ", ");
              }
              fprintf(out, "}; memcpy(__tmp_buf, __stack_init, sizeof(__stack_init)); ");
          }
          
          fprintf(out, "__tmp_buf; }), .length = ((int64_t)%dLL), .capacity = ((int64_t)%dLL) }", count, count);
      } else {
          fprintf(out, "0");
      }
      break;
    }
    case AST_EXPR_MATCH: {
        const AstMatchArm* arm = expr->as.match_expr.arms;
        int count = 0;
        Str type_name = infer_expr_type(ctx, expr->as.match_expr.subject);
        bool is_str = types_match(type_name, str_from_cstr("String"));
        
        while (arm) {
            if (!arm->pattern) {
                emit_expr(ctx, arm->value, out, PREC_LOWEST, false, false);
                break;
            } else {
                fprintf(out, "(");
                if (is_str) {
                    fprintf(out, "rae_ext_rae_str_eq(");
                    emit_expr(ctx, expr->as.match_expr.subject, out, PREC_LOWEST, false, false);
                    fprintf(out, ", ");
                    emit_expr(ctx, arm->pattern, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                } else {
                    emit_expr(ctx, expr->as.match_expr.subject, out, PREC_EQUALITY, false, false);
                    fprintf(out, " == ");
                    emit_expr(ctx, arm->pattern, out, PREC_EQUALITY, false, false);
                }
                fprintf(out, " ? ");
                emit_expr(ctx, arm->value, out, PREC_TERNARY, false, false);
                fprintf(out, " : ");
                count++;
            }
            arm = arm->next;
        }
        if (!arm) fprintf(out, "0"); // Fallback if no default
        for (int i = 0; i < count; i++) fprintf(out, ")");
        break;
    }
    case AST_EXPR_INTERP: {
        fprintf(out, "rae_ext_rae_str_concat(");
        const AstInterpPart* p = expr->as.interp.parts;
        if (p) {
            fprintf(out, "rae_str_any(");
            emit_rae_any_expr(ctx, p->value, out);
            fprintf(out, "), ");
            if (p->next) {
                AstExpr sub = {.kind = AST_EXPR_INTERP, .as.interp.parts = p->next};
                emit_expr(ctx, &sub, out, PREC_LOWEST, false, false);
            } else {
                fprintf(out, "\"\"");
            }
        } else {
            fprintf(out, "\"\", \"\"");
        }
        fprintf(out, ")");
        break;
    }
    case AST_EXPR_METHOD_CALL: {
        Str method_name = expr->as.method_call.method_name;
        if (str_eq_cstr(method_name, "toString")) {
            // Built-in toString() handling
            fprintf(out, "rae_ext_rae_str_from_cstr(rae_str_any(");
            emit_rae_any_expr(ctx, expr->as.method_call.object, out);
            fprintf(out, "))");
            break;
        }
        // Desugar to function call: method(object, args...)
        AstExpr call = { .kind = AST_EXPR_CALL, .line = expr->line, .column = expr->column };
        call.as.call.callee = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstExpr));
        call.as.call.callee->kind = AST_EXPR_IDENT;
        call.as.call.callee->as.ident = method_name;
        
        AstCallArg* first_arg = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstCallArg));
        first_arg->name = str_from_cstr("this");
        first_arg->value = expr->as.method_call.object;
        first_arg->next = expr->as.method_call.args;
        
        call.as.call.args = first_arg;
        call.as.call.generic_args = expr->as.method_call.generic_args;
        
        // Use the same logic as AST_EXPR_CALL for unwrapping
        const AstTypeRef* tr = infer_expr_type_ref(ctx, &call);
        bool returns_any = tr && tr->is_opt;
        bool needs_unwrap = returns_any && ctx->has_expected_type;
        
        if (needs_unwrap) {
            Str base = get_base_type_name(&ctx->expected_type);
            fprintf(out, "(");
            emit_call_expr(ctx, &call, out);
            if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, ").as.i");
            else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) fprintf(out, ").as.f");
            else if (str_eq_cstr(base, "Bool")) fprintf(out, ").as.b");
            else if (str_eq_cstr(base, "String")) fprintf(out, ").as.s");
            else fprintf(out, ").as.ptr");
        } else {
            emit_call_expr(ctx, &call, out);
        }
        break;
    }
    case AST_EXPR_LIST: {
        const AstExprList* item = expr->as.list;
        fprintf(out, "{ ");
        while (item) {
            emit_expr(ctx, item->value, out, PREC_LOWEST, false, false);
            if (item->next) fprintf(out, ", ");
            item = item->next;
        }
        fprintf(out, " }");
        break;
    }
    case AST_EXPR_INDEX: {
        bool is_ptr = expr->as.index.target->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.index.target->as.ident);
        if (is_ptr) {
            fprintf(out, "(");
            emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false);
            fprintf(out, ")");
        } else {
            emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false);
        }
        fprintf(out, "[");
        emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false, false);
        fprintf(out, "]");
        break;
    }
    default: break;
  }
  return true;
}

static bool emit_rae_any_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
    if (!expr) return true;
    
    Str type_name = infer_expr_type(ctx, expr);
    bool is_primitive = is_primitive_type(type_name);
    
    const AstTypeRef* tr_full = infer_expr_type_ref(ctx, expr);
    bool is_ref = tr_full && (tr_full->is_view || tr_full->is_mod);
    bool is_mod = tr_full && tr_full->is_mod;
    
    // Buffer, List and Any are pointer types in C, don't treat them as Rae references for tagging
    bool is_pointer_representation = str_eq_cstr(type_name, "List") || str_eq_cstr(type_name, "Buffer") || str_eq_cstr(type_name, "Any");
    if (is_pointer_representation) {
        fprintf(out, "rae_any((");
        emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
        fprintf(out, "))");
        return true;
    }

    if (tr_full && tr_full->is_opt) {
        if (!is_ref && (is_primitive || str_eq_cstr(type_name, "String"))) {
            fprintf(out, "rae_any((");
            emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
            if (str_eq_cstr(type_name, "Int64") || str_eq_cstr(type_name, "Char")) fprintf(out, ").as.i)");
            else if (str_eq_cstr(type_name, "Float64")) fprintf(out, ").as.f)");
            else if (str_eq_cstr(type_name, "Bool")) fprintf(out, ").as.b)");
            else if (str_eq_cstr(type_name, "String")) fprintf(out, ").as.s)");
            else fprintf(out, ").as.ptr)");
            return true;
        }
        emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
        return true;
    }
    
    if (is_primitive || str_eq_cstr(type_name, "String")) {
        if (is_ref) {
            const char* rt = "RAE_TYPE_NONE";
            const char* ct = "int64_t";
            if (str_eq_cstr(type_name, "Int64") || str_eq_cstr(type_name, "Int") || str_eq_cstr(type_name, "int64_t")) { rt = "RAE_TYPE_INT64"; ct = "int64_t"; }
            else if (str_eq_cstr(type_name, "Int32") || str_eq_cstr(type_name, "int32_t")) { rt = "RAE_TYPE_INT32"; ct = "int32_t"; }
            else if (str_eq_cstr(type_name, "UInt64") || str_eq_cstr(type_name, "uint64_t")) { rt = "RAE_TYPE_UINT64"; ct = "uint64_t"; }
            else if (str_eq_cstr(type_name, "Float64") || str_eq_cstr(type_name, "Float") || str_eq_cstr(type_name, "double")) { rt = "RAE_TYPE_FLOAT64"; ct = "double"; }
            else if (str_eq_cstr(type_name, "Float32") || str_eq_cstr(type_name, "float")) { rt = "RAE_TYPE_FLOAT32"; ct = "float"; }
            else if (str_eq_cstr(type_name, "Bool") || str_eq_cstr(type_name, "rae_Bool") || str_eq_cstr(type_name, "int8_t")) { rt = "RAE_TYPE_BOOL"; ct = "rae_Bool"; }
            else if (str_eq_cstr(type_name, "Char") || str_eq_cstr(type_name, "Char32") || str_eq_cstr(type_name, "uint32_t")) { rt = "RAE_TYPE_CHAR"; ct = "uint32_t"; }
            else if (str_eq_cstr(type_name, "String") || str_eq_cstr(type_name, "rae_String") || str_eq_cstr(type_name, "const_char_p") || str_eq_cstr(type_name, "const char*")) { rt = "RAE_TYPE_STRING"; ct = "rae_String"; }
            
            fprintf(out, "rae_any_%s(", is_mod ? "mod" : "view");
            emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
            fprintf(out, ".ptr, %s)", rt);
        } else {
            if (str_eq_cstr(type_name, "Char") || str_eq_cstr(type_name, "Char32")) fprintf(out, "rae_any_char((");
            else if (str_eq_cstr(type_name, "Bool")) fprintf(out, "rae_any_bool((");
            else fprintf(out, "rae_any((");
            emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
            fprintf(out, "))");
        }
        return true;
    }

    // Non-primitive types (structs, etc)
    bool is_enum_access = false;
    if (expr->kind == AST_EXPR_MEMBER && expr->as.member.object->kind == AST_EXPR_IDENT) {
        if (find_enum_decl(ctx, ctx->module, expr->as.member.object->as.ident)) is_enum_access = true;
    }
    bool is_ptr_var = (expr->kind == AST_EXPR_IDENT && is_pointer_type(ctx, expr->as.ident));
    bool is_lvalue = (expr->kind == AST_EXPR_IDENT || expr->kind == AST_EXPR_MEMBER || expr->kind == AST_EXPR_INDEX) && !is_enum_access && !is_ptr_var;

    if (is_ptr_var) {
        // Already a pointer representation
        fprintf(out, "rae_any((");
        emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
        fprintf(out, "))");
        return true;
    }

    if (is_lvalue) {
        if (is_mod) fprintf(out, "rae_any_mod(");
        else if (is_ref) fprintf(out, "rae_any_view(");
        fprintf(out, "rae_any((&(");
        emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
        fprintf(out, ")))");
        if (is_ref) fprintf(out, ", RAE_TYPE_BUFFER)");
        return true;
    }

    // Rvalue, lift to temp
    if (is_ref && is_primitive) {
        const char* rt = "RAE_TYPE_NONE";
        const char* ct = "int64_t";
        if (str_eq_cstr(type_name, "Int64") || str_eq_cstr(type_name, "Int") || str_eq_cstr(type_name, "int64_t")) { rt = "RAE_TYPE_INT64"; ct = "int64_t"; }
        else if (str_eq_cstr(type_name, "Int32") || str_eq_cstr(type_name, "int32_t")) { rt = "RAE_TYPE_INT32"; ct = "int32_t"; }
        else if (str_eq_cstr(type_name, "UInt64") || str_eq_cstr(type_name, "uint64_t")) { rt = "RAE_TYPE_UINT64"; ct = "uint64_t"; }
        else if (str_eq_cstr(type_name, "Float64") || str_eq_cstr(type_name, "Float") || str_eq_cstr(type_name, "double")) { rt = "RAE_TYPE_FLOAT64"; ct = "double"; }
        else if (str_eq_cstr(type_name, "Float32") || str_eq_cstr(type_name, "float")) { rt = "RAE_TYPE_FLOAT32"; ct = "float"; }
        else if (str_eq_cstr(type_name, "Bool") || str_eq_cstr(type_name, "rae_Bool") || str_eq_cstr(type_name, "int8_t")) { rt = "RAE_TYPE_BOOL"; ct = "rae_Bool"; }
        else if (str_eq_cstr(type_name, "Char") || str_eq_cstr(type_name, "Char32") || str_eq_cstr(type_name, "uint32_t")) { rt = "RAE_TYPE_CHAR"; ct = "uint32_t"; }
        else if (str_eq_cstr(type_name, "String") || str_eq_cstr(type_name, "rae_String") || str_eq_cstr(type_name, "const_char_p") || str_eq_cstr(type_name, "const char*")) {
            // String is a struct rvalue, don't lift to temp with {X}
            fprintf(out, "rae_any(");
            emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
            fprintf(out, ")");
            return true;
        }

        fprintf(out, "rae_any_%s(", is_mod ? "mod" : "view");
        fprintf(out, "LIFT_TO_TEMP_STRUCT(");
        emit_type_ref_as_c_type(ctx, tr_full, out, false);
        fprintf(out, ", .ptr = LIFT_TO_TEMP(%s, ", ct);
        emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
        fprintf(out, "))).ptr, %s)", rt);
    } else {
        if (is_mod) fprintf(out, "rae_any_mod(");
        else if (is_ref) fprintf(out, "rae_any_view(");
        
        if (!is_ref && !is_mod && (str_eq_cstr(type_name, "String") || str_eq_cstr(type_name, "rae_String"))) {
             fprintf(out, "rae_any((");
             emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
             fprintf(out, "))");
             return true;
        }

        if (is_ref) {
            fprintf(out, "rae_any((");
            emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
            fprintf(out, ".ptr))");
        } else {
            fprintf(out, "rae_any_ptr(&(LIFT_VALUE(");
            const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr_full);
            fprintf(out, "%s, ", mangled);
            emit_expr(ctx, expr, out, PREC_LOWEST, false, true);
            fprintf(out, ")))");
        }
        
        if (is_ref) fprintf(out, ", RAE_TYPE_BUFFER)");
    }

    return true;
}
static bool emit_log_any_call(CFuncContext* ctx, const AstExpr* expr, FILE* out, bool stream) {
    const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
    Str type_name = get_base_type_name(tr);
    bool is_primitive = is_primitive_type(type_name);
    bool is_raylib = ctx->uses_raylib && is_raylib_builtin_type(type_name);
    bool is_id_key = tr && (tr->is_id || tr->is_key);
    
    if (is_primitive || is_raylib || is_id_key || str_eq_cstr(type_name, "String") || str_eq_cstr(type_name, "Any") || type_name.len == 0 || (tr && tr->is_opt)) {
        fprintf(out, "%s(", stream ? "rae_ext_rae_log_stream_any" : "rae_ext_rae_log_any");
        emit_rae_any_expr(ctx, expr, out);
        fprintf(out, ")");
    } else {
        // Use specialized logger
        const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
        if (stream) {
            fprintf(out, "rae_log_stream_%s_(", mangled);
            emit_expr(ctx, expr, out, PREC_LOWEST, false, false);
        } else {
            fprintf(out, "rae_log_%s_(", mangled);
            emit_expr(ctx, expr, out, PREC_LOWEST, false, false);
        }
        fprintf(out, ")");
    }
    return true;
}

static bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
    const AstExpr* callee = expr->as.call.callee;
    if (callee->kind != AST_EXPR_IDENT) return false;
    
    const AstFuncDecl* d = NULL;

    // Special handling for buffer primitives to support monomorphisation
    if (str_eq_cstr(callee->as.ident, "rae_ext_rae_buf_set") || str_eq_cstr(callee->as.ident, "__buf_set")) {
        const AstCallArg* buf = expr->as.call.args;
        const AstCallArg* idx = buf->next;
        const AstCallArg* val = idx->next;
        
        const AstTypeRef* tr = infer_expr_type_ref(ctx, buf->value);
        AstTypeRef elem_tr = {0};
        bool has_elem = false;
        if (tr && tr->generic_args) { elem_tr = *tr->generic_args; has_elem = true; }

        emit_expr(ctx, buf->value, out, PREC_CALL, false, false);
        fprintf(out, "[");
        emit_expr(ctx, idx->value, out, PREC_LOWEST, false, false);
        fprintf(out, "] = ");
        
        AstTypeRef old_expected = ctx->expected_type;
        bool old_has = ctx->has_expected_type;
        if (has_elem) { ctx->expected_type = elem_tr; ctx->has_expected_type = true; }
        else ctx->has_expected_type = false;

        emit_expr(ctx, val->value, out, PREC_LOWEST, false, false);
        
        ctx->expected_type = old_expected;
        ctx->has_expected_type = old_has;
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "rae_ext_rae_buf_get") || str_eq_cstr(callee->as.ident, "__buf_get")) {
        const AstCallArg* buf = expr->as.call.args;
        const AstCallArg* idx = buf->next;
        emit_expr(ctx, buf->value, out, PREC_CALL, false, false);
        fprintf(out, "[");
        emit_expr(ctx, idx->value, out, PREC_LOWEST, false, false);
        fprintf(out, "]");
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "rae_ext_rae_buf_alloc") || str_eq_cstr(callee->as.ident, "__buf_alloc")) {
        fprintf(out, "(void*)rae_ext_rae_buf_alloc(");
        const AstCallArg* a = expr->as.call.args;
        while (a) {
            emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
            if (a->next) fprintf(out, ", ");
            a = a->next;
        }
        // Buffer allocation needs element size if not provided
        uint16_t arg_count = 0; for (const AstCallArg* ca = expr->as.call.args; ca; ca = ca->next) arg_count++;
        if (arg_count == 1) {
            fprintf(out, ", sizeof(");
            if (expr->as.call.generic_args) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
                emit_type_ref_as_c_type(ctx, sub, out, false);
            } else {
                fprintf(out, "int64_t");
            }
            fprintf(out, ")");
        }
        fprintf(out, ")");
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "rae_ext_rae_buf_free") || str_eq_cstr(callee->as.ident, "__buf_free")) {
        fprintf(out, "rae_ext_rae_buf_free((void*)(");
        if (expr->as.call.args) emit_expr(ctx, expr->as.call.args->value, out, PREC_LOWEST, false, false);
        fprintf(out, "))");
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "rae_ext_rae_buf_copy") || str_eq_cstr(callee->as.ident, "__buf_copy")) {
        fprintf(out, "rae_ext_rae_buf_copy(");
        const AstCallArg* a = expr->as.call.args;
        int count = 0;
        while (a) {
            if (count == 0 || count == 2) fprintf(out, "(void*)(");
            emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
            if (count == 0 || count == 2) fprintf(out, ")");
            if (a->next) fprintf(out, ", ");
            a = a->next;
            count++;
        }
        if (count == 5) {
            fprintf(out, ", sizeof(");
            if (expr->as.call.generic_args) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
                emit_type_ref_as_c_type(ctx, sub, out, false);
            } else {
                fprintf(out, "int64_t");
            }
            fprintf(out, ")");
        }
        fprintf(out, ")");
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "sizeof")) {
        fprintf(out, "sizeof(");
        const AstTypeRef* type_to_emit = NULL;
        if (expr->as.call.generic_args) {
            type_to_emit = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
        } else if (expr->as.call.args && expr->as.call.args->value->kind == AST_EXPR_IDENT) {
            // Check if it's a generic parameter
            Str ident = expr->as.call.args->value->as.ident;
            if (ctx->generic_params) {
                const AstIdentifierPart* gp = ctx->generic_params;
                const AstTypeRef* arg = ctx->generic_args;
                while (gp && arg) {
                    if (str_eq(gp->text, ident)) { type_to_emit = arg; break; }
                    gp = gp->next; arg = arg->next;
                }
            }
            if (!type_to_emit) {
                // Fallback to identifier as type name
                fprintf(out, "rae_%.*s", (int)ident.len, ident.data);
            }
        }
        
        if (type_to_emit) {
            emit_type_ref_as_c_type(ctx, type_to_emit, out, false);
        } else if (!expr->as.call.args) {
            fprintf(out, "int64_t"); // Default
        }
        fprintf(out, ")");
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "__buf_alloc")) {
        fprintf(out, "rae_ext_rae_buf_alloc(");
        const AstCallArg* a = expr->as.call.args;
        emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
        fprintf(out, ", sizeof(");
        const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
        if (tr && tr->generic_args) {
            AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr->generic_args);
            emit_type_ref_as_c_type(ctx, sub, out, false);
        } else if (expr->as.call.generic_args) {
            AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
            emit_type_ref_as_c_type(ctx, sub, out, false);
        } else {
            fprintf(out, "int64_t");
        }
        fprintf(out, "))");
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "__buf_free")) {
        fprintf(out, "rae_ext_rae_buf_free(");
        emit_expr(ctx, expr->as.call.args->value, out, PREC_LOWEST, false, false);
        fprintf(out, ")");
        return true;
    }
    if (str_eq_cstr(callee->as.ident, "__buf_copy")) {
        fprintf(out, "rae_ext_rae_buf_copy(");
        const AstCallArg* src = expr->as.call.args;
        const AstCallArg* src_off = src->next;
        const AstCallArg* dst = src_off->next;
        const AstCallArg* dst_off = dst->next;
        const AstCallArg* len = dst_off->next;
        emit_expr(ctx, src->value, out, PREC_LOWEST, false, false); fprintf(out, ", ");
        emit_expr(ctx, src_off->value, out, PREC_LOWEST, false, false); fprintf(out, ", ");
        emit_expr(ctx, dst->value, out, PREC_LOWEST, false, false); fprintf(out, ", ");
        emit_expr(ctx, dst_off->value, out, PREC_LOWEST, false, false); fprintf(out, ", ");
        emit_expr(ctx, len->value, out, PREC_LOWEST, false, false);
        fprintf(out, ", 8)"); // Default element size for now
        return true;
    }

        if (str_eq_cstr(callee->as.ident, "log")) {

            const AstCallArg* a = expr->as.call.args;

            if (a && a->value->kind == AST_EXPR_INTERP) {

                const AstInterpPart* p = a->value->as.interp.parts;

                while (p) {

                    emit_log_any_call(ctx, p->value, out, true);

                    fprintf(out, ", ");

                    p = p->next;

                }

                fprintf(out, "rae_ext_rae_log_cstr(\"\")"); 

                return true;

            }

                

            if (a) {

                emit_log_any_call(ctx, a->value, out, false);

                return true;

            }

            fprintf(out, "rae_ext_rae_log_cstr(\"\")");

            return true;

        } else if (str_eq_cstr(callee->as.ident, "log_stream") || str_eq_cstr(callee->as.ident, "logS")) {

            const AstCallArg* a = expr->as.call.args;

            if (a) {

                emit_log_any_call(ctx, a->value, out, true);

                return true;

            }

            return true;

        }

    

        // Check if it's already an external function call (starts with rae_ext_)

        if (str_starts_with_cstr(callee->as.ident, "rae_ext_")) {

            fprintf(out, "%.*s(", (int)callee->as.ident.len, callee->as.ident.data);

        } else {

            // Find the actual function declaration to mangle correctly

            uint16_t param_count = 0;

            for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;

            

            Str receiver_type_name = {0};

            if (expr->as.call.args) receiver_type_name = infer_expr_type(ctx, expr->as.call.args->value);

            

            d = find_function_overload(ctx->module, ctx, callee->as.ident, &receiver_type_name, param_count, false);

            if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, &receiver_type_name, param_count, true);

            if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, false);

            if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, true);

            

            if (d) {

                AstTypeRef* inferred_args = NULL;

                if (d->generic_params && expr->as.call.generic_args) {

                    // We MUST substitute any generic parameters in the call's generic args

                    // using the current function's specialization context.

                    AstTypeRef* concrete_args = NULL;

                    AstTypeRef* last_arg = NULL;

                    for (const AstTypeRef* arg = expr->as.call.generic_args; arg; arg = arg->next) {

                        AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg);

                        if (!concrete_args) concrete_args = sub;

                        else last_arg->next = sub;

                        last_arg = sub;

                    }

                    inferred_args = concrete_args;

                } else if (d->generic_params) {

                    // Try inferring from receiver
                    if (!inferred_args && d->params && str_eq_cstr(d->params->name, "this")) {
                        const AstCallArg* first_arg = expr->as.call.args;
                        if (first_arg) {
                            const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, first_arg->value);
                            AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->params->type, receiver_type);
                            if (inferred) {
                                // We MUST substitute any generic parameters in the inferred args
                                inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                            }
                        }
                    }

                    if (!inferred_args && ctx->generic_args) {
                        // Implicit specialization: use current function's generic args if the callee is generic
                        inferred_args = (AstTypeRef*)ctx->generic_args;
                    }

                }

                

                if (inferred_args) {
                    // Ensure all generic parameters in inferred args are substituted using calling context
                    AstTypeRef* concrete = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred_args);
                    register_function_specialization(ctx->compiler_ctx, d, concrete);
                    fprintf(out, "%s(", rae_mangle_specialized_function(ctx->compiler_ctx, d, concrete));
                } else {

                    fprintf(out, "%s(", rae_mangle_function(ctx->compiler_ctx, d));

                }

            } else {

                // Fallback for cases where decl is not found (e.g. some built-ins or log)

                const char* ray_mapping = find_raylib_mapping(callee->as.ident);

                if (ray_mapping) {

                    fprintf(out, "rae_ext_%.*s(", (int)callee->as.ident.len, callee->as.ident.data);

                } else if (str_starts_with_cstr(callee->as.ident, "create") && expr->as.call.generic_args) {

                    const char* m = rae_mangle_type(ctx->compiler_ctx, NULL, expr->as.call.generic_args);

                    fprintf(out, "rae_create%s_(", m);

                    const AstCallArg* a = expr->as.call.args;

                    while (a) {

                        emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);

                        if (a->next) fprintf(out, ", ");

                        a = a->next;

                    }

                    fprintf(out, ")");

                    return true;

                } else {

                    fprintf(out, "rae_%.*s_(", (int)callee->as.ident.len, callee->as.ident.data);

                }

            }

        }

    
    
    const AstParam* p = d ? d->params : NULL;
    for (const AstCallArg* a = expr->as.call.args; a; a = a->next) {
        bool needs_ref = false;
        bool is_rvalue = true;
        
        const AstTypeRef* source_tr = infer_expr_type_ref(ctx, a->value);
        bool source_is_ref = source_tr && (source_tr->is_view || source_tr->is_mod);
        
        // Also check if it's a function call that returns a reference
        if (!source_is_ref && a->value->kind == AST_EXPR_CALL) {
            const AstExpr* c = a->value->as.call.callee;
            if (c->kind == AST_EXPR_IDENT) {
                uint16_t pc = 0; for (const AstCallArg* ca = a->value->as.call.args; ca; ca = ca->next) pc++;
                const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, c->as.ident, NULL, pc, false);
                if (!fd) fd = find_function_overload(ctx->module, ctx, c->as.ident, NULL, pc, true);
                if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ref = true;
            } else if (c->kind == AST_EXPR_MEMBER) {
                Str m = c->as.member.member; Str on = infer_expr_type(ctx, c->as.member.object);
                uint16_t pc = 1; for (const AstCallArg* ca = a->value->as.call.args; ca; ca = ca->next) pc++;
                const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, m, &on, pc, true);
                if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ref = true;
            }
        } else if (!source_is_ref && a->value->kind == AST_EXPR_METHOD_CALL) {
            Str m = a->value->as.method_call.method_name; Str on = infer_expr_type(ctx, a->value->as.method_call.object);
            uint16_t pc = 1; for (const AstCallArg* ca = a->value->as.method_call.args; ca; ca = ca->next) pc++;
            const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, m, &on, pc, true);
            if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ref = true;
        }

        bool is_param_ref = p && p->type && (p->type->is_view || p->type->is_mod);
        if (is_param_ref && !source_is_ref) {
            bool is_ptr_var = a->value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, a->value->as.ident);
            if (is_ptr_var) {
                needs_ref = false; is_rvalue = false;
            } else if (a->value->kind == AST_EXPR_IDENT || a->value->kind == AST_EXPR_MEMBER || a->value->kind == AST_EXPR_INDEX) {
                needs_ref = true; is_rvalue = false;
            } else {
                needs_ref = true; is_rvalue = true;
            }
        }
        
        if (needs_ref) {
            bool is_prim_ref_param = is_primitive_ref(ctx, p->type);
            if (is_rvalue) {
                if (is_prim_ref_param) {
                    fprintf(out, "LIFT_TO_TEMP_STRUCT(");
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, p->type);
                    emit_type_ref_as_c_type(ctx, sub, out, false);
                    fprintf(out, ", .ptr = LIFT_TO_TEMP(");
                    emit_type_ref_as_c_type(ctx, sub, out, true);
                    fprintf(out, ", ");
                } else {
                    fprintf(out, "LIFT_TO_TEMP(");
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, p->type);
                    emit_type_ref_as_c_type(ctx, sub, out, true); // skip_ptr because we take address
                    fprintf(out, ", ");
                }
                emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
                if (is_prim_ref_param) fprintf(out, "))");
                else fprintf(out, ")");
            } else {
                if (is_prim_ref_param) {
                    fprintf(out, "LIFT_TO_TEMP_STRUCT(");
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, p->type);
                    emit_type_ref_as_c_type(ctx, sub, out, false);
                    fprintf(out, ", .ptr = &(");
                } else {
                    fprintf(out, "&(");
                }
                emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
                if (is_prim_ref_param) fprintf(out, "))");
                else fprintf(out, ")");
            }
        } else {
            emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
        }
        
        if (a->next) fprintf(out, ", ");
        if (p) p = p->next;
    }
    fprintf(out, ")");
    return true;
}

static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out) { fprintf(out, "  "); emit_expr(ctx, expr, out, PREC_LOWEST, false, false); fprintf(out, ";\n"); return true; }

static bool emit_block(CFuncContext* ctx, const AstBlock* block, FILE* out) {
    if (!block) return true;
    fprintf(out, "  {\n"); int old = (int)ctx->local_count; ctx->scope_depth++;
    for (const AstStmt* s = block->first; s; s = s->next) emit_stmt(ctx, s, out);
    emit_defers(ctx, ctx->scope_depth, out); pop_defers(ctx, ctx->scope_depth);
    ctx->scope_depth--; ctx->local_count = old; fprintf(out, "  }\n");
    return true;
}

static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    if (!stmt) return true;
    switch (stmt->kind) {
        case AST_STMT_LET: {
            fprintf(out, "  ");
            bool is_ref = stmt->as.let_stmt.type && (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod);
            if (stmt->as.let_stmt.type) emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
            else fprintf(out, "int64_t");
            fprintf(out, " %.*s = ", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
            
            bool source_is_ptr = false;
            const AstTypeRef* source_tr = NULL;
            if (stmt->as.let_stmt.value) {
                source_tr = infer_expr_type_ref(ctx, stmt->as.let_stmt.value);
                if (source_tr && (source_tr->is_view || source_tr->is_mod)) source_is_ptr = true;
                
                // Check if it's a function call that returns a reference
                if (!source_is_ptr && stmt->as.let_stmt.value->kind == AST_EXPR_CALL) {
                    const AstExpr* callee = stmt->as.let_stmt.value->as.call.callee;
                    if (callee->kind == AST_EXPR_IDENT) {
                        uint16_t param_count = 0;
                        for (const AstCallArg* a = stmt->as.let_stmt.value->as.call.args; a; a = a->next) param_count++;
                        const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, false);
                        if (!fd) fd = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, true);
                        if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ptr = true;
                    } else if (callee->kind == AST_EXPR_MEMBER) {
                        Str member = callee->as.member.member;
                        Str obj_name = infer_expr_type(ctx, callee->as.member.object);
                        uint16_t param_count = 1;
                        for (const AstCallArg* a = stmt->as.let_stmt.value->as.call.args; a; a = a->next) param_count++;
                        const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, member, &obj_name, param_count, true);
                        if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ptr = true;
                    }
                } else if (!source_is_ptr && stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL) {
                    Str method_name = stmt->as.let_stmt.value->as.method_call.method_name;
                    Str obj_name = infer_expr_type(ctx, stmt->as.let_stmt.value->as.method_call.object);
                    uint16_t param_count = 1;
                    for (const AstCallArg* a = stmt->as.let_stmt.value->as.method_call.args; a; a = a->next) param_count++;
                    const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, method_name, &obj_name, param_count, true);
                    if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ptr = true;
                }
            }
            
            bool needs_lift = is_primitive_ref(ctx, stmt->as.let_stmt.type) && !source_is_ptr;

            if (needs_lift) {
                fprintf(out, "LIFT_VALUE(");
                emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
                fprintf(out, ", .ptr = &(");
            } else if (is_ref && !source_is_ptr) {
                fprintf(out, "&(");
            }
            if (is_ref && source_is_ptr) {
                fprintf(out, "(");
                emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
                fprintf(out, ")");
            }
            if (stmt->as.let_stmt.value) {
                bool needs_unwrap = false;
                if (source_tr && source_tr->is_opt && stmt->as.let_stmt.type && !stmt->as.let_stmt.type->is_opt && !is_ref) {
                    needs_unwrap = true;
                }

                if (stmt->as.let_stmt.type) { ctx->expected_type = *stmt->as.let_stmt.type; ctx->has_expected_type = true; }
                else ctx->has_expected_type = false;
                
                if (needs_unwrap) {
                    Str base = get_base_type_name(stmt->as.let_stmt.type);
                    fprintf(out, "(");
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, ").as.i");
                    else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) fprintf(out, ").as.f");
                    else if (str_eq_cstr(base, "Bool")) fprintf(out, ").as.b");
                    else if (str_eq_cstr(base, "String")) fprintf(out, ").as.s");
                    else fprintf(out, ").as.ptr");
                } else {
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false); 
                }
                ctx->has_expected_type = false;
            } else {
                emit_auto_init(ctx, stmt->as.let_stmt.type, out);
            }
            if (needs_lift) fprintf(out, ")))");
            else if (is_ref && !source_is_ptr) fprintf(out, ")");
            
            fprintf(out, ";\n");
            if (ctx->local_count < 256) {
                ctx->locals[ctx->local_count] = stmt->as.let_stmt.name; ctx->local_type_refs[ctx->local_count] = stmt->as.let_stmt.type;
                const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.let_stmt.type);
                ctx->local_types[ctx->local_count] = str_from_cstr(tn);
                
                // If it's a primitive ref, it's NOT a raw C pointer in our representation (it's a struct rae_View_Int)
                bool is_prim_ref = is_primitive_ref(ctx, stmt->as.let_stmt.type);
                ctx->local_is_ptr[ctx->local_count] = (stmt->as.let_stmt.type && (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod)) && !is_prim_ref;
                ctx->local_is_mod[ctx->local_count] = (stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_mod);
                ctx->local_count++;
            }
            return true;
        }
        case AST_STMT_ASSIGN: {
            fprintf(out, "  ");
            const AstTypeRef* tr = infer_expr_type_ref(ctx, stmt->as.assign_stmt.target);
            bool target_is_ptr_var = stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT && is_pointer_type(ctx, stmt->as.assign_stmt.target->as.ident);
            bool target_is_mod_ref = is_primitive_ref(ctx, tr);

            // If it's a mod/view variable being reassigned (not dereferenced), we need & or LIFT_VALUE
            bool needs_ref = false;
            bool needs_lift = false;
            if (target_is_ptr_var && stmt->as.assign_stmt.is_bind) {
                // Binding a reference: ptr = &val
                bool source_is_ptr = false;
                const AstTypeRef* source_tr = infer_expr_type_ref(ctx, stmt->as.assign_stmt.value);
                if (source_tr && (source_tr->is_view || source_tr->is_mod)) source_is_ptr = true;
                
                // Also check if it's a function call that returns a reference
                if (!source_is_ptr && stmt->as.assign_stmt.value->kind == AST_EXPR_CALL) {
                    const AstExpr* c = stmt->as.assign_stmt.value->as.call.callee;
                    if (c->kind == AST_EXPR_IDENT) {
                        uint16_t pc = 0; for (const AstCallArg* ca = stmt->as.assign_stmt.value->as.call.args; ca; ca = ca->next) pc++;
                        const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, c->as.ident, NULL, pc, false);
                        if (!fd) fd = find_function_overload(ctx->module, ctx, c->as.ident, NULL, pc, true);
                        if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ptr = true;
                    } else if (c->kind == AST_EXPR_MEMBER) {
                        Str m = c->as.member.member; Str on = infer_expr_type(ctx, c->as.member.object);
                        uint16_t pc = 1; for (const AstCallArg* ca = stmt->as.assign_stmt.value->as.call.args; ca; ca = ca->next) pc++;
                        const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, m, &on, pc, true);
                        if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ptr = true;
                    }
                                    } else if (!source_is_ptr && stmt->as.assign_stmt.value->kind == AST_EXPR_METHOD_CALL) {
                                        Str m = stmt->as.assign_stmt.value->as.method_call.method_name;
                                        Str on = infer_expr_type(ctx, stmt->as.assign_stmt.value->as.method_call.object);
                                        uint16_t pc = 1; for (const AstCallArg* ca = stmt->as.assign_stmt.value->as.call.args; ca; ca = ca->next) pc++;
                                        const AstFuncDecl* fd = find_function_overload(ctx->module, ctx, m, &on, pc, true);
                                        if (fd && fd->returns && (fd->returns->type->is_view || fd->returns->type->is_mod)) source_is_ptr = true;
                                    }
                                    
                                    if (!source_is_ptr) {
                                        if (is_primitive_ref(ctx, tr)) needs_lift = true;
                                        else needs_ref = true;
                                    }
                        
            }

            if (target_is_mod_ref && !stmt->as.assign_stmt.is_bind) {
                fprintf(out, "(*");
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_ASSIGN, true, false);
                fprintf(out, ".ptr)");
            } else {
                if (target_is_ptr_var && !stmt->as.assign_stmt.is_bind) fprintf(out, "(*");
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_ASSIGN, true, false);
                if (target_is_ptr_var && !stmt->as.assign_stmt.is_bind) fprintf(out, ")");
            }
            fprintf(out, " = ");
            
            if (needs_lift) {
                fprintf(out, "LIFT_VALUE(");
                emit_type_ref_as_c_type(ctx, tr, out, false);
                fprintf(out, ", .ptr = &(");
            } else if (needs_ref) {
                fprintf(out, "&(");
            }
            
            if (tr) {
                ctx->expected_type = *tr;
                if (target_is_ptr_var && !stmt->as.assign_stmt.is_bind) { 
                    ctx->expected_type.is_view = ctx->expected_type.is_mod = false; 
                }
                ctx->has_expected_type = true;
            } else {
                ctx->has_expected_type = false;
            }

            bool needs_unwrap = false;
            const AstTypeRef* source_tr = infer_expr_type_ref(ctx, stmt->as.assign_stmt.value);
            if (source_tr && source_tr->is_opt && tr && !tr->is_opt && !(tr->is_view || tr->is_mod)) {
                needs_unwrap = true;
            }

            if (needs_unwrap) {
                Str base = get_base_type_name(tr);
                fprintf(out, "(");
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
                if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, ").as.i");
                else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) fprintf(out, ").as.f");
                else if (str_eq_cstr(base, "Bool")) fprintf(out, ").as.b");
                else if (str_eq_cstr(base, "String")) fprintf(out, ").as.s");
                else fprintf(out, ").as.ptr");
            } else {
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
            }
            ctx->has_expected_type = false;
            if (needs_lift) fprintf(out, ")))");
            else if (needs_ref) fprintf(out, ")");
            
            fprintf(out, ";\n"); return true;
        }
        case AST_STMT_EXPR: return emit_call(ctx, stmt->as.expr_stmt, out);
        case AST_STMT_RET: {
            fprintf(out, "  ");
            emit_defers(ctx, 0, out);
            fprintf(out, "return");
            if (stmt->as.ret_stmt.values) {
                fprintf(out, " ");
                bool ret_is_ref = ctx->func_decl && ctx->func_decl->returns && ctx->func_decl->returns->type && (ctx->func_decl->returns->type->is_view || ctx->func_decl->returns->type->is_mod);
                bool ret_is_opt = ctx->func_decl && ctx->func_decl->returns && ctx->func_decl->returns->type && ctx->func_decl->returns->type->is_opt;
                
                if (ctx->func_decl && ctx->func_decl->returns && ctx->func_decl->returns->type) {
                    ctx->has_expected_type = true;
                    // Substitute generic return type if necessary
                    ctx->expected_type = *substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, ctx->func_decl->returns->type);
                }

                if (ret_is_ref) {
                    const AstTypeRef* tr = infer_expr_type_ref(ctx, stmt->as.ret_stmt.values->value);
                    bool source_is_ptr = tr && (tr->is_view || tr->is_mod);
                    if (!source_is_ptr) fprintf(out, "&(");
                    if (source_is_ptr) {
                        fprintf(out, "(");
                        emit_type_ref_as_c_type(ctx, ctx->func_decl->returns->type, out, false);
                        fprintf(out, ")");
                    }
                    emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                    if (!source_is_ptr) fprintf(out, ")");
                } else if (ret_is_opt) {
                    fprintf(out, "rae_any(");
                    emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                } else {
                    emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                }
                ctx->has_expected_type = false;
            }
            fprintf(out, ";\n");
            return true;
        }
        case AST_STMT_IF: return emit_if(ctx, stmt, out);
        case AST_STMT_LOOP: return emit_loop(ctx, stmt, out);
        case AST_STMT_DEFER: {
            if (ctx->defer_stack.count < 64) {
                ctx->defer_stack.entries[ctx->defer_stack.count].block = stmt->as.defer_stmt.block;
                ctx->defer_stack.entries[ctx->defer_stack.count].scope_depth = ctx->scope_depth;
                ctx->defer_stack.count++;
            }
            return true;
        }
        case AST_STMT_MATCH: return emit_match(ctx, stmt, out);
        default: return true;
    }
}

static bool emit_generic_helpers(CompilerContext* ctx, FILE* out) {
  size_t processed = 0;
  while (processed < ctx->generic_type_count) {
    size_t current_count = ctx->generic_type_count;
    for (size_t i = processed; i < current_count; i++) {
      const char* m = rae_mangle_type(ctx, NULL, ctx->generic_types[i]);
      bool dup = false; for (size_t j = 0; j < ctx->emitted_method_count; j++) if (strcmp(ctx->emitted_method_names[j], m) == 0) { dup = true; break; }
      if (dup) continue; 
      if (ctx->emitted_method_count < ctx->emitted_method_cap) ctx->emitted_method_names[ctx->emitted_method_count++] = m;

      Str base = ctx->generic_types[i]->parts->text;
      fprintf(out, "RAE_UNUSED static const char* rae_toJson_%s_(%s* this) { (void)this; return \"{}\"; }\n", m, m);
      fprintf(out, "RAE_UNUSED static %s rae_fromJson_%s_(const char* json) { (void)json; %s res = {0}; return res; }\n", m, m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_%s_(%s val) { rae_log_stream_%s_(val); printf(\"\\n\"); }\n", m, m, m);
      if (str_eq_cstr(base, "List")) {
          const char* elem_mangled = rae_mangle_type_specialized(ctx, NULL, NULL, ctx->generic_types[i]->generic_args);
          Str elem_base = get_base_type_name(ctx->generic_types[i]->generic_args);
          bool elem_is_primitive = is_primitive_type(elem_base);

          fprintf(out, "RAE_UNUSED static void rae_log_stream_%s_(%s val) {\n", m, m);
          fprintf(out, "  printf(\"{ #(\");\n");
          fprintf(out, "  for (int64_t i = 0; i < val.capacity; i++) {\n");
          fprintf(out, "    if (i > 0) printf(\", \");\n");
          fprintf(out, "    if (i < val.length) {\n");
          if (elem_is_primitive || str_eq_cstr(elem_base, "String") || str_eq_cstr(elem_base, "Any")) {
              fprintf(out, "      rae_ext_rae_log_stream_any(rae_any(val.data[i]));\n");
          } else {
              fprintf(out, "      rae_log_stream_%s_(val.data[i]);\n", elem_mangled);
          }
          fprintf(out, "    } else {\n");
          fprintf(out, "      printf(\"none\");\n");
          fprintf(out, "    }\n");
          fprintf(out, "  }\n");
          fprintf(out, "  printf(\"), %%lld, %%lld }\", (long long)val.length, (long long)val.capacity);\n");
          fprintf(out, "}\n");
          fprintf(out, "RAE_UNUSED static const char* rae_str_%s_(%s val) { (void)val; return \"List(...)\"; }\n\n", m, m);
      } else {
          fprintf(out, "RAE_UNUSED static void rae_log_stream_%s_(%s val) { (void)val; printf(\"%.*s(...)\"); }\n", m, m, (int)base.len, base.data);
          fprintf(out, "RAE_UNUSED static const char* rae_str_%s_(%s val) { (void)val; return \"%.*s(...)\"; }\n\n", m, m, (int)base.len, base.data);
      }
    }
    processed = current_count;
  }
  return true;
}

static bool emit_json_methods(CompilerContext* ctx, const AstModule* module, FILE* out, bool uses_raylib) {
  (void)module;
  // Use context to track emitted names to avoid duplicates
  ctx->emitted_method_count = 0;

  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind == AST_DECL_TYPE && !d->as.type_decl.generic_params) {
      const AstTypeDecl* td = &d->as.type_decl;
      if (has_property(td->properties, "c_struct")) continue;
      
      const char* m = rae_mangle_type(ctx, NULL, & (AstTypeRef){.parts = & (AstIdentifierPart){.text = td->name}});
      bool dup = false; for (size_t j = 0; j < ctx->emitted_method_count; j++) if (strcmp(ctx->emitted_method_names[j], m) == 0) { dup = true; break; }
      if (dup) continue; 
      if (ctx->emitted_method_count < ctx->emitted_method_cap) ctx->emitted_method_names[ctx->emitted_method_count++] = m;

      fprintf(out, "RAE_UNUSED static const char* rae_toJson_%s_(%s* this);\n", m, m);
      fprintf(out, "RAE_UNUSED static %s rae_fromJson_%s_(const char* json);\n", m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_%s_(%s val);\n", m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_stream_%s_(%s val);\n", m, m);
      fprintf(out, "RAE_UNUSED static const char* rae_str_%s_(%s val);\n", m, m);
    } else if (d->kind == AST_DECL_ENUM) {
      const AstEnumDecl* ed = &d->as.enum_decl;
      if (is_primitive_type(ed->name) || (uses_raylib && is_raylib_builtin_type(ed->name))) continue;
      
      const char* m = rae_mangle_type(ctx, NULL, & (AstTypeRef){.parts = & (AstIdentifierPart){.text = ed->name}});
      bool dup = false; for (size_t j = 0; j < ctx->emitted_method_count; j++) if (strcmp(ctx->emitted_method_names[j], m) == 0) { dup = true; break; }
      if (dup) continue; 
      if (ctx->emitted_method_count < ctx->emitted_method_cap) ctx->emitted_method_names[ctx->emitted_method_count++] = m;

      fprintf(out, "RAE_UNUSED static void rae_log_%s_(%s val);\n", m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_stream_%s_(%s val);\n", m, m);
      fprintf(out, "RAE_UNUSED static const char* rae_str_%s_(%s val);\n", m, m);
    }
  }
  for (size_t i = 0; i < ctx->generic_type_count; i++) {
      const char* m = rae_mangle_type(ctx, NULL, ctx->generic_types[i]);
      bool dup = false; for (size_t j = 0; j < ctx->emitted_method_count; j++) if (strcmp(ctx->emitted_method_names[j], m) == 0) { dup = true; break; }
      if (dup) continue; 
      if (ctx->emitted_method_count < ctx->emitted_method_cap) ctx->emitted_method_names[ctx->emitted_method_count++] = m;

      fprintf(out, "RAE_UNUSED static const char* rae_toJson_%s_(%s* this);\n", m, m);
      fprintf(out, "RAE_UNUSED static %s rae_fromJson_%s_(const char* json);\n", m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_%s_(%s val);\n", m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_stream_%s_(%s val);\n", m, m);
      fprintf(out, "RAE_UNUSED static const char* rae_str_%s_(%s val);\n", m, m);
  }
  fprintf(out, "\n");
  
  ctx->emitted_method_count = 0;
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind == AST_DECL_TYPE && !d->as.type_decl.generic_params) {
      const AstTypeDecl* td = &d->as.type_decl;
      if (has_property(td->properties, "c_struct")) continue;
      
      const char* m = rae_mangle_type(ctx, NULL, & (AstTypeRef){.parts = & (AstIdentifierPart){.text = td->name}});
      bool dup = false; for (size_t j = 0; j < ctx->emitted_method_count; j++) if (strcmp(ctx->emitted_method_names[j], m) == 0) { dup = true; break; }
      if (dup) continue; 
      if (ctx->emitted_method_count < ctx->emitted_method_cap) ctx->emitted_method_names[ctx->emitted_method_count++] = m;

      fprintf(out, "RAE_UNUSED static const char* rae_toJson_%s_(%s* this) { (void)this; return \"{}\"; }\n", m, m);
      fprintf(out, "RAE_UNUSED static %s rae_fromJson_%s_(const char* json) { (void)json; %s res = {0}; return res; }\n", m, m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_%s_(%s val) { rae_log_stream_%s_(val); printf(\"\\n\"); }\n", m, m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_stream_%s_(%s val) { (void)val; printf(\"%.*s(...)\"); }\n", m, m, (int)td->name.len, td->name.data);
      fprintf(out, "RAE_UNUSED static const char* rae_str_%s_(%s val) { (void)val; return \"%.*s(...)\"; }\n\n", m, m, (int)td->name.len, td->name.data);
    } else if (d->kind == AST_DECL_ENUM) {
      const AstEnumDecl* ed = &d->as.enum_decl;
      if (is_primitive_type(ed->name) || (uses_raylib && is_raylib_builtin_type(ed->name))) continue;
      
      const char* m = rae_mangle_type(ctx, NULL, & (AstTypeRef){.parts = & (AstIdentifierPart){.text = ed->name}});
      bool dup = false; for (size_t j = 0; j < ctx->emitted_method_count; j++) if (strcmp(ctx->emitted_method_names[j], m) == 0) { dup = true; break; }
      if (dup) continue; 
      if (ctx->emitted_method_count < ctx->emitted_method_cap) ctx->emitted_method_names[ctx->emitted_method_count++] = m;

      fprintf(out, "RAE_UNUSED static void rae_log_%s_(%s val) { rae_log_stream_%s_(val); printf(\"\\n\"); }\n", m, m, m);
      fprintf(out, "RAE_UNUSED static void rae_log_stream_%s_(%s val) { printf(\"%%lld\", (long long)val); }\n", m, m);
      fprintf(out, "RAE_UNUSED static const char* rae_str_%s_(%s val) { (void)val; return \"Enum\"; }\n\n", m, m);
    }
  }
  
  size_t last_gen_count = 0;
  do {
      last_gen_count = ctx->generic_type_count;
      emit_generic_helpers(ctx, out);
  } while (ctx->generic_type_count > last_gen_count);
  
  return true;
}

static bool emit_struct_defs(CompilerContext* ctx, const AstModule* module, FILE* out, bool uses_raylib) {
  static char* emitted_m[1024]; size_t count_m = 0;
  size_t processed_generic = 0;
  
  // Loop as long as we have unprocessed generic types.
  // emit_specialized_struct_def might call register_generic_type, increasing ctx->generic_type_count.
  while (processed_generic < ctx->generic_type_count) {
    size_t current_count = ctx->generic_type_count;
    for (size_t i = processed_generic; i < current_count; i++) {
      const char* m = rae_mangle_type(ctx, NULL, ctx->generic_types[i]);
      bool dup = false;
      for (size_t j = 0; j < count_m; j++) {
        if (strcmp(emitted_m[j], m) == 0) { dup = true; break; }
      }
      if (dup) continue;
      
      emitted_m[count_m++] = (char*)m; 
      emit_specialized_struct_def(ctx, module, ctx->generic_types[i], out, uses_raylib);
    }
    processed_generic = current_count;
  }
  Str emitted_t[512]; size_t count_t = 0;
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind == AST_DECL_TYPE && !d->as.type_decl.generic_params) emit_single_struct_def(ctx, module, d, out, emitted_t, &count_t, uses_raylib);
  }
  return true;
}

static bool emit_enum_defs(CompilerContext* ctx, const AstModule* module, FILE* out, bool uses_raylib) {
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind == AST_DECL_ENUM) {
      if (is_primitive_type(d->as.enum_decl.name) || (uses_raylib && is_raylib_builtin_type(d->as.enum_decl.name))) continue;
      fprintf(out, "typedef enum {\n");
      for (const AstEnumMember* m = d->as.enum_decl.members; m; m = m->next) fprintf(out, "  %.*s_%.*s%s\n", (int)d->as.enum_decl.name.len, d->as.enum_decl.name.data, (int)m->name.len, m->name.data, m->next ? "," : "");
      fprintf(out, "} rae_%.*s;\n\n", (int)d->as.enum_decl.name.len, d->as.enum_decl.name.data);
    }
  }
  return true;
}

bool c_backend_emit_module(CompilerContext* ctx, const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {

  ctx->all_decl_count = 0;
  ctx->generic_type_count = 0;
  ctx->emitted_generic_type_count = 0;
  ctx->specialized_func_count = 0;
  ctx->emitted_method_count = 0;
  ctx->current_module = (AstModule*)module;

  // Aggregate all declarations from graph and entry module
  collect_decls_from_module(ctx, module);
  for (const AstDecl* d = module->decls; d; d = d->next) register_decl(ctx, d);

  // Deep recursive discovery of types and specializations
  collect_type_refs_module(ctx);
  
  // Specialization pass for all collected declarations (not just the entry module)
  size_t processed_funcs = 0;
  size_t processed_specs = 0;
  while (true) {
      size_t start_generic = ctx->generic_type_count;
      size_t start_funcs = ctx->specialized_func_count;
      
      // Discover from non-generic functions
      for (size_t i = processed_funcs; i < ctx->all_decl_count; i++) {
          const AstDecl* d = ctx->all_decls[i];
          if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params) {
              CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = &d->as.func_decl};
              if (d->as.func_decl.body) discover_specializations_stmt(&fctx, d->as.func_decl.body->first);
          }
      }
      processed_funcs = ctx->all_decl_count;

      // Discover from specialized functions recursively
      while (processed_specs < ctx->specialized_func_count) {
          size_t limit = ctx->specialized_func_count;
          for (size_t i = processed_specs; i < limit; i++) {
              const AstFuncDecl* f = ctx->specialized_funcs[i].decl;
              const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
              CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = f, .generic_params = f->generic_params, .generic_args = args};
              if (f->body) discover_specializations_stmt(&fctx, f->body->first);
          }
          processed_specs = limit;
      }

      if (ctx->generic_type_count == start_generic && ctx->specialized_func_count == start_funcs) break;
  }

  FILE* out = fopen(out_path, "w"); if (!out) return false;

  bool uses_raylib = false;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) {
      if (str_eq_cstr(imp->path, "raylib")) { uses_raylib = true; break; }
  }
  // Check entry module imports as well (redundant but safe)
  if (!uses_raylib) {
      for (const AstImport* imp = module->imports; imp; imp = imp->next) {
          char* p = str_to_cstr(imp->path);
          if (p && (strcmp(p, "raylib") == 0 || str_ends_with_cstr(imp->path, "/raylib.rae") || str_ends_with_cstr(imp->path, "\\raylib.rae"))) { uses_raylib = true; free(p); break; }
          free(p);
      }
  }

  if (uses_raylib) { if (out_uses_raylib) *out_uses_raylib = true; fprintf(out, "#ifndef RAE_HAS_RAYLIB\n#define RAE_HAS_RAYLIB\n#endif\n#include <raylib.h>\n"); }

  fprintf(out, "#include \"rae_runtime.h\"\n\n");
  fprintf(out, "typedef const char* const_char_p;\n");
  fprintf(out, "#define LIFT_TO_TEMP(T, ...) (&(T){__VA_ARGS__})\n");
  fprintf(out, "#define LIFT_TO_TEMP_STRUCT(T, ...) ((T){__VA_ARGS__})\n");
  fprintf(out, "#define LIFT_VALUE(T, ...) ((T){__VA_ARGS__})\n\n");

  fprintf(out, "// Built-in buffer functions\n");
  fprintf(out, "RAE_UNUSED void* rae_ext_rae_buf_alloc(int64_t size, int64_t elemSize);\n");
  fprintf(out, "RAE_UNUSED void rae_ext_rae_buf_free(void* ptr);\n");
  fprintf(out, "RAE_UNUSED void* rae_ext_rae_buf_resize(void* ptr, int64_t new_size, int64_t elemSize);\n");
  fprintf(out, "RAE_UNUSED void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elemSize);\n\n");

  // Forward declarations for all specialized generic types
  fprintf(out, "// Forward declarations for specialized generics\n");
  for (size_t i = 0; i < ctx->generic_type_count; i++) {
      const char* m = rae_mangle_type(ctx, NULL, ctx->generic_types[i]);
      fprintf(out, "typedef struct %s %s;\n", m, m);
  }
  // Also common map entries
  fprintf(out, "typedef struct rae_StringMapEntry_RaeAny rae_StringMapEntry_RaeAny;\n");
  fprintf(out, "typedef struct rae_IntMapEntry_RaeAny rae_IntMapEntry_RaeAny;\n");
  fprintf(out, "typedef struct rae_MapEntry_RaeAny rae_MapEntry_RaeAny;\n\n");

  emit_enum_defs(ctx, module, out, uses_raylib);
  emit_struct_defs(ctx, module, out, uses_raylib);
  emit_json_methods(ctx, module, out, uses_raylib);

  

  // Prototypes for ALL functions (non-generic first)
  const char* emitted_names[2048];
  size_t emitted_count = 0;

  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC) {
          const AstFuncDecl* f = &d->as.func_decl;
          if (f->generic_params != NULL) continue;
          
          const char* mangled = rae_mangle_function(ctx, f);
          bool dup = false;
          for (size_t j = 0; j < emitted_count; j++) if (strcmp(emitted_names[j], mangled) == 0) { dup = true; break; }
          if (dup) continue;
          if (emitted_count < 2048) emitted_names[emitted_count++] = mangled;

          CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .func_decl = f, .uses_raylib = uses_raylib};
          const char* rt = c_return_type(&tctx, f);
          
          if (str_eq_cstr(f->name, "main")) {
              fprintf(out, "int main(void);\n");
          } else {
              bool is_ext = f->is_extern || str_starts_with_cstr(f->name, "rae_ext_");
              fprintf(out, "RAE_UNUSED %s %s %s(", is_ext ? "" : "static", rt, mangled);
              emit_param_list(&tctx, f->params, out, is_ext);
              fprintf(out, ");\n");
          }
      }
  }

  // Prototypes for ALL specialized functions (pre-discovered)
  size_t emitted_protos = 0;
  while (emitted_protos < ctx->specialized_func_count) {
      size_t current_limit = ctx->specialized_func_count;
      for (size_t i = emitted_protos; i < current_limit; i++) {
          const AstFuncDecl* f = ctx->specialized_funcs[i].decl;
          const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
          
          const char* mangled = rae_mangle_specialized_function(ctx, f, args);
          // bool dup = false;
          // for (size_t j = 0; j < emitted_count; j++) if (strcmp(emitted_names[j], mangled) == 0) { dup = true; break; }
          // if (dup) continue;
          // if (emitted_count < 2048) emitted_names[emitted_count++] = mangled;

          CFuncContext tctx = {.compiler_ctx = ctx, .module = module, .func_decl = f, .uses_raylib = uses_raylib, .generic_params = f->generic_params, .generic_args = args};
          const char* rt = c_return_type(&tctx, f);
          fprintf(out, "RAE_UNUSED static %s %s(", rt, mangled);
          const AstParam* p = f->params;
          while (p) {
              emit_type_ref_as_c_type(&tctx, p->type, out, false);
              fprintf(out, " %.*s%s", (int)p->name.len, p->name.data, p->next ? ", " : "");
              p = p->next;
          }
          if (!f->params) fprintf(out, "void");
          fprintf(out, ");\n");
      }
      emitted_protos = current_limit;
  }

  fprintf(out, "\n");

  // Bodies for ALL functions
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
      const AstDecl* d = ctx->all_decls[i];
      if (d->kind == AST_DECL_FUNC) {
          const AstFuncDecl* f = &d->as.func_decl;
          if (f->generic_params != NULL) continue;
          emit_function(ctx, module, f, out, registry, uses_raylib); 
      }
  }

  // Bodies for specialized functions (handled in a loop to support recursive specializations)
  size_t emitted_specialized = 0;
  while (emitted_specialized < ctx->specialized_func_count) {
      size_t current_limit = ctx->specialized_func_count;
      for (size_t i = emitted_specialized; i < current_limit; i++) {
          emit_specialized_function(ctx, module, ctx->specialized_funcs[i].decl, ctx->specialized_funcs[i].concrete_args, out, registry, uses_raylib);
      }
      emitted_specialized = current_limit;
  }

  fclose(out); return true;
}

static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  fprintf(out, "  if ("); emit_expr(ctx, stmt->as.if_stmt.condition, out, PREC_LOWEST, false, false); fprintf(out, ") {\n");
  emit_block(ctx, stmt->as.if_stmt.then_block, out); fprintf(out, "  }");
  if (stmt->as.if_stmt.else_block) { fprintf(out, " else {\n"); emit_block(ctx, stmt->as.if_stmt.else_block, out); fprintf(out, "  }"); }
  fprintf(out, "\n"); return true;
}

static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  fprintf(out, "  {\n"); ctx->scope_depth++; if (stmt->as.loop_stmt.init) emit_stmt(ctx, stmt->as.loop_stmt.init, out);
  fprintf(out, "  while ("); if (stmt->as.loop_stmt.condition) emit_expr(ctx, stmt->as.loop_stmt.condition, out, PREC_LOWEST, false, false); else fprintf(out, "1");
  fprintf(out, ") {\n"); emit_block(ctx, stmt->as.loop_stmt.body, out);
  if (stmt->as.loop_stmt.increment) { fprintf(out, "  "); emit_expr(ctx, stmt->as.loop_stmt.increment, out, PREC_LOWEST, false, false); fprintf(out, ";\n"); }
  fprintf(out, "  }\n"); emit_defers(ctx, ctx->scope_depth, out); pop_defers(ctx, ctx->scope_depth); ctx->scope_depth--; fprintf(out, "  }\n"); return true;
}

static bool emit_specialized_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, const AstTypeRef* args, FILE* out, const struct VmRegistry* r, bool ray) {
    if (str_eq_cstr(f->name, "sizeof")) return true;
    if (str_eq_cstr(f->name, "__buf_alloc") || str_eq_cstr(f->name, "__buf_free") || str_eq_cstr(f->name, "__buf_copy")) return true;
    CFuncContext tctx = {.compiler_ctx = ctx, .module = m, .func_decl = f, .uses_raylib = ray, .registry = r, .generic_params = f->generic_params, .generic_args = args};
    const char* rt = c_return_type(&tctx, f);
    const char* mangled = rae_mangle_specialized_function(ctx, f, args);

    if (str_eq_cstr(f->name, "main")) { fprintf(out, "int main("); tctx.is_main = true; }
    else {
        fprintf(out, "RAE_UNUSED static %s %s(", rt, mangled);
    }
    const AstParam* p = f->params;
    while (p) {
        emit_type_ref_as_c_type(&tctx, p->type, out, false);
        fprintf(out, " %.*s%s", (int)p->name.len, p->name.data, p->next ? ", " : "");
        p = p->next;
    }
    if (!f->params) fprintf(out, "void");
    fprintf(out, ") {\n");
    
    CFuncContext cctx = tctx; cctx.return_type_name = rt; cctx.returns_value = func_has_return_value(f);
    for (const AstParam* p = f->params; p; p = p->next) {
        if (cctx.local_count < 256) {
            cctx.locals[cctx.local_count] = p->name; 
            cctx.local_type_refs[cctx.local_count] = substitute_type_ref(ctx, f->generic_params, args, p->type);
            const char* tn = rae_mangle_type_specialized(ctx, f->generic_params, args, p->type);
            cctx.local_types[cctx.local_count] = str_from_cstr(tn);
            cctx.local_is_ptr[cctx.local_count] = p->type && (p->type->is_view || p->type->is_mod);
            cctx.local_is_mod[cctx.local_count] = p->type && p->type->is_mod;
            cctx.local_count++;
        }
    }
    emit_block(&cctx, f->body, out);
    if (tctx.is_main) fprintf(out, "  return 0;\n");
    fprintf(out, "}\n\n");
    return true;
}

static bool emit_function(CompilerContext* ctx, const AstModule* m, const AstFuncDecl* f, FILE* out, const struct VmRegistry* r, bool ray) {
  if (f->is_extern || str_starts_with_cstr(f->name, "rae_ext_")) return true;
  CFuncContext tctx = {.compiler_ctx = ctx, .module = m, .func_decl = f, .uses_raylib = ray, .registry = r};
  const char* rt = c_return_type(&tctx, f);
  const char* mangled = rae_mangle_function(ctx, f);

  if (str_eq_cstr(f->name, "main")) { fprintf(out, "int main("); tctx.is_main = true; }
  else {
      bool is_ext = f->is_extern || str_starts_with_cstr(f->name, "rae_ext_");
      fprintf(out, "RAE_UNUSED %s %s ", is_ext ? "" : "static", rt);
      fprintf(out, "%s", mangled);
      fprintf(out, "(");
  }
  emit_param_list(&tctx, f->params, out, f->is_extern || str_starts_with_cstr(f->name, "rae_ext_"));
  fprintf(out, ") {\n");
  CFuncContext cctx = tctx; cctx.return_type_name = rt; cctx.returns_value = func_has_return_value(f);
  for (const AstParam* p = f->params; p; p = p->next) {
      if (cctx.local_count < 256) {
          cctx.locals[cctx.local_count] = p->name; cctx.local_type_refs[cctx.local_count] = p->type;
          const char* tn = rae_mangle_type_specialized(ctx, f->generic_params, NULL, p->type);
          cctx.local_types[cctx.local_count] = str_from_cstr(tn);
          cctx.local_is_ptr[cctx.local_count] = p->type && (p->type->is_view || p->type->is_mod);
          cctx.local_is_mod[cctx.local_count] = p->type && p->type->is_mod;
          cctx.local_count++;
      }
  }
  emit_block(&cctx, f->body, out);
  if (tctx.is_main) fprintf(out, "  return 0;\n"); fprintf(out, "}\n\n"); return true;
}

static bool emit_single_struct_def(CompilerContext* ctx, const AstModule* m, const AstDecl* d, FILE* out, Str* et, size_t* ec, bool ray) {
  for (size_t i = 0; i < *ec; i++) if (str_eq(et[i], d->as.type_decl.name)) return true;
  Str ns = d->as.type_decl.name; if (is_primitive_type(ns) || (ray && is_raylib_builtin_type(ns))) { et[(*ec)++] = ns; return true; }
  if (has_property(d->as.type_decl.properties, "c_struct")) { et[(*ec)++] = ns; return true; }
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    if (f->type && f->type->parts && !f->type->is_view && !f->type->is_mod) {
      const AstDecl* dep = find_type_decl(NULL, m, f->type->parts->text);
      if (dep) emit_single_struct_def(ctx, m, dep, out, et, ec, ray);
    }
  }
  char* n = str_to_cstr(ns); fprintf(out, "typedef struct rae_%s rae_%s;\nstruct rae_%s {\n", n, n, n);
  CFuncContext tctx = {.compiler_ctx = ctx, .generic_params = d->as.type_decl.generic_params, .uses_raylib = ray};
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    fprintf(out, "  "); bool p = f->type && (f->type->is_view || f->type->is_mod);
    emit_type_ref_as_c_type(&tctx, f->type, out, false); fprintf(out, "%s %.*s;\n", p ? "*" : "", (int)f->name.len, f->name.data);
  }
  fprintf(out, "};\n\n"); free(n); et[(*ec)++] = ns; return true;
}

static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out) {
  for (int i = ctx->defer_stack.count - 1; i >= 0; i--) {
    if (ctx->defer_stack.entries[i].scope_depth >= min_depth) {
      if (!emit_block(ctx, ctx->defer_stack.entries[i].block, out)) return false;
    }
  }
  return true;
}

static void pop_defers(CFuncContext* ctx, int depth) {
  while (ctx->defer_stack.count > 0 && ctx->defer_stack.entries[ctx->defer_stack.count - 1].scope_depth >= depth) {
    ctx->defer_stack.count--;
  }
}

static bool emit_spawn_wrapper(CFuncContext* ctx, const AstFuncDecl* func, FILE* out) {
  (void)ctx; (void)func; (void)out;
  return true; 
}

static bool base_types_match(Str pattern, Str concrete) {
    if (str_eq(pattern, concrete)) return true;
    if (str_starts_with_cstr(concrete, "rae_")) {
        // Strip rae_ prefix and also any generic suffix starting with _ (e.g. rae_StringMap_int64_t -> StringMap)
        char* raw = str_to_cstr(concrete);
        char* start = raw + 4;
        char* end = strchr(start, '_');
        if (end) *end = '\0';
        bool match = strcmp(start, str_to_cstr(pattern)) == 0;
        free(raw);
        return match;
    }
    return false;
}

static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count, bool is_method) {
  const AstDecl** decls = NULL;
  size_t count = 0;

  if (ctx && ctx->compiler_ctx) {
      decls = ctx->compiler_ctx->all_decls;
      count = ctx->compiler_ctx->all_decl_count;
  } else if (module) {
      // Temporary fallback if context not available
      decls = (const AstDecl**)module->decls;
      // This is a bit unsafe as it's not a proper array, but we'll fix it if needed
  }

  if (!decls && module) {
      // Proper fallback: use module-based recursive search if all_decls not available
      for (const AstDecl* d = module->decls; d; d = d->next) {
          if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, name)) {
              const AstFuncDecl* f = &d->as.func_decl;
              bool has_this = f->params && str_eq_cstr(f->params->name, "this");
              if (is_method && !has_this) continue;
              if (!is_method && has_this) continue;
              uint16_t c = 0; for (const AstParam* p = f->params; p; p = p->next) c++;
              if (c == param_count) {
                  if (is_method && param_types) {
                      if (base_types_match(get_base_type_name(f->params->type), param_types[0])) return f;
                  } else if (!param_types) return f;
              }
          }
      }
      for (const AstImport* imp = module->imports; imp; imp = imp->next) {
          const AstFuncDecl* f = find_function_overload(imp->module, ctx, name, param_types, param_count, is_method);
          if (f) return f;
      }
      return NULL;
  }

  // Phase 1: Try to find exact match for method receiver (named 'this')
  for (size_t i = 0; i < count; i++) {
    const AstDecl* d = decls[i];
    if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, name)) {
      const AstFuncDecl* f = &d->as.func_decl;
      bool has_this = f->params && str_eq_cstr(f->params->name, "this");
      if (is_method && !has_this) continue;
      if (!is_method && has_this) continue;

      uint16_t c = 0; for (const AstParam* p = f->params; p; p = p->next) c++;
      if (c == param_count) {
          if (is_method && param_types) {
              Str p_base = get_base_type_name(f->params->type);
              if (base_types_match(p_base, param_types[0])) return f;
          } else if (!param_types) {
              return f;
          }
      }
    }
  }

  // Phase 2: Relaxed matching (UFCS - first param matches type but not named 'this')
  for (size_t i = 0; i < count; i++) {
    const AstDecl* d = decls[i];
    if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, name)) {
      const AstFuncDecl* f = &d->as.func_decl;
      bool has_this = f->params && str_eq_cstr(f->params->name, "this");
      if (is_method && !f->params) continue;
      if (!is_method && has_this) continue;

      uint16_t c = 0; for (const AstParam* p = f->params; p; p = p->next) c++;
      if (c == param_count) {
          if (is_method && param_types) {
              if (types_match(get_base_type_name(f->params->type), param_types[0])) return f;
          } else if (!param_types) {
              return f;
          }
      }
    }
  }

  // Phase 3: Generic fallback or name-only match
  for (size_t i = 0; i < count; i++) {
    const AstDecl* d = decls[i];
    if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, name)) {
      const AstFuncDecl* f = &d->as.func_decl;
      bool has_this = f->params && str_eq_cstr(f->params->name, "this");
      if (is_method && !f->params) continue;
      if (!is_method && has_this) continue;

      uint16_t c = 0; for (const AstParam* p = f->params; p; p = p->next) c++;
      if (c == param_count) {
          if (!param_types) return f;
          bool match = true;
          const AstParam* p = f->params;
          for (uint16_t i = 0; i < param_count; i++) {
              Str p_base = get_base_type_name(p->type);
              // Allow generic parameters to match anything in this phase
              bool is_gp = false;
              if (f->generic_params) {
                  for (const AstIdentifierPart* gp = f->generic_params; gp; gp = gp->next) {
                      if (str_eq(p_base, gp->text)) { is_gp = true; break; }
                  }
              }
              if (!is_gp && !types_match(p_base, param_types[i])) { match = false; break; }
              p = p->next;
          }
          if (match) return f;
      }
    }
  }
  
  return NULL;
}

static bool is_wildcard_pattern(const AstExpr* expr) {
  return expr && expr->kind == AST_EXPR_IDENT && str_eq_cstr(expr->as.ident, "_");
}

static bool is_string_literal_expr(const AstExpr* expr) {
  return expr && expr->kind == AST_EXPR_STRING;
}

static bool match_cases_use_string(const AstMatchCase* cases, bool* out_use_string) {
  bool s = false, o = false;
  for (const AstMatchCase* c = cases; c; c = c->next) {
    if (c->pattern) { if (is_string_literal_expr(c->pattern)) s = true; else o = true; }
  }
  if (s && o) return false;
  *out_use_string = s; return true;
}

static bool match_arms_use_string(const AstMatchArm* arms, bool* out_use_string) {
  bool s = false, o = false;
  for (const AstMatchArm* a = arms; a; a = a->next) {
    if (a->pattern) { if (is_string_literal_expr(a->pattern)) s = true; else o = true; }
  }
  if (s && o) return false;
  *out_use_string = s; return true;
}

static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt->as.match_stmt.subject || !stmt->as.match_stmt.cases) return false;
  bool use_str = false; match_cases_use_string(stmt->as.match_stmt.cases, &use_str);
  
  const AstTypeRef* tr = infer_expr_type_ref(ctx, stmt->as.match_stmt.subject);
  bool use_any = tr && tr->is_opt;

  size_t tid = ctx->temp_counter++;
  if (use_any) fprintf(out, "  RaeAny __m%zu = ", tid);
  else if (use_str) fprintf(out, "  const char* __m%zu = ", tid);
  else fprintf(out, "  int64_t __m%zu = ", tid);
  
  emit_expr(ctx, stmt->as.match_stmt.subject, out, PREC_LOWEST, false, false); fprintf(out, ";\n");
  int ci = 0; const AstMatchCase* def = NULL;
  for (const AstMatchCase* c = stmt->as.match_stmt.cases; c; c = c->next) {
    if (!c->pattern || is_wildcard_pattern(c->pattern)) { def = c; continue; }
    if (use_any) {
        fprintf(out, "%s(rae_any_eq(__m%zu, ", ci > 0 ? " else if " : "  if ", tid);
        emit_expr(ctx, c->pattern, out, PREC_LOWEST, false, false);
        fprintf(out, ")) {\n");
    } else if (use_str) {
        fprintf(out, "%s(rae_ext_rae_str_eq(__m%zu, ", ci > 0 ? " else if " : "  if ", tid);
        emit_expr(ctx, c->pattern, out, PREC_LOWEST, false, false);
        fprintf(out, ")) {\n");
    } else {
        fprintf(out, "%s(__m%zu == ", ci > 0 ? " else if " : "  if ", tid);
        emit_expr(ctx, c->pattern, out, PREC_LOWEST, false, false);
        fprintf(out, ") {\n");
    }
    emit_block(ctx, c->block, out); fprintf(out, "  }"); ci++;
  }
  if (def) { if (ci > 0) fprintf(out, " else {\n"); emit_block(ctx, def->block, out); if (ci > 0) fprintf(out, "  }"); }
  fprintf(out, "\n"); return true;
}
