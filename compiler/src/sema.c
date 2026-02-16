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

// Forward declarations
static void sema_analyze_decl(CompilerContext* ctx, SymbolTable* symbols, AstDecl* decl);
static void sema_analyze_expr(CompilerContext* ctx, SymbolTable* symbols, AstExpr* expr);
static void sema_analyze_stmt(CompilerContext* ctx, SymbolTable* symbols, AstStmt* stmt);
static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, SymbolTable* symbols, AstTypeRef* type_ref);

bool sema_analyze_module(CompilerContext* ctx, AstModule* module) {
    if (!ctx->type_registry) {
        ctx->type_registry = arena_alloc(ctx->ast_arena, sizeof(TypeRegistry));
        type_registry_init(ctx->type_registry, ctx->ast_arena);
    }
    
    SymbolTable symbols = {0};
    
    // Pass 1: Declare all types and functions (populate symbol table)
    AstDecl* decl = module->decls;
    while (decl) {
        Str name = {0};
        TypeInfo* t = NULL;
        switch (decl->kind) {
            case AST_DECL_TYPE: 
                name = decl->as.type_decl.name; 
                // Create the type identity immediately for structs
                t = type_get_struct(ctx->type_registry, decl, NULL, 0);
                decl->resolved_type = t;
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

    // Pass 2: Analyze bodies
    decl = module->decls;
    while (decl) {
        sema_analyze_decl(ctx, &symbols, decl);
        decl = decl->next;
    }

    return !module->had_error;
}

static void sema_analyze_decl(CompilerContext* ctx, SymbolTable* symbols, AstDecl* decl) {
    if (!decl) return;
    switch (decl->kind) {
        case AST_DECL_FUNC: {
            symbol_table_push_scope(symbols);
            // Define parameters in function scope
            AstParam* param = decl->as.func_decl.params;
            while (param) {
                TypeInfo* t = sema_resolve_type_internal(ctx, symbols, param->type);
                symbol_table_define(symbols, ctx->ast_arena, param->name, NULL, t);
                param = param->next;
            }

            if (decl->as.func_decl.body) {
                AstStmt* stmt = decl->as.func_decl.body->first;
                while (stmt) {
                    sema_analyze_stmt(ctx, symbols, stmt);
                    stmt = stmt->next;
                }
            }
            symbol_table_pop_scope(symbols);
            break;
        }
        case AST_DECL_GLOBAL_LET: {
            if (decl->as.let_decl.value) {
                sema_analyze_expr(ctx, symbols, decl->as.let_decl.value);
            }
            break;
        }
        default: break;
    }
}

static void sema_analyze_stmt(CompilerContext* ctx, SymbolTable* symbols, AstStmt* stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case AST_STMT_EXPR:
            if (stmt->as.expr_stmt) sema_analyze_expr(ctx, symbols, stmt->as.expr_stmt);
            break;
        case AST_STMT_LET: {
            TypeInfo* t = NULL;
            if (stmt->as.let_stmt.type) {
                t = sema_resolve_type_internal(ctx, symbols, stmt->as.let_stmt.type);
            }
            if (stmt->as.let_stmt.value) {
                sema_analyze_expr(ctx, symbols, stmt->as.let_stmt.value);
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
                if (arg->value) sema_analyze_expr(ctx, symbols, arg->value);
                arg = arg->next;
            }
            break;
        }
        case AST_STMT_IF: {
            if (stmt->as.if_stmt.condition) sema_analyze_expr(ctx, symbols, stmt->as.if_stmt.condition);
            if (stmt->as.if_stmt.then_block) {
                symbol_table_push_scope(symbols);
                AstStmt* s = stmt->as.if_stmt.then_block->first;
                while (s) { sema_analyze_stmt(ctx, symbols, s); s = s->next; }
                symbol_table_pop_scope(symbols);
            }
            if (stmt->as.if_stmt.else_block) {
                symbol_table_push_scope(symbols);
                AstStmt* s = stmt->as.if_stmt.else_block->first;
                while (s) { sema_analyze_stmt(ctx, symbols, s); s = s->next; }
                symbol_table_pop_scope(symbols);
            }
            break;
        }
        case AST_STMT_LOOP: {
             symbol_table_push_scope(symbols);
             if (stmt->as.loop_stmt.init) sema_analyze_stmt(ctx, symbols, stmt->as.loop_stmt.init);
             if (stmt->as.loop_stmt.condition) sema_analyze_expr(ctx, symbols, stmt->as.loop_stmt.condition);
             if (stmt->as.loop_stmt.increment) sema_analyze_expr(ctx, symbols, stmt->as.loop_stmt.increment);
             if (stmt->as.loop_stmt.body) {
                 AstStmt* s = stmt->as.loop_stmt.body->first;
                 while (s) { sema_analyze_stmt(ctx, symbols, s); s = s->next; }
             }
             symbol_table_pop_scope(symbols);
             break;
        }
        case AST_STMT_ASSIGN: {
            sema_analyze_expr(ctx, symbols, stmt->as.assign_stmt.target);
            sema_analyze_expr(ctx, symbols, stmt->as.assign_stmt.value);
            break;
        }
        default: break;
    }
}

static TypeInfo* find_field_type(CompilerContext* ctx, SymbolTable* symbols, TypeInfo* struct_type, Str field_name) {
    if (struct_type->kind == TYPE_REF) struct_type = struct_type->as.ref.base;
    if (struct_type->kind != TYPE_STRUCT) return NULL;

    AstDecl* decl = struct_type->as.structure.decl;
    AstTypeField* field = decl->as.type_decl.fields;
    while (field) {
        if (str_eq(field->name, field_name)) {
            // If it's a generic struct, we must substitute generic parameters in the field type
            if (struct_type->as.structure.generic_count > 0) {
                // Create a temporary scope for generic mapping
                symbol_table_push_scope(symbols);
                AstIdentifierPart* param = decl->as.type_decl.generic_params;
                size_t i = 0;
                while (param && i < struct_type->as.structure.generic_count) {
                    symbol_table_define(symbols, ctx->ast_arena, param->text, NULL, struct_type->as.structure.generic_args[i]);
                    param = param->next; i++;
                }
                TypeInfo* result = sema_resolve_type_internal(ctx, symbols, field->type);
                symbol_table_pop_scope(symbols);
                return result;
            }
            return sema_resolve_type_internal(ctx, symbols, field->type);
        }
        field = field->next;
    }
    return NULL;
}

static void sema_analyze_expr(CompilerContext* ctx, SymbolTable* symbols, AstExpr* expr) {
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
            sema_analyze_expr(ctx, symbols, expr->as.binary.lhs);
            sema_analyze_expr(ctx, symbols, expr->as.binary.rhs);
            if (expr->as.binary.lhs && expr->as.binary.lhs->resolved_type) {
                expr->resolved_type = expr->as.binary.lhs->resolved_type;
            }
            break;
        case AST_EXPR_CALL:
            sema_analyze_expr(ctx, symbols, expr->as.call.callee);
            AstCallArg* arg = expr->as.call.args;
            while (arg) {
                sema_analyze_expr(ctx, symbols, arg->value);
                arg = arg->next;
            }
            // Return type resolution:
            if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
                Str name = expr->as.call.callee->as.ident;
                if (str_eq_cstr(name, "sizeof")) {
                    expr->is_builtin_sizeof = true;
                    expr->resolved_type = type_get_int(ctx->type_registry);
                } else {
                    Symbol* sym = symbol_table_lookup(symbols, name);
                    if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                        // TODO: handle multi-return
                        if (sym->decl->as.func_decl.returns) {
                            expr->resolved_type = sema_resolve_type_internal(ctx, symbols, sym->decl->as.func_decl.returns->type);
                        }
                    }
                }
            }
            break;
        case AST_EXPR_MEMBER:
            sema_analyze_expr(ctx, symbols, expr->as.member.object);
            if (expr->as.member.object->resolved_type) {
                expr->resolved_type = find_field_type(ctx, symbols, expr->as.member.object->resolved_type, expr->as.member.member);
            }
            break;
        case AST_EXPR_METHOD_CALL:
            sema_analyze_expr(ctx, symbols, expr->as.method_call.object);
            AstCallArg* marg = expr->as.method_call.args;
            while (marg) {
                sema_analyze_expr(ctx, symbols, marg->value);
                marg = marg->next;
            }
            if (expr->as.method_call.object->resolved_type) {
                TypeInfo* t = expr->as.method_call.object->resolved_type;
                if (t->kind == TYPE_REF) t = t->as.ref.base;
                
                // Lookup in MethodTable
                for (size_t i = 0; i < ctx->methods.count; i++) {
                    MethodEntry* entry = &ctx->methods.entries[i];
                    if (str_eq(entry->type_name, t->name) && str_eq(entry->method_name, expr->as.method_call.method_name)) {
                        // Resolve the return type of the actual function
                        Symbol* sym = symbol_table_lookup(symbols, entry->actual_function_name);
                        if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                            if (sym->decl->as.func_decl.returns) {
                                expr->resolved_type = sema_resolve_type_internal(ctx, symbols, sym->decl->as.func_decl.returns->type);
                            }
                        }
                        break;
                    }
                }
            }
            break;
        case AST_EXPR_INDEX:
            sema_analyze_expr(ctx, symbols, expr->as.index.target);
            sema_analyze_expr(ctx, symbols, expr->as.index.index);
            // Result of index Buffer(T) is T
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
    return sema_resolve_type_internal(ctx, NULL, type_ref);
}

