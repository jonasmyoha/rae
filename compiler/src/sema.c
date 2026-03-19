#include "sema.h"
#include "type.h"
#include "ast.h"
#include "diag.h"
#include "mangler.h"
#include "c_backend.h"
#include <string.h>

typedef struct Symbol Symbol;
struct Symbol {
    Str name;
    AstDecl* decl;
    TypeInfo* type;
    int scope_depth;
    bool is_immutable;
    Symbol* next;
};

typedef struct SymbolTable {
    Symbol* head;
    int current_depth;
} SymbolTable;

static void symbol_table_push_scope(SymbolTable* table) {
    table->current_depth++;
}

static void symbol_table_pop_scope(SymbolTable* table) {
    while (table->head && table->head->scope_depth == table->current_depth) {
        table->head = table->head->next;
    }
    table->current_depth--;
}

static void symbol_table_define(SymbolTable* table, Arena* arena, Str name, AstDecl* decl, TypeInfo* type, bool is_immutable) {
    Symbol* sym = arena_alloc(arena, sizeof(Symbol));
    sym->name = name;
    sym->decl = decl;
    sym->type = type;
    sym->scope_depth = table->current_depth;
    sym->is_immutable = is_immutable;
    sym->next = table->head;
    table->head = sym;
}

static Symbol* symbol_table_lookup(SymbolTable* table, Str name) {
    Symbol* curr = table->head;
    while (curr) {
        if (str_eq(curr->name, name)) return curr;
        curr = curr->next;
    }
    return NULL;
}

typedef struct InstantiationEntry {
    const char* file_path;
    size_t line;
    size_t column;
    struct InstantiationEntry* next;
} InstantiationEntry;

typedef struct InstantiationStack {
    InstantiationEntry* head;
} InstantiationStack;

// Forward declarations
static void sema_analyze_decl(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* decl);
static void sema_analyze_expr(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* expr);
static void sema_analyze_stmt(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt, TypeInfo* current_return_type);
static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstTypeRef* type_ref);
static void ensure_type_match(CompilerContext* ctx, TypeInfo* expected, AstExpr** expr_ptr);
static AstDecl* specialize_decl(CompilerContext* ctx, AstModule* module, AstDecl* generic_decl, TypeInfo** args, size_t arg_count, size_t line, size_t column);

AstIdentifierPart* clone_parts(CompilerContext* ctx, const AstIdentifierPart* p) {
    if (!p) return NULL;
    AstIdentifierPart* res = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
    *res = *p;
    res->next = clone_parts(ctx, p->next);
    return res;
}

static AstTypeRef* clone_type_ref(Arena* arena, const AstTypeRef* tr) {
    if (!tr) return NULL;
    AstTypeRef* res = arena_alloc(arena, sizeof(AstTypeRef));
    *res = *tr;
    if (tr->parts) {
        AstIdentifierPart* head = NULL; AstIdentifierPart* tail = NULL; AstIdentifierPart* curr = tr->parts;
        while (curr) {
            AstIdentifierPart* p = arena_alloc(arena, sizeof(AstIdentifierPart)); *p = *curr;
            if (!head) head = p; if (tail) tail->next = p; tail = p; curr = curr->next;
        }
        res->parts = head;
    }
    if (tr->generic_args) res->generic_args = clone_type_ref(arena, tr->generic_args);
    if (tr->next) res->next = clone_type_ref(arena, tr->next);
    return res;
}

AstTypeRef* substitute_type_ref(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type) {
    if (!type) return NULL;
    if (generic_params && concrete_args && type->parts && !type->parts->next) {
        Str base = type->parts->text;
        const AstIdentifierPart* gp = generic_params;
        const AstTypeRef* arg = concrete_args;
        while (gp && arg) {
            if (str_eq(gp->text, base)) {
                const AstTypeRef* match = arg;
                if (match->generic_args) match = substitute_type_ref(ctx, NULL, NULL, match);
                AstTypeRef* result = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                *result = *match;
                result->next = NULL;
                result->parts = clone_parts(ctx, match->parts);
                if (type->is_view) result->is_view = true;
                if (type->is_mod) result->is_mod = true;
                if (type->is_opt) result->is_opt = true;
                return result;
            }
            gp = gp->next; arg = arg->next;
        }
    }
    if (type->generic_args) {
        AstTypeRef* new_type = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
        *new_type = *type;
        new_type->next = NULL;
        new_type->parts = clone_parts(ctx, type->parts);
        AstTypeRef* sub_args = NULL;
        AstTypeRef* last_sub = NULL;
        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {
            AstTypeRef* sub = substitute_type_ref(ctx, generic_params, concrete_args, a);
            if (!sub_args) sub_args = sub; else last_sub->next = sub;
            last_sub = sub;
        }
        new_type->generic_args = sub_args;
        if (type->is_view) new_type->is_view = true;
        if (type->is_mod) new_type->is_mod = true;
        if (type->is_opt) new_type->is_opt = true;
        return new_type;
    }
    AstTypeRef* result = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
    *result = *type;
    result->next = NULL;
    result->parts = clone_parts(ctx, type->parts);
    return result;
}

