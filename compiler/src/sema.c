#include "sema.h"
#include "type.h"
#include "ast.h"
#include "diag.h"

// Forward declarations
static void sema_analyze_decl(CompilerContext* ctx, AstDecl* decl);
static void sema_analyze_expr(CompilerContext* ctx, AstExpr* expr);
static void sema_analyze_stmt(CompilerContext* ctx, AstStmt* stmt);

bool sema_analyze_module(CompilerContext* ctx, AstModule* module) {
    if (!ctx->type_registry) {
        ctx->type_registry = arena_alloc(ctx->ast_arena, sizeof(TypeRegistry));
        type_registry_init(ctx->type_registry, ctx->ast_arena);
    }
    
    // Pass 1: Declare all types and functions (populate symbol table)
    AstDecl* decl = module->decls;
    while (decl) {
        // TODO: Symbol table insertion
        decl = decl->next;
    }

    // Pass 2: Analyze bodies
    decl = module->decls;
    while (decl) {
        sema_analyze_decl(ctx, decl);
        decl = decl->next;
    }

    return !module->had_error;
}

static void sema_analyze_decl(CompilerContext* ctx, AstDecl* decl) {
    if (!decl) return;
    switch (decl->kind) {
        case AST_DECL_FUNC: {
            if (decl->as.func_decl.body) {
                AstStmt* stmt = decl->as.func_decl.body->first;
                while (stmt) {
                    sema_analyze_stmt(ctx, stmt);
                    stmt = stmt->next;
                }
            }
            break;
        }
        case AST_DECL_GLOBAL_LET: {
            if (decl->as.let_decl.value) {
                sema_analyze_expr(ctx, decl->as.let_decl.value);
            }
            break;
        }
        default: break;
    }
}

static void sema_analyze_stmt(CompilerContext* ctx, AstStmt* stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case AST_STMT_EXPR:
            if (stmt->as.expr_stmt) sema_analyze_expr(ctx, stmt->as.expr_stmt);
            break;
        case AST_STMT_LET:
            if (stmt->as.let_stmt.value) {
                sema_analyze_expr(ctx, stmt->as.let_stmt.value);
            }
            break;
        case AST_STMT_RET: {
            AstReturnArg* arg = stmt->as.ret_stmt.values;
            while (arg) {
                if (arg->value) sema_analyze_expr(ctx, arg->value);
                arg = arg->next;
            }
            break;
        }
        case AST_STMT_IF: {
            if (stmt->as.if_stmt.condition) sema_analyze_expr(ctx, stmt->as.if_stmt.condition);
            if (stmt->as.if_stmt.then_block) {
                AstStmt* s = stmt->as.if_stmt.then_block->first;
                while (s) { sema_analyze_stmt(ctx, s); s = s->next; }
            }
            if (stmt->as.if_stmt.else_block) {
                AstStmt* s = stmt->as.if_stmt.else_block->first;
                while (s) { sema_analyze_stmt(ctx, s); s = s->next; }
            }
            break;
        }
        case AST_STMT_LOOP: {
             if (stmt->as.loop_stmt.init) sema_analyze_stmt(ctx, stmt->as.loop_stmt.init);
             if (stmt->as.loop_stmt.condition) sema_analyze_expr(ctx, stmt->as.loop_stmt.condition);
             if (stmt->as.loop_stmt.increment) sema_analyze_expr(ctx, stmt->as.loop_stmt.increment);
             if (stmt->as.loop_stmt.body) {
                 AstStmt* s = stmt->as.loop_stmt.body->first;
                 while (s) { sema_analyze_stmt(ctx, s); s = s->next; }
             }
             break;
        }
        default: break;
    }
}

static void sema_analyze_expr(CompilerContext* ctx, AstExpr* expr) {
    if (!expr) return;
    
    // Assign primitive types immediately
    switch (expr->kind) {
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
            sema_analyze_expr(ctx, expr->as.binary.lhs);
            sema_analyze_expr(ctx, expr->as.binary.rhs);
            // Basic inference: if lhs is int, result is int (for arithmetic)
            // Ideally we check op.
            if (expr->as.binary.lhs->resolved_type) {
                expr->resolved_type = expr->as.binary.lhs->resolved_type;
            }
            break;
        case AST_EXPR_CALL:
            sema_analyze_expr(ctx, expr->as.call.callee);
            AstCallArg* arg = expr->as.call.args;
            while (arg) {
                sema_analyze_expr(ctx, arg->value);
                arg = arg->next;
            }
            // Return type resolution needs symbol table...
            break;
        default: break;
    }
}

TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref) {
    if (!type_ref) return type_get_void(ctx->type_registry);
    
    // Simple resolution for now
    Str name = type_ref->parts->text;
    if (str_eq_cstr(name, "Int")) return type_get_int(ctx->type_registry);
    if (str_eq_cstr(name, "Float")) return type_get_float(ctx->type_registry);
    if (str_eq_cstr(name, "Bool")) return type_get_bool(ctx->type_registry);
    if (str_eq_cstr(name, "String")) return type_get_string(ctx->type_registry);
    if (str_eq_cstr(name, "Char")) return type_get_char(ctx->type_registry);
    if (str_eq_cstr(name, "Any")) return type_get_any(ctx->type_registry);

    // TODO: Look up in symbol table for structs/enums
    return type_get_void(ctx->type_registry); 
}