static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, SymbolTable* symbols, AstTypeRef* type_ref) {
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
            if (type_ref->generic_args) arg = sema_resolve_type_internal(ctx, symbols, type_ref->generic_args);
            base = type_get_buffer(ctx->type_registry, arg);
        }
        else if (symbols) {
            Symbol* sym = symbol_table_lookup(symbols, name);
            if (sym && sym->type) {
                if (type_ref->generic_args && sym->decl && sym->decl->kind == AST_DECL_TYPE) {
                    // Resolve all generic args
                    TypeInfo* args[16]; // Max 16 generic args
                    size_t arg_count = 0;
                    AstTypeRef* curr_arg = type_ref->generic_args;
                    while (curr_arg && arg_count < 16) {
                        args[arg_count++] = sema_resolve_type_internal(ctx, symbols, curr_arg);
                        curr_arg = curr_arg->next;
                    }
                    base = type_get_struct(ctx->type_registry, sym->decl, args, arg_count);
                } else {
                    base = sym->type;
                }
            }
        }
    }

    if (!base) base = type_get_void(ctx->type_registry);

    if (type_ref->is_opt) {
        base = type_get_opt(ctx->type_registry, base);
    }
    if (type_ref->is_view) {
        base = type_get_ref(ctx->type_registry, base, false);
    } else if (type_ref->is_mod) {
        base = type_get_ref(ctx->type_registry, base, true);
    }

    return base;
}