static AstCallArg* clone_call_args(Arena* arena, const AstCallArg* arg);
static AstExpr* clone_expr(Arena* arena, const AstExpr* expr) {
    if (!expr) return NULL;
    AstExpr* res = arena_alloc(arena, sizeof(AstExpr));
    *res = *expr;
    switch (expr->kind) {
        case AST_EXPR_BINARY:
            res->as.binary.lhs = clone_expr(arena, expr->as.binary.lhs);
            res->as.binary.rhs = clone_expr(arena, expr->as.binary.rhs);
            break;
        case AST_EXPR_UNARY:
            res->as.unary.operand = clone_expr(arena, expr->as.unary.operand);
            break;
        case AST_EXPR_CALL:
            res->as.call.callee = clone_expr(arena, expr->as.call.callee);
            res->as.call.args = clone_call_args(arena, expr->as.call.args);
            res->as.call.generic_args = clone_type_ref(arena, expr->as.call.generic_args);
            break;
        case AST_EXPR_MEMBER:
            res->as.member.object = clone_expr(arena, expr->as.member.object);
            break;
        case AST_EXPR_INDEX:
            res->as.index.target = clone_expr(arena, expr->as.index.target);
            res->as.index.index = clone_expr(arena, expr->as.index.index);
            break;
        case AST_EXPR_METHOD_CALL:
            res->as.method_call.object = clone_expr(arena, expr->as.method_call.object);
            res->as.method_call.args = clone_call_args(arena, expr->as.method_call.args);
            res->as.method_call.generic_args = clone_type_ref(arena, expr->as.method_call.generic_args);
            break;
        case AST_EXPR_OBJECT:
            res->as.object_literal.type = clone_type_ref(arena, expr->as.object_literal.type);
            {
                AstObjectField* head = NULL; AstObjectField* tail = NULL;
                AstObjectField* curr = expr->as.object_literal.fields;
                while (curr) {
                    AstObjectField* f = arena_alloc(arena, sizeof(AstObjectField)); *f = *curr;
                    f->value = clone_expr(arena, curr->value); f->next = NULL;
                    if (!head) head = f; else tail->next = f; tail = f; curr = curr->next;
                }
                res->as.object_literal.fields = head;
            }
            break;
        default: break;
    }
    return res;
}

static AstCallArg* clone_call_args(Arena* arena, const AstCallArg* arg) {
    if (!arg) return NULL;
    AstCallArg* res = arena_alloc(arena, sizeof(AstCallArg));
    *res = *arg;
    res->value = clone_expr(arena, arg->value);
    res->next = clone_call_args(arena, arg->next);
    return res;
}

static AstStmt* clone_stmt(Arena* arena, const AstStmt* stmt);
static AstBlock* clone_block(Arena* arena, const AstBlock* block) {
    if (!block) return NULL;
    AstBlock* res = arena_alloc(arena, sizeof(AstBlock));
    AstStmt* head = NULL; AstStmt* tail = NULL;
    AstStmt* curr = block->first;
    while (curr) {
        AstStmt* s = clone_stmt(arena, curr);
        if (!head) head = s; else tail->next = s;
        tail = s; curr = curr->next;
    }
    res->first = head;
    return res;
}

static AstStmt* clone_stmt(Arena* arena, const AstStmt* stmt) {
    if (!stmt) return NULL;
    AstStmt* res = arena_alloc(arena, sizeof(AstStmt));
    *res = *stmt;
    res->next = NULL;
    switch (stmt->kind) {
        case AST_STMT_EXPR: res->as.expr_stmt = clone_expr(arena, stmt->as.expr_stmt); break;
        case AST_STMT_LET: res->as.let_stmt.value = clone_expr(arena, stmt->as.let_stmt.value); break;
        case AST_STMT_RET: {
            AstReturnArg* head = NULL; AstReturnArg* tail = NULL;
            AstReturnArg* curr = stmt->as.ret_stmt.values;
            while (curr) {
                AstReturnArg* a = arena_alloc(arena, sizeof(AstReturnArg));
                *a = *curr; a->value = clone_expr(arena, curr->value); a->next = NULL;
                if (!head) head = a; else tail->next = a;
                tail = a; curr = curr->next;
            }
            res->as.ret_stmt.values = head;
            break;
        }
        case AST_STMT_IF:
            res->as.if_stmt.condition = clone_expr(arena, stmt->as.if_stmt.condition);
            res->as.if_stmt.then_block = clone_block(arena, stmt->as.if_stmt.then_block);
            res->as.if_stmt.else_block = clone_block(arena, stmt->as.if_stmt.else_block);
            break;
        case AST_STMT_LOOP:
            res->as.loop_stmt.init = clone_stmt(arena, stmt->as.loop_stmt.init);
            res->as.loop_stmt.condition = clone_expr(arena, stmt->as.loop_stmt.condition);
            res->as.loop_stmt.increment = clone_expr(arena, stmt->as.loop_stmt.increment);
            res->as.loop_stmt.body = clone_block(arena, stmt->as.loop_stmt.body);
            break;
        case AST_STMT_ASSIGN:
            res->as.assign_stmt.target = clone_expr(arena, stmt->as.assign_stmt.target);
            res->as.assign_stmt.value = clone_expr(arena, stmt->as.assign_stmt.value);
            break;
        default: break;
    }
    return res;
}

