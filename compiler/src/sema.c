#include "sema.h"
#include "type.h"
#include "ast.h"
#include "diag.h"
#include <string.h>

typedef struct Symbol Symbol;
struct Symbol {
    Str name;
    AstDecl* decl;
    TypeInfo* type;
    int scope_depth;
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

static void symbol_table_define(SymbolTable* table, Arena* arena, Str name, AstDecl* decl, TypeInfo* type) {
    Symbol* sym = arena_alloc(arena, sizeof(Symbol));
    sym->name = name;
    sym->decl = decl;
    sym->type = type;
    sym->scope_depth = table->current_depth;
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

static void instantiation_stack_push(InstantiationStack* stack, Arena* arena, const char* file_path, size_t line, size_t column) {
    InstantiationEntry* entry = arena_alloc(arena, sizeof(InstantiationEntry));
    entry->file_path = file_path;
    entry->line = line;
    entry->column = column;
    entry->next = stack->head;
    stack->head = entry;
}

static void instantiation_stack_pop(InstantiationStack* stack) {
    if (stack->head) stack->head = stack->head->next;
}

// Forward declarations
static void sema_analyze_decl(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* decl);
static void sema_analyze_expr(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* expr);
static void sema_analyze_stmt(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt);
static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstTypeRef* type_ref);
static AstDecl* specialize_decl(CompilerContext* ctx, AstModule* module, AstDecl* generic_decl, TypeInfo** args, size_t arg_count, size_t line, size_t column);

// --- AST Cloning ---

static AstTypeRef* clone_type_ref(Arena* arena, const AstTypeRef* tr) {
    if (!tr) return NULL;
    AstTypeRef* res = arena_alloc(arena, sizeof(AstTypeRef));
    *res = *tr;
    if (tr->parts) {
        AstIdentifierPart* head = NULL;
        AstIdentifierPart* tail = NULL;
        AstIdentifierPart* curr = tr->parts;
        while (curr) {
            AstIdentifierPart* p = arena_alloc(arena, sizeof(AstIdentifierPart));
            *p = *curr;
            if (!head) head = p;
            if (tail) tail->next = p;
            tail = p;
            curr = curr->next;
        }
        res->parts = head;
    }
    if (tr->generic_args) res->generic_args = clone_type_ref(arena, tr->generic_args);
    if (tr->next) res->next = clone_type_ref(arena, tr->next);
    return res;
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
    AstStmt* head = NULL;
    AstStmt* tail = NULL;
    AstStmt* curr = block->first;
    while (curr) {
        AstStmt* s = clone_stmt(arena, curr);
        if (!head) head = s;
        if (tail) tail->next = s;
        tail = s;
        curr = curr->next;
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
        case AST_STMT_EXPR:
            res->as.expr_stmt = clone_expr(arena, stmt->as.expr_stmt);
            break;
        case AST_STMT_LET:
            res->as.let_stmt.type = clone_type_ref(arena, stmt->as.let_stmt.type);
            res->as.let_stmt.value = clone_expr(arena, stmt->as.let_stmt.value);
            break;
        case AST_STMT_RET: {
            AstReturnArg* head = NULL;
            AstReturnArg* tail = NULL;
            AstReturnArg* curr = stmt->as.ret_stmt.values;
            while (curr) {
                AstReturnArg* a = arena_alloc(arena, sizeof(AstReturnArg));
                *a = *curr;
                a->value = clone_expr(arena, curr->value);
                a->next = NULL;
                if (!head) head = a;
                if (tail) tail->next = a;
                tail = a;
                curr = curr->next;
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

// --- Specialization ---

static AstDecl* specialize_decl(CompilerContext* ctx, AstModule* module, AstDecl* generic_decl, TypeInfo** args, size_t arg_count, size_t line, size_t column) {
    AstDecl* existing = type_registry_find_specialization(ctx->type_registry, generic_decl, args, arg_count);
    if (existing) return existing;

    AstDecl* spec = arena_alloc(ctx->ast_arena, sizeof(AstDecl));
    *spec = *generic_decl;
    spec->next = NULL;

    if (generic_decl->kind == AST_DECL_FUNC) {
        // Clone and map params
        AstParam* head = NULL;
        AstParam* tail = NULL;
        AstParam* p = generic_decl->as.func_decl.params;
        while (p) {
            AstParam* np = arena_alloc(ctx->ast_arena, sizeof(AstParam));
            *np = *p;
            np->type = clone_type_ref(ctx->ast_arena, p->type);
            np->next = NULL;
            if (!head) head = np;
            if (tail) tail->next = np;
            tail = np;
            p = p->next;
        }
        spec->as.func_decl.params = head;
        spec->as.func_decl.body = clone_block(ctx->ast_arena, generic_decl->as.func_decl.body);
        spec->as.func_decl.generic_params = NULL;
    } else if (generic_decl->kind == AST_DECL_TYPE) {
        // Struct specialization - clone fields
        AstTypeField* head = NULL;
        AstTypeField* tail = NULL;
        AstTypeField* f = generic_decl->as.type_decl.fields;
        while (f) {
            AstTypeField* nf = arena_alloc(ctx->ast_arena, sizeof(AstTypeField));
            *nf = *f;
            nf->type = clone_type_ref(ctx->ast_arena, f->type);
            nf->default_value = clone_expr(ctx->ast_arena, f->default_value);
            nf->next = NULL;
            if (!head) head = nf;
            if (tail) tail->next = nf;
            tail = nf;
            f = f->next;
        }
        spec->as.type_decl.fields = head;
        spec->as.type_decl.generic_params = NULL;
    }

    type_registry_add_specialization(ctx->type_registry, generic_decl, args, arg_count, spec);

    // 3. Assign unique mangled name
    // Simple mangling: Name_Arg1_Arg2
    char name_buf[256];
    Str base_name = (generic_decl->kind == AST_DECL_FUNC) ? generic_decl->as.func_decl.name : generic_decl->as.type_decl.name;
    int len = snprintf(name_buf, sizeof(name_buf), "%.*s_", (int)base_name.len, base_name.data);
    for (size_t i = 0; i < arg_count; i++) {
        len += snprintf(name_buf + len, sizeof(name_buf) - len, "%.*s%s", (int)args[i]->name.len, args[i]->name.data, (i == arg_count - 1) ? "" : "_");
    }
    Str mangled_name = str_dup_arena(ctx->ast_arena, (Str){(uint8_t*)name_buf, (size_t)len});
    if (spec->kind == AST_DECL_FUNC) spec->as.func_decl.name = mangled_name;
    else if (spec->kind == AST_DECL_TYPE) spec->as.type_decl.name = mangled_name;

    // 4. Analyze specialized body
    instantiation_stack_push(ctx->instantiation_stack, ctx->ast_arena, module->file_path, line, column);
    
    SymbolTable spec_symbols = {0};
    symbol_table_push_scope(&spec_symbols);
    AstIdentifierPart* param = (generic_decl->kind == AST_DECL_FUNC) ? generic_decl->as.func_decl.generic_params : generic_decl->as.type_decl.generic_params;
    for (size_t i = 0; i < arg_count && param; i++) {
        symbol_table_define(&spec_symbols, ctx->ast_arena, param->text, NULL, args[i]);
        param = param->next;
    }
    
    // Recursive analysis
    sema_analyze_decl(ctx, module, &spec_symbols, spec);
    symbol_table_pop_scope(&spec_symbols);
    
    instantiation_stack_pop(ctx->instantiation_stack);
    
    return spec;
}

// --- Analysis ---

bool sema_analyze_module(CompilerContext* ctx, AstModule* module) {
    if (!ctx->type_registry) {
        ctx->type_registry = arena_alloc(ctx->ast_arena, sizeof(TypeRegistry));
        type_registry_init(ctx->type_registry, ctx->ast_arena);
    }
    if (!ctx->instantiation_stack) {
        ctx->instantiation_stack = arena_alloc(ctx->ast_arena, sizeof(InstantiationStack));
        ctx->instantiation_stack->head = NULL;
    }
    
    SymbolTable symbols = {0};
    
    AstDecl* decl = module->decls;
    while (decl) {
        Str name = {0};
        TypeInfo* t = NULL;
        switch (decl->kind) {
            case AST_DECL_TYPE: 
                name = decl->as.type_decl.name; 
                if (!decl->as.type_decl.generic_params) {
                    t = type_get_struct(ctx->type_registry, decl, NULL, 0);
                    decl->resolved_type = t;
                }
                break;
            case AST_DECL_FUNC: name = decl->as.func_decl.name; break;
            case AST_DECL_ENUM: name = decl->as.enum_decl.name; break;
            case AST_DECL_GLOBAL_LET: name = decl->as.let_decl.name; break;
        }
        if (name.len > 0) {
            symbol_table_define(&symbols, ctx->ast_arena, name, decl, t);
        }
        decl = decl->next;
    }

    decl = module->decls;
    while (decl) {
        bool is_generic = false;
        if (decl->kind == AST_DECL_FUNC && decl->as.func_decl.generic_params) is_generic = true;
        if (decl->kind == AST_DECL_TYPE && decl->as.type_decl.generic_params) is_generic = true;
        if (!is_generic) {
            sema_analyze_decl(ctx, module, &symbols, decl);
        }
        decl = decl->next;
    }

    return !module->had_error;
}

static void sema_analyze_decl(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* decl) {
    if (!decl) return;
    switch (decl->kind) {
        case AST_DECL_FUNC: {
            symbol_table_push_scope(symbols);
            AstParam* param = decl->as.func_decl.params;
            while (param) {
                TypeInfo* t = sema_resolve_type_internal(ctx, module, symbols, param->type);
                symbol_table_define(symbols, ctx->ast_arena, param->name, NULL, t);
                param = param->next;
            }

            if (decl->as.func_decl.body) {
                AstStmt* stmt = decl->as.func_decl.body->first;
                while (stmt) {
                    sema_analyze_stmt(ctx, module, symbols, stmt);
                    stmt = stmt->next;
                }
            }
            symbol_table_pop_scope(symbols);
            break;
        }
        case AST_DECL_GLOBAL_LET: {
            if (decl->as.let_decl.value) {
                sema_analyze_expr(ctx, module, symbols, decl->as.let_decl.value);
            }
            break;
        }
        default: break;
    }
}

static void sema_analyze_stmt(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case AST_STMT_EXPR:
            if (stmt->as.expr_stmt) sema_analyze_expr(ctx, module, symbols, stmt->as.expr_stmt);
            break;
        case AST_STMT_LET: {
            TypeInfo* t = NULL;
            if (stmt->as.let_stmt.type) {
                t = sema_resolve_type_internal(ctx, module, symbols, stmt->as.let_stmt.type);
            }
            if (stmt->as.let_stmt.value) {
                sema_analyze_expr(ctx, module, symbols, stmt->as.let_stmt.value);
                if (!t && stmt->as.let_stmt.value->resolved_type) {
                    t = stmt->as.let_stmt.value->resolved_type;
                }
            }
            symbol_table_define(symbols, ctx->ast_arena, stmt->as.let_stmt.name, NULL, t);
            break;
        }
        case AST_STMT_RET: {
            AstReturnArg* arg = stmt->as.ret_stmt.values;
            while (arg) {
                if (arg->value) sema_analyze_expr(ctx, module, symbols, arg->value);
                arg = arg->next;
            }
            break;
        }
        case AST_STMT_IF: {
            if (stmt->as.if_stmt.condition) sema_analyze_expr(ctx, module, symbols, stmt->as.if_stmt.condition);
            if (stmt->as.if_stmt.then_block) {
                symbol_table_push_scope(symbols);
                AstStmt* s = stmt->as.if_stmt.then_block->first;
                while (s) { sema_analyze_stmt(ctx, module, symbols, s); s = s->next; }
                symbol_table_pop_scope(symbols);
            }
            if (stmt->as.if_stmt.else_block) {
                symbol_table_push_scope(symbols);
                AstStmt* s = stmt->as.if_stmt.else_block->first;
                while (s) { sema_analyze_stmt(ctx, module, symbols, s); s = s->next; }
                symbol_table_pop_scope(symbols);
            }
            break;
        }
        case AST_STMT_LOOP: {
             symbol_table_push_scope(symbols);
             if (stmt->as.loop_stmt.init) sema_analyze_stmt(ctx, module, symbols, stmt->as.loop_stmt.init);
             if (stmt->as.loop_stmt.condition) sema_analyze_expr(ctx, module, symbols, stmt->as.loop_stmt.condition);
             if (stmt->as.loop_stmt.increment) sema_analyze_expr(ctx, module, symbols, stmt->as.loop_stmt.increment);
             if (stmt->as.loop_stmt.body) {
                 AstStmt* s = stmt->as.loop_stmt.body->first;
                 while (s) { sema_analyze_stmt(ctx, module, symbols, s); s = s->next; }
             }
             symbol_table_pop_scope(symbols);
             break;
        }
        case AST_STMT_ASSIGN: {
            sema_analyze_expr(ctx, module, symbols, stmt->as.assign_stmt.target);
            sema_analyze_expr(ctx, module, symbols, stmt->as.assign_stmt.value);
            break;
        }
        default: break;
    }
}

static TypeInfo* find_field_type(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, TypeInfo* struct_type, Str field_name) {
    if (struct_type->kind == TYPE_REF) struct_type = struct_type->as.ref.base;
    if (struct_type->kind != TYPE_STRUCT) return NULL;

    AstDecl* decl = struct_type->as.structure.decl;
    AstTypeField* field = decl->as.type_decl.fields;
    while (field) {
        if (str_eq(field->name, field_name)) {
            if (struct_type->as.structure.generic_count > 0) {
                symbol_table_push_scope(symbols);
                AstIdentifierPart* param = decl->as.type_decl.generic_params;
                size_t i = 0;
                while (param && i < struct_type->as.structure.generic_count) {
                    symbol_table_define(symbols, ctx->ast_arena, param->text, NULL, struct_type->as.structure.generic_args[i]);
                    param = param->next; i++;
                }
                TypeInfo* result = sema_resolve_type_internal(ctx, module, symbols, field->type);
                symbol_table_pop_scope(symbols);
                return result;
            }
            return sema_resolve_type_internal(ctx, module, symbols, field->type);
        }
        field = field->next;
    }
    return NULL;
}

static void sema_analyze_expr(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* expr) {
    if (!expr) return;
    
    switch (expr->kind) {
        case AST_EXPR_IDENT: {
            Symbol* sym = symbol_table_lookup(symbols, expr->as.ident);
            if (sym) {
                expr->resolved_type = sym->type;
            }
            break;
        }
        case AST_EXPR_INTEGER:
            expr->resolved_type = type_get_int(ctx->type_registry);
            break;
        case AST_EXPR_FLOAT:
            expr->resolved_type = type_get_float(ctx->type_registry);
            break;
        case AST_EXPR_BOOL:
            expr->resolved_type = type_get_bool(ctx->type_registry);
            break;
        case AST_EXPR_STRING:
            expr->resolved_type = type_get_string(ctx->type_registry);
            break;
        case AST_EXPR_CHAR:
            expr->resolved_type = type_get_char(ctx->type_registry);
            break;
        case AST_EXPR_BINARY:
            sema_analyze_expr(ctx, module, symbols, expr->as.binary.lhs);
            sema_analyze_expr(ctx, module, symbols, expr->as.binary.rhs);
            if (expr->as.binary.lhs && expr->as.binary.lhs->resolved_type) {
                expr->resolved_type = expr->as.binary.lhs->resolved_type;
            }
            break;
        case AST_EXPR_CALL:
            sema_analyze_expr(ctx, module, symbols, expr->as.call.callee);
            AstCallArg* arg = expr->as.call.args;
            while (arg) {
                sema_analyze_expr(ctx, module, symbols, arg->value);
                arg = arg->next;
            }
            if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
                Str name = expr->as.call.callee->as.ident;
                if (str_eq_cstr(name, "sizeof")) {
                    expr->is_builtin_sizeof = true;
                    expr->resolved_type = type_get_int(ctx->type_registry);
                } else {
                    Symbol* sym = symbol_table_lookup(symbols, name);
                    if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                        if (sym->decl->as.func_decl.returns) {
                            expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, sym->decl->as.func_decl.returns->type);
                        }
                    }
                }
            }
            break;
        case AST_EXPR_MEMBER:
            sema_analyze_expr(ctx, module, symbols, expr->as.member.object);
            if (expr->as.member.object->resolved_type) {
                expr->resolved_type = find_field_type(ctx, module, symbols, expr->as.member.object->resolved_type, expr->as.member.member);
            }
            break;
        case AST_EXPR_METHOD_CALL:
            sema_analyze_expr(ctx, module, symbols, expr->as.method_call.object);
            AstCallArg* marg = expr->as.method_call.args;
            while (marg) {
                sema_analyze_expr(ctx, module, symbols, marg->value);
                marg = marg->next;
            }
            if (expr->as.method_call.object->resolved_type) {
                TypeInfo* t = expr->as.method_call.object->resolved_type;
                if (t->kind == TYPE_REF) t = t->as.ref.base;
                for (size_t i = 0; i < ctx->methods.count; i++) {
                    MethodEntry* entry = &ctx->methods.entries[i];
                    if (str_eq(entry->type_name, t->name) && str_eq(entry->method_name, expr->as.method_call.method_name)) {
                        Symbol* sym = symbol_table_lookup(symbols, entry->actual_function_name);
                        if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                            if (sym->decl->as.func_decl.returns) {
                                expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, sym->decl->as.func_decl.returns->type);
                            }
                        }
                        break;
                    }
                }
            }
            break;
        case AST_EXPR_INDEX:
            sema_analyze_expr(ctx, module, symbols, expr->as.index.target);
            sema_analyze_expr(ctx, module, symbols, expr->as.index.index);
            if (expr->as.index.target->resolved_type) {
                TypeInfo* t = expr->as.index.target->resolved_type;
                if (t->kind == TYPE_REF) t = t->as.ref.base;
                if (t->kind == TYPE_BUFFER) expr->resolved_type = t->as.buffer.base;
            }
            break;
        default: break;
    }
}

TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref) {
    return sema_resolve_type_internal(ctx, NULL, NULL, type_ref);
}

static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstTypeRef* type_ref) {
    if (!type_ref) return type_get_void(ctx->type_registry);
    
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
        }
        else if (symbols) {
            Symbol* sym = symbol_table_lookup(symbols, name);
            if (sym) {
                if (sym->type) {
                    if (type_ref->generic_args && sym->decl && sym->decl->kind == AST_DECL_TYPE) {
                        TypeInfo* args[16];
                        size_t arg_count = 0;
                        AstTypeRef* curr_arg = type_ref->generic_args;
                        while (curr_arg && arg_count < 16) {
                            args[arg_count++] = sema_resolve_type_internal(ctx, module, symbols, curr_arg);
                            curr_arg = curr_arg->next;
                        }
                        base = type_get_struct(ctx->type_registry, sym->decl, args, arg_count);
                        if (!type_registry_find_specialization(ctx->type_registry, sym->decl, args, arg_count)) {
                            AstDecl* spec = specialize_decl(ctx, module, sym->decl, args, arg_count, type_ref->line, type_ref->column);
                            spec->next = module->decls;
                            module->decls = spec;
                        }
                    } else {
                        base = sym->type;
                    }
                }
            }
        }
    }

    if (!base) base = type_get_void(ctx->type_registry);

    if (type_ref->is_opt) base = type_get_opt(ctx->type_registry, base);
    if (type_ref->is_view) base = type_get_ref(ctx->type_registry, base, false);
    else if (type_ref->is_mod) base = type_get_ref(ctx->type_registry, base, true);

    return base;
}