static AstDecl* specialize_decl(CompilerContext* ctx, AstModule* module, AstDecl* generic_decl, TypeInfo** args, size_t arg_count, size_t line, size_t column) {
    AstDecl* existing = type_registry_find_specialization(ctx->type_registry, generic_decl, args, arg_count);
    if (existing) return existing;
    AstDecl* spec = arena_alloc(ctx->ast_arena, sizeof(AstDecl));
    *spec = *generic_decl; spec->next = NULL;
    if (spec->kind == AST_DECL_FUNC) spec->as.func_decl.generic_template = generic_decl;
    else if (spec->kind == AST_DECL_TYPE) spec->as.type_decl.generic_template = generic_decl;
    AstTypeRef* args_tr = NULL; AstTypeRef* last_tr = NULL;
    for (size_t i = 0; i < arg_count; i++) {
        AstTypeRef* tr = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
        tr->resolved_type = args[i]; tr->next = NULL;
        tr->parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
        tr->parts->text = args[i]->name;
        if (!args_tr) args_tr = tr; else last_tr->next = tr;
        last_tr = tr;
    }
    if (generic_decl->kind == AST_DECL_FUNC) {
        spec->as.func_decl.specialization_args = args_tr;
        AstParam* head = NULL; AstParam* tail = NULL; AstParam* p = generic_decl->as.func_decl.params;
        while (p) {
            AstParam* np = arena_alloc(ctx->ast_arena, sizeof(AstParam));
            *np = *p; np->type = substitute_type_ref(ctx, generic_decl->as.func_decl.generic_params, args_tr, p->type); np->next = NULL;
            if (!head) head = np; else tail->next = np;
            tail = np; p = p->next;
        }
        spec->as.func_decl.params = head;
        if (generic_decl->as.func_decl.returns) {
            AstReturnItem* head_ret = NULL; AstReturnItem* tail_ret = NULL; AstReturnItem* r = generic_decl->as.func_decl.returns;
            while (r) {
                AstReturnItem* nr = arena_alloc(ctx->ast_arena, sizeof(AstReturnItem));
                *nr = *r; nr->type = substitute_type_ref(ctx, generic_decl->as.func_decl.generic_params, args_tr, r->type); nr->next = NULL;
                if (!head_ret) head_ret = nr; else tail_ret->next = nr;
                tail_ret = nr; r = r->next;
            }
            spec->as.func_decl.returns = head_ret;
        }
        spec->as.func_decl.body = clone_block(ctx->ast_arena, generic_decl->as.func_decl.body);
    } else if (generic_decl->kind == AST_DECL_TYPE) {
        spec->as.type_decl.specialization_args = args_tr;
        AstTypeField* head = NULL; AstTypeField* tail = NULL; AstTypeField* f = generic_decl->as.type_decl.fields;
        while (f) {
            AstTypeField* nf = arena_alloc(ctx->ast_arena, sizeof(AstTypeField));
            *nf = *f; nf->type = substitute_type_ref(ctx, generic_decl->as.type_decl.generic_params, args_tr, f->type);
            nf->default_value = clone_expr(ctx->ast_arena, f->default_value); nf->next = NULL;
            if (!head) head = nf; else tail->next = nf;
            tail = nf; f = f->next;
        }
        spec->as.type_decl.fields = head;
        
        AstTypeRef* tr = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
        tr->parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
        tr->parts->text = generic_decl->as.type_decl.name;
        tr->generic_args = args_tr;
        tr->resolved_type = type_get_struct(ctx->type_registry, generic_decl, args, arg_count);
        register_generic_type(ctx, tr);
    }
    type_registry_add_specialization(ctx->type_registry, generic_decl, args, arg_count, spec);
    if (module) { spec->next = module->decls; module->decls = spec; }
    if (generic_decl->kind == AST_DECL_FUNC) {
        register_function_specialization(ctx, &generic_decl->as.func_decl, args_tr);
    }
    return spec;
}

Str get_base_type_name(const AstTypeRef* type) {
    if (!type || !type->parts) return (Str){0};
    return type->parts->text;
}

Str get_decl_name(const AstDecl* d) {
    if (!d) return (Str){0};
    switch (d->kind) {
        case AST_DECL_TYPE: return d->as.type_decl.name;
        case AST_DECL_FUNC: return d->as.func_decl.name;
        case AST_DECL_ENUM: return d->as.enum_decl.name;
        case AST_DECL_GLOBAL_LET: return d->as.let_decl.name;
        default: return (Str){0};
    }
}

AstTypeRef* infer_generic_args(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef* pattern, const AstTypeRef* concrete_type) {
    if (!func || !func->generic_params || !pattern || !concrete_type) return NULL;
    Str pattern_base = get_base_type_name(pattern);
    Str receiver_base = get_base_type_name(concrete_type);
    if (!str_eq(pattern_base, receiver_base)) return NULL;
    AstTypeRef* inferred_list = NULL; AstTypeRef* last_inferred = NULL;
    for (const AstIdentifierPart* gp = func->generic_params; gp; gp = gp->next) {
        AstTypeRef* match = NULL; const AstTypeRef* p_arg = pattern->generic_args; const AstTypeRef* r_arg = concrete_type->generic_args;
        while (p_arg && r_arg) {
            Str p_name = get_base_type_name(p_arg);
            if (str_eq(p_name, gp->text)) { match = (AstTypeRef*)r_arg; break; }
            p_arg = p_arg->next; r_arg = r_arg->next;
        }
        if (match) {
            AstTypeRef* copy = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
            *copy = *match; copy->next = NULL;
            if (!inferred_list) inferred_list = copy; else last_inferred->next = copy;
            last_inferred = copy;
        } else return NULL;
    }
    return inferred_list;
}

static void sema_analyze_decl(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* decl) {
    if (!decl) return;
    switch (decl->kind) {
        case AST_DECL_TYPE: {
            symbol_table_push_scope(symbols);
            if (decl->as.type_decl.specialization_args && decl->as.type_decl.generic_template) {
                AstDecl* gt = decl->as.type_decl.generic_template;
                AstIdentifierPart* gp = gt->as.type_decl.generic_params;
                AstTypeRef* arg = decl->as.type_decl.specialization_args;
                while (gp && arg) {
                    TypeInfo* concrete_t = sema_resolve_type_internal(ctx, module, symbols, arg);
                    symbol_table_define(symbols, ctx->ast_arena, gp->text, NULL, concrete_t, true);
                    gp = gp->next; arg = arg->next;
                }
            }
            AstTypeField* field = decl->as.type_decl.fields;
            while (field) {
                if (field->type) sema_resolve_type_internal(ctx, module, symbols, field->type);
                if (field->default_value) sema_analyze_expr(ctx, module, symbols, field->default_value);
                field = field->next;
            }
            symbol_table_pop_scope(symbols);
            break;
        }
        case AST_DECL_FUNC: {
            symbol_table_push_scope(symbols);
            if (decl->as.func_decl.specialization_args && decl->as.func_decl.generic_template) {
                AstDecl* gt = decl->as.func_decl.generic_template;
                AstIdentifierPart* gp = gt->as.func_decl.generic_params;
                AstTypeRef* arg = decl->as.func_decl.specialization_args;
                while (gp && arg) {
                    TypeInfo* concrete_t = sema_resolve_type_internal(ctx, module, symbols, arg);
                    symbol_table_define(symbols, ctx->ast_arena, gp->text, NULL, concrete_t, true);
                    gp = gp->next; arg = arg->next;
                }
            }
            TypeInfo* current_return_type = type_get_void(ctx->type_registry);
            if (decl->as.func_decl.returns) current_return_type = sema_resolve_type_internal(ctx, module, symbols, decl->as.func_decl.returns->type);
            AstParam* param = decl->as.func_decl.params;
            while (param) {
                TypeInfo* t = sema_resolve_type_internal(ctx, module, symbols, param->type);
                symbol_table_define(symbols, ctx->ast_arena, param->name, NULL, t, (param->type && !param->type->is_mod));
                param = param->next;
            }
            if (decl->as.func_decl.body) {
                AstStmt* stmt = decl->as.func_decl.body->first;
                while (stmt) { sema_analyze_stmt(ctx, module, symbols, stmt, current_return_type); stmt = stmt->next; }
            }
            symbol_table_pop_scope(symbols);
            break;
        }
        case AST_DECL_GLOBAL_LET: if (decl->as.let_decl.value) sema_analyze_expr(ctx, module, symbols, decl->as.let_decl.value); break;
        default: break;
    }
}

static void sema_analyze_stmt(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt, TypeInfo* current_return_type) {
    if (!stmt) return;
    switch (stmt->kind) {
        case AST_STMT_EXPR: if (stmt->as.expr_stmt) sema_analyze_expr(ctx, module, symbols, stmt->as.expr_stmt); break;
        case AST_STMT_LET: {
            TypeInfo* t = NULL;
            if (stmt->as.let_stmt.type) t = sema_resolve_type_internal(ctx, module, symbols, stmt->as.let_stmt.type);
            if (stmt->as.let_stmt.value) {
                sema_analyze_expr(ctx, module, symbols, stmt->as.let_stmt.value);
                if (!t && stmt->as.let_stmt.value->resolved_type) t = stmt->as.let_stmt.value->resolved_type;
                if (t) ensure_type_match(ctx, t, &stmt->as.let_stmt.value);
            }
            symbol_table_define(symbols, ctx->ast_arena, stmt->as.let_stmt.name, NULL, t, false);
            break;
        }
        case AST_STMT_RET: {
            AstReturnArg* arg = stmt->as.ret_stmt.values;
            while (arg) {
                if (arg->value) {
                    sema_analyze_expr(ctx, module, symbols, arg->value);
                    if (current_return_type) ensure_type_match(ctx, current_return_type, &arg->value);
                }
                arg = arg->next;
            }
            break;
        }
        case AST_STMT_IF: {
            if (stmt->as.if_stmt.condition) sema_analyze_expr(ctx, module, symbols, stmt->as.if_stmt.condition);
            if (stmt->as.if_stmt.then_block) {
                symbol_table_push_scope(symbols);
                AstStmt* s = stmt->as.if_stmt.then_block->first;
                while (s) { sema_analyze_stmt(ctx, module, symbols, s, current_return_type); s = s->next; }
                symbol_table_pop_scope(symbols);
            }
            if (stmt->as.if_stmt.else_block) {
                symbol_table_push_scope(symbols);
                AstStmt* s = stmt->as.if_stmt.else_block->first;
                while (s) { sema_analyze_stmt(ctx, module, symbols, s, current_return_type); s = s->next; }
                symbol_table_pop_scope(symbols);
            }
            break;
        }
        case AST_STMT_LOOP: {
             symbol_table_push_scope(symbols);
             if (stmt->as.loop_stmt.init) sema_analyze_stmt(ctx, module, symbols, stmt->as.loop_stmt.init, current_return_type);
             if (stmt->as.loop_stmt.condition) sema_analyze_expr(ctx, module, symbols, stmt->as.loop_stmt.condition);
             if (stmt->as.loop_stmt.increment) sema_analyze_expr(ctx, module, symbols, stmt->as.loop_stmt.increment);
             if (stmt->as.loop_stmt.body) {
                 AstStmt* s = stmt->as.loop_stmt.body->first;
                 while (s) { sema_analyze_stmt(ctx, module, symbols, s, current_return_type); s = s->next; }
             }
             symbol_table_pop_scope(symbols);
             break;
        }
        case AST_STMT_ASSIGN: {
            sema_analyze_expr(ctx, module, symbols, stmt->as.assign_stmt.target);
            sema_analyze_expr(ctx, module, symbols, stmt->as.assign_stmt.value);
            if (stmt->as.assign_stmt.target->resolved_type) ensure_type_match(ctx, stmt->as.assign_stmt.target->resolved_type, &stmt->as.assign_stmt.value);
            break;
        }
        default: break;
    }
}

static void ensure_type_match(CompilerContext* ctx, TypeInfo* expected, AstExpr** expr_ptr) {
    if (!expected || !expr_ptr || !*expr_ptr) return;
    AstExpr* expr = *expr_ptr;
    if (!expr->resolved_type) return;
    if (expected->kind == TYPE_ANY && expr->resolved_type->kind != TYPE_ANY) {
        AstExpr* box = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
        *box = (AstExpr){.kind = AST_EXPR_BOX, .resolved_type = expected, .line = expr->line, .column = expr->column};
        box->as.unary.operand = expr; *expr_ptr = box;
    } else if (expected->kind != TYPE_ANY && expr->resolved_type->kind == TYPE_ANY) {
        AstExpr* unbox = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
        *unbox = (AstExpr){.kind = AST_EXPR_UNBOX, .resolved_type = expected, .line = expr->line, .column = expr->column};
        unbox->as.unary.operand = expr; *expr_ptr = unbox;
    } else if (expected->kind != TYPE_OPT && expr->resolved_type->kind == TYPE_OPT) {
        AstExpr* unbox = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
        *unbox = (AstExpr){.kind = AST_EXPR_UNBOX, .resolved_type = expected, .line = expr->line, .column = expr->column};
        unbox->as.unary.operand = expr; *expr_ptr = unbox;
    }
}

static void sema_analyze_expr(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* expr) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_IDENT: { Symbol* sym = symbol_table_lookup(symbols, expr->as.ident); if (sym) expr->resolved_type = sym->type; break; }
        case AST_EXPR_INTEGER: expr->resolved_type = type_get_int(ctx->type_registry); break;
        case AST_EXPR_FLOAT: expr->resolved_type = type_get_float(ctx->type_registry); break;
        case AST_EXPR_BOOL: expr->resolved_type = type_get_bool(ctx->type_registry); break;
        case AST_EXPR_STRING: expr->resolved_type = type_get_string(ctx->type_registry); break;
        case AST_EXPR_CHAR: expr->resolved_type = type_get_char(ctx->type_registry); break;
        case AST_EXPR_BINARY:
            sema_analyze_expr(ctx, module, symbols, expr->as.binary.lhs); sema_analyze_expr(ctx, module, symbols, expr->as.binary.rhs);
            if (expr->as.binary.op >= AST_BIN_LT && expr->as.binary.op <= AST_BIN_OR) expr->resolved_type = type_get_bool(ctx->type_registry);
            else if (expr->as.binary.lhs && expr->as.binary.lhs->resolved_type) expr->resolved_type = expr->as.binary.lhs->resolved_type;
            break;
        case AST_EXPR_UNARY:
            sema_analyze_expr(ctx, module, symbols, expr->as.unary.operand);
            if (expr->as.unary.op == AST_UNARY_NOT) expr->resolved_type = type_get_bool(ctx->type_registry);
            else if (expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
                if (expr->as.unary.operand->resolved_type) expr->resolved_type = type_get_ref(ctx->type_registry, expr->as.unary.operand->resolved_type, expr->as.unary.op == AST_UNARY_MOD);
            } else if (expr->as.unary.operand->resolved_type) expr->resolved_type = expr->as.unary.operand->resolved_type;
            break;
        case AST_EXPR_CALL: {
            sema_analyze_expr(ctx, module, symbols, expr->as.call.callee);
            AstCallArg* arg = expr->as.call.args;
            while (arg) { sema_analyze_expr(ctx, module, symbols, arg->value); arg = arg->next; }
            if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
                Str name = expr->as.call.callee->as.ident;
                if (str_eq_cstr(name, "sizeof")) { expr->is_builtin_sizeof = true; expr->resolved_type = type_get_int(ctx->type_registry); }
                else {
                    Symbol* sym = symbol_table_lookup(symbols, name);
                    if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                        AstFuncDecl* fd = &sym->decl->as.func_decl;
                        if (fd->generic_params) {
                            AstTypeRef* inferred_args = NULL;
                            if (expr->as.call.generic_args) inferred_args = expr->as.call.generic_args;
                            else if (fd->params && str_eq_cstr(fd->params->name, "this") && expr->as.call.args) {
                                const AstExpr* first_arg = expr->as.call.args->value;
                                if (first_arg->resolved_type) {
                                    TypeInfo* rec = first_arg->resolved_type; if (rec->kind == TYPE_REF) rec = rec->as.ref.base;
                                    if (rec->kind == TYPE_STRUCT) {
                                        AstTypeRef rec_tr = { .parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart)) }; rec_tr.parts->text = rec->name;
                                        if (rec->as.structure.generic_count > 0) {
                                            AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
                                            for (size_t i = 0; i < rec->as.structure.generic_count; i++) {
                                                AstTypeRef* arg_tr = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                                                arg_tr->resolved_type = rec->as.structure.generic_args[i]; arg_tr->next = NULL;
                                                if (!head) head = arg_tr; else tail->next = arg_tr; tail = arg_tr;
                                            }
                                            rec_tr.generic_args = head;
                                        }
                                        inferred_args = infer_generic_args(ctx, fd, fd->params->type, &rec_tr);
                                    }
                                }
                            }
                            if (inferred_args) {
                                TypeInfo* type_args[16]; size_t ac = 0;
                                for (AstTypeRef* tr = inferred_args; tr && ac < 16; tr = tr->next) type_args[ac++] = sema_resolve_type_internal(ctx, module, symbols, tr);
                                AstDecl* spec = type_registry_find_specialization(ctx->type_registry, sym->decl, type_args, ac);
                                if (!spec) spec = specialize_decl(ctx, module, sym->decl, type_args, ac, expr->line, expr->column);
                                expr->decl_link = spec;
                            } else {
                                expr->decl_link = sym->decl;
                            }
                        } else expr->decl_link = sym->decl;
                        if (expr->decl_link->as.func_decl.returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, expr->decl_link->as.func_decl.returns->type);
                        AstParam* p = expr->decl_link->as.func_decl.params; AstCallArg* a = expr->as.call.args;
                        while (p && a) { TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type); ensure_type_match(ctx, pt, &a->value); p = p->next; a = a->next; }
                    }
                }
            }
            break;
        }
        case AST_EXPR_MEMBER:
            sema_analyze_expr(ctx, module, symbols, expr->as.member.object);
            if (expr->as.member.object->resolved_type) {
                TypeInfo* t = expr->as.member.object->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                if (t->kind == TYPE_STRUCT) {
                    AstDecl* d = t->as.structure.decl; AstTypeDecl* td = &d->as.type_decl;
                    AstIdentifierPart* params = td->generic_params; AstTypeRef* args = td->specialization_args;
                    if (!params && d->as.type_decl.generic_template) params = d->as.type_decl.generic_template->as.type_decl.generic_params;
                    AstTypeField* f = td->fields;
                    while (f) { if (str_eq(f->name, expr->as.member.member)) { AstTypeRef* sub = substitute_type_ref(ctx, params, args, f->type); expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, sub); break; } f = f->next; }
                }
            }
            break;
        case AST_EXPR_METHOD_CALL:
            sema_analyze_expr(ctx, module, symbols, expr->as.method_call.object);
            AstCallArg* marg = expr->as.method_call.args;
            while (marg) { sema_analyze_expr(ctx, module, symbols, marg->value); marg = marg->next; }
            if (expr->as.method_call.object->resolved_type) {
                TypeInfo* t = expr->as.method_call.object->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                bool found = false;
                for (size_t i = 0; i < ctx->methods.count; i++) {
                    MethodEntry* entry = &ctx->methods.entries[i];
                    if (str_eq(entry->type_name, t->name) && str_eq(entry->method_name, expr->as.method_call.method_name)) {
                        Symbol* sym = symbol_table_lookup(symbols, entry->actual_function_name);
                        if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                            expr->decl_link = sym->decl; if (sym->decl->as.func_decl.returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, sym->decl->as.func_decl.returns->type);
                            found = true;
                        }
                        break;
                    }
                }
                if (!found) {
                    Symbol* sym = symbol_table_lookup(symbols, expr->as.method_call.method_name);
                    if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                        AstFuncDecl* fd = &sym->decl->as.func_decl;
                        if (fd->generic_params) {
                            TypeInfo* rec = expr->as.method_call.object->resolved_type; if (rec->kind == TYPE_REF) rec = rec->as.ref.base;
                            AstTypeRef rec_tr = { .parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart)) }; rec_tr.parts->text = rec->name;
                            if (rec->kind == TYPE_STRUCT && rec->as.structure.generic_count > 0) {
                                AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
                                for (size_t j = 0; j < rec->as.structure.generic_count; j++) {
                                    AstTypeRef* arg_tr = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                                    arg_tr->resolved_type = rec->as.structure.generic_args[j]; arg_tr->next = NULL;
                                    if (!head) head = arg_tr; else tail->next = arg_tr; tail = arg_tr;
                                }
                                rec_tr.generic_args = head;
                            }
                            AstTypeRef* ga = infer_generic_args(ctx, fd, fd->params->type, &rec_tr);
                            if (ga) {
                                TypeInfo* type_args[16]; size_t ac = 0;
                                for (AstTypeRef* tr = ga; tr && ac < 16; tr = tr->next) type_args[ac++] = sema_resolve_type_internal(ctx, module, symbols, tr);
                                AstDecl* spec = type_registry_find_specialization(ctx->type_registry, sym->decl, type_args, ac);
                                if (!spec) spec = specialize_decl(ctx, module, sym->decl, type_args, ac, expr->line, expr->column);
                                expr->decl_link = spec;
                            } else {
                                expr->decl_link = sym->decl;
                            }
                        } else expr->decl_link = sym->decl;
                        if (expr->decl_link->as.func_decl.returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, expr->decl_link->as.func_decl.returns->type);
                        found = true;
                    }
                }
                if (found && expr->decl_link) {
                    AstParam* p = expr->decl_link->as.func_decl.params; if (p) p = p->next;
                    AstCallArg* a = expr->as.method_call.args;
                    while (p && a) { TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type); ensure_type_match(ctx, pt, &a->value); p = p->next; a = a->next; }
                }
            }
            break;
        case AST_EXPR_INDEX:
            sema_analyze_expr(ctx, module, symbols, expr->as.index.target); sema_analyze_expr(ctx, module, symbols, expr->as.index.index);
            if (expr->as.index.target->resolved_type) {
                TypeInfo* t = expr->as.index.target->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                if (t->kind == TYPE_BUFFER) expr->resolved_type = t->as.buffer.base;
                else if (t->kind == TYPE_STRUCT && str_eq_cstr(t->name, "List") && t->as.structure.generic_count > 0) expr->resolved_type = t->as.structure.generic_args[0];
            }
            break;
        case AST_EXPR_BOX: sema_analyze_expr(ctx, module, symbols, expr->as.unary.operand); break;
        case AST_EXPR_UNBOX: sema_analyze_expr(ctx, module, symbols, expr->as.unary.operand); break;
        case AST_EXPR_OBJECT: if (expr->as.object_literal.type) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, expr->as.object_literal.type); for (AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) sema_analyze_expr(ctx, module, symbols, f->value); break;
        default: break;
    }
}

TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref) { return sema_resolve_type_internal(ctx, NULL, NULL, type_ref); }

static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstTypeRef* type_ref) {
    if (!type_ref) return type_get_void(ctx->type_registry);
    if (type_ref->resolved_type) return type_ref->resolved_type;
    TypeInfo* base = NULL;
    if (type_ref->parts) {
        Str name = type_ref->parts->text;
        if (str_eq_cstr(name, "Int")) base = type_get_int(ctx->type_registry);
        else if (str_eq_cstr(name, "Float")) base = type_get_float(ctx->type_registry);
        else if (str_eq_cstr(name, "Bool")) base = type_get_bool(ctx->type_registry);
        else if (str_eq_cstr(name, "String")) base = type_get_string(ctx->type_registry);
        else if (str_eq_cstr(name, "Char")) base = type_get_char(ctx->type_registry);
        else if (str_eq_cstr(name, "Any")) base = type_get_any(ctx->type_registry);
        else if (str_eq_cstr(name, "Buffer")) {
            TypeInfo* arg = type_get_void(ctx->type_registry);
            if (type_ref->generic_args) arg = sema_resolve_type_internal(ctx, module, symbols, type_ref->generic_args);
            base = type_get_buffer(ctx->type_registry, arg);
        } else if (symbols) {
            Symbol* sym = symbol_table_lookup(symbols, name);
            if (sym && sym->type) {
                if (type_ref->generic_args && sym->decl && sym->decl->kind == AST_DECL_TYPE) {
                    TypeInfo* args[16]; size_t ac = 0; AstTypeRef* curr = type_ref->generic_args;
                    while (curr && ac < 16) { args[ac++] = sema_resolve_type_internal(ctx, module, symbols, curr); curr = curr->next; }
                    base = type_get_struct(ctx->type_registry, sym->decl, args, ac);
                    register_generic_type(ctx, type_ref);
                    if (!type_registry_find_specialization(ctx->type_registry, sym->decl, args, ac)) specialize_decl(ctx, module, sym->decl, args, ac, type_ref->line, type_ref->column);
                } else base = sym->type;
            }
        }
    }
    if (!base) base = type_get_void(ctx->type_registry);
    if (type_ref->is_opt) base = type_get_opt(ctx->type_registry, base);
    if (type_ref->is_view) base = type_get_ref(ctx->type_registry, base, false);
    else if (type_ref->is_mod) base = type_get_ref(ctx->type_registry, base, true);
    type_ref->resolved_type = base; return base;
}

bool sema_analyze_module(CompilerContext* ctx, AstModule* module) {
    if (!ctx->type_registry) { ctx->type_registry = arena_alloc(ctx->ast_arena, sizeof(TypeRegistry)); type_registry_init(ctx->type_registry, ctx->ast_arena); }
    if (!ctx->instantiation_stack) { ctx->instantiation_stack = arena_alloc(ctx->ast_arena, sizeof(InstantiationStack)); ctx->instantiation_stack->head = NULL; }
    SymbolTable symbols = {0}; size_t processed_count = 0; const AstDecl* processed[8192]; memset(processed, 0, sizeof(processed));
    AstDecl* d = module->decls;
    while (d) {
        Str name = {0}; TypeInfo* t = NULL;
        switch (d->kind) {
            case AST_DECL_TYPE: 
                name = d->as.type_decl.name; 
                if (!d->resolved_type) d->resolved_type = type_get_struct(ctx->type_registry, d, NULL, 0); 
                t = d->resolved_type; break;
            case AST_DECL_FUNC: name = d->as.func_decl.name; break;
            case AST_DECL_ENUM: name = d->as.enum_decl.name; break;
            case AST_DECL_GLOBAL_LET: name = d->as.let_decl.name; break;
            default: break;
        }
        if (name.len > 0) { if (!symbol_table_lookup(&symbols, name)) symbol_table_define(&symbols, ctx->ast_arena, name, d, t, false); }
        d = d->next;
    }
    bool found_new = true;
    while (found_new) {
        found_new = false; d = module->decls;
        while (d) {
            bool already = false; for (size_t i = 0; i < processed_count; i++) if (processed[i] == d) { already = true; break; }
            if (already) { d = d->next; continue; }
            
            bool is_template = (d->kind == AST_DECL_FUNC && d->as.func_decl.generic_params && !d->as.func_decl.specialization_args) || 
                               (d->kind == AST_DECL_TYPE && d->as.type_decl.generic_params && !d->as.type_decl.specialization_args);
            
            if (!is_template) {
                if (processed_count < 8192) processed[processed_count++] = d;
                found_new = true;
                
                // If it's a specialization, we might need to define it in symbol table if it's not there
                if (d->kind == AST_DECL_TYPE && d->as.type_decl.specialization_args && !d->resolved_type) {
                    TypeInfo* args[16]; size_t ac = 0;
                    for (AstTypeRef* tr = d->as.type_decl.specialization_args; tr && ac < 16; tr = tr->next) args[ac++] = sema_resolve_type_internal(ctx, module, &symbols, tr);
                    d->resolved_type = type_get_struct(ctx->type_registry, d->as.type_decl.generic_template, args, ac);
                }

                sema_analyze_decl(ctx, module, &symbols, d);
            } else {
                // Templates are marked as processed but not analyzed
                if (processed_count < 8192) processed[processed_count++] = d;
            }
            d = d->next;
        }
    }
    return !module->had_error;
}
