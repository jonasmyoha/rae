#include "c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
  const AstParam* params;
  Str locals[256];
  size_t local_count;
  bool returns_value;
} CFuncContext;

static bool emit_expr(const CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_function(const AstFuncDecl* func, FILE* out);
static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_log_call(const CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline);
static bool emit_string_literal(FILE* out, Str literal);
static bool emit_param_list(const AstParam* params, FILE* out);
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_while(CFuncContext* ctx, const AstStmt* stmt, FILE* out);

static bool emit_string_literal(FILE* out, Str literal) {
  if (!literal.data) return false;
  return fprintf(out, "%.*s", (int)literal.len, literal.data) >= 0;
}

static bool emit_param_list(const AstParam* params, FILE* out) {
  size_t index = 0;
  const AstParam* param = params;
  if (!param) {
    return fprintf(out, "void") >= 0;
  }
  while (param) {
    if (index > 0) {
      if (fprintf(out, ", ") < 0) return false;
    }
    if (fprintf(out, "int64_t %.*s", (int)param->name.len, param->name.data) < 0) {
      return false;
    }
    param = param->next;
    index += 1;
  }
  return true;
}

static bool emit_log_call(const CFuncContext* ctx, const AstExpr* expr, FILE* out, bool newline) {
  const AstCallArg* arg = expr->as.call.args;
  if (!arg || arg->next) {
    fprintf(stderr,
            "error: C backend expects exactly one argument for log/logS during codegen\n");
    return false;
  }
  const AstExpr* value = arg->value;
  if (value->kind == AST_EXPR_STRING) {
    if (newline) {
      if (fprintf(out, "  puts(") < 0) return false;
      if (!emit_string_literal(out, value->as.string_lit)) return false;
      if (fprintf(out, ");\n") < 0) return false;
    } else {
      if (fprintf(out, "  fputs(") < 0) return false;
      if (!emit_string_literal(out, value->as.string_lit)) return false;
      if (fprintf(out, ", stdout);\n") < 0) return false;
    }
    if (fprintf(out, "  fflush(stdout);\n") < 0) return false;
    return true;
  }
  const char* suffix = newline ? "\\n" : "";
  if (fprintf(out, "  printf(\"%%lld%s\", (long long)(", suffix) < 0) {
    return false;
  }
  if (!emit_expr(ctx, value, out)) {
    return false;
  }
  if (fprintf(out, "));\n") < 0) {
    return false;
  }
  if (fprintf(out, "  fflush(stdout);\n") < 0) return false;
  return true;
}

static bool emit_expr(const CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (!expr) return false;
  switch (expr->kind) {
    case AST_EXPR_STRING:
      return emit_string_literal(out, expr->as.string_lit);
    case AST_EXPR_INTEGER:
      return fprintf(out, "%.*s", (int)expr->as.integer.len, expr->as.integer.data) >= 0;
    case AST_EXPR_BOOL:
      return fprintf(out, "%d", expr->as.boolean ? 1 : 0) >= 0;
    case AST_EXPR_IDENT: {
      for (const AstParam* param = ctx->params; param; param = param->next) {
        if (str_eq(param->name, expr->as.ident)) {
          return fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data) >= 0;
        }
      }
      for (size_t i = 0; i < ctx->local_count; ++i) {
        if (str_eq(ctx->locals[i], expr->as.ident)) {
          return fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data) >= 0;
        }
      }
      fprintf(stderr,
              "error: C backend currently only supports referencing parameters or prior locals "
              "(%.*s)\n",
              (int)expr->as.ident.len, expr->as.ident.data);
      return false;
    }
    case AST_EXPR_BINARY: {
      const char* op = NULL;
      switch (expr->as.binary.op) {
        case AST_BIN_ADD:
          op = "+";
          break;
        case AST_BIN_SUB:
          op = "-";
          break;
        case AST_BIN_MUL:
          op = "*";
          break;
        case AST_BIN_DIV:
          op = "/";
          break;
        case AST_BIN_MOD:
          op = "%";
          break;
        case AST_BIN_LT:
          op = "<";
          break;
        case AST_BIN_GT:
          op = ">";
          break;
        case AST_BIN_LE:
          op = "<=";
          break;
        case AST_BIN_GE:
          op = ">=";
          break;
        case AST_BIN_IS:
          op = "==";
          break;
        case AST_BIN_AND:
          op = "&&";
          break;
        case AST_BIN_OR:
          op = "||";
          break;
        default:
          fprintf(stderr, "error: C backend does not support this binary operator yet\n");
          return false;
      }
      if (fprintf(out, "(") < 0) return false;
      if (!emit_expr(ctx, expr->as.binary.lhs, out)) return false;
      if (fprintf(out, " %s ", op) < 0) return false;
      if (!emit_expr(ctx, expr->as.binary.rhs, out)) return false;
      return fprintf(out, ")") >= 0;
    }
    case AST_EXPR_UNARY:
      if (expr->as.unary.op == AST_UNARY_NEG) {
        if (fprintf(out, "(-") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, ")") >= 0;
      } else if (expr->as.unary.op == AST_UNARY_NOT) {
        if (fprintf(out, "(!") < 0) return false;
        if (!emit_expr(ctx, expr->as.unary.operand, out)) return false;
        return fprintf(out, ")") >= 0;
      }
      fprintf(stderr, "error: C backend unsupported unary operator\n");
      return false;
    default:
      fprintf(stderr, "error: unsupported expression kind in C backend\n");
      return false;
  }
}

static bool emit_call(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (expr->kind != AST_EXPR_CALL || !expr->as.call.callee ||
      expr->as.call.callee->kind != AST_EXPR_IDENT) {
    fprintf(stderr, "error: unsupported expression in C backend (only direct calls allowed)\n");
    return false;
  }
  Str name = expr->as.call.callee->as.ident;
  if (str_eq_cstr(name, "log")) {
    return emit_log_call(ctx, expr, out, true);
  }
  if (str_eq_cstr(name, "logS")) {
    return emit_log_call(ctx, expr, out, false);
  }
  const AstCallArg* arg = expr->as.call.args;
  if (fprintf(out, "  %.*s(", (int)name.len, name.data) < 0) {
    return false;
  }
  size_t arg_index = 0;
  while (arg) {
    if (arg_index > 0) {
      if (fprintf(out, ", ") < 0) return false;
    }
    if (!emit_expr(ctx, arg->value, out)) {
      return false;
    }
    arg = arg->next;
    arg_index += 1;
  }
  if (fprintf(out, ");\n") < 0) {
    return false;
  }
  return true;
}

static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt) return true;
  switch (stmt->kind) {
    case AST_STMT_RET: {
      const AstReturnArg* arg = stmt->as.ret_stmt.values;
      if (!arg) {
        if (ctx->returns_value) {
          fprintf(stderr, "error: return without value in function expecting a value\n");
          return false;
        }
        return fprintf(out, "  return;\n") >= 0;
      }
      if (arg->next) {
        fprintf(stderr, "error: C backend only supports single return values\n");
        return false;
      }
      if (!ctx->returns_value) {
        fprintf(stderr, "error: return with value in non-returning function\n");
        return false;
      }
      if (fprintf(out, "  return ") < 0) {
        return false;
      }
      if (!emit_expr(ctx, arg->value, out)) {
        return false;
      }
      return fprintf(out, ";\n") >= 0;
    }
    case AST_STMT_DEF: {
      if (!stmt->as.def_stmt.value) {
        fprintf(stderr, "error: C backend def statements require an initializer\n");
        return false;
      }
      if (ctx->local_count >= sizeof(ctx->locals) / sizeof(ctx->locals[0])) {
        fprintf(stderr, "error: C backend local limit exceeded\n");
        return false;
      }
      if (fprintf(out, "  int64_t %.*s = ", (int)stmt->as.def_stmt.name.len,
                  stmt->as.def_stmt.name.data) < 0) {
        return false;
      }
      if (!emit_expr(ctx, stmt->as.def_stmt.value, out)) {
        return false;
      }
      if (fprintf(out, ";\n") < 0) {
        return false;
      }
      ctx->locals[ctx->local_count++] = stmt->as.def_stmt.name;
      return true;
    }
    case AST_STMT_EXPR:
      return emit_call(ctx, stmt->as.expr_stmt, out);
    case AST_STMT_IF:
      return emit_if(ctx, stmt, out);
    case AST_STMT_WHILE:
      return emit_while(ctx, stmt, out);
    default:
      fprintf(stderr, "error: C backend does not yet support this statement kind (%d)\n",
              (int)stmt->kind);
      return false;
  }
}

static bool emit_function(const AstFuncDecl* func, FILE* out) {
  if (!func->body) {
    fprintf(stderr, "error: C backend requires function bodies to be present\n");
    return false;
  }
  bool is_main = str_eq_cstr(func->name, "main");
  bool returns_value = func->returns != NULL;
  if (func->returns && func->returns->next) {
    fprintf(stderr, "error: C backend only supports single return values per function\n");
    return false;
  }
  char* name = str_to_cstr(func->name);
  if (!name) return false;
  bool ok = true;
  if (is_main) {
    ok = fprintf(out, "int main(") >= 0;
  } else {
    ok = fprintf(out, "static %s %s(", returns_value ? "int64_t" : "void", name) >= 0;
  }
  if (ok) {
    ok = emit_param_list(func->params, out);
  }
  if (ok) {
    ok = fprintf(out, ") {\n") >= 0;
  }
  if (!ok) {
    free(name);
    return false;
  }
  CFuncContext ctx = {.params = func->params, .local_count = 0, .returns_value = returns_value};
  const AstStmt* stmt = func->body->first;
  while (stmt && ok) {
    ok = emit_stmt(&ctx, stmt, out);
    stmt = stmt->next;
  }
  if (ok && is_main) {
    ok = fprintf(out, "  return 0;\n") >= 0;
  }
  if (ok) {
    ok = fprintf(out, "}\n\n") >= 0;
  }
  free(name);
  return ok;
}

bool c_backend_emit(const AstModule* module, const char* out_path) {
  if (!module || !out_path) return false;
  FILE* out = fopen(out_path, "w");
  if (!out) {
    fprintf(stderr, "error: unable to open '%s' for C backend output\n", out_path);
    return false;
  }
  bool ok = true;
  if (fprintf(out,
              "/* Generated by Rae C backend (experimental) */\n#include <stdio.h>\n#include "
              "<stdint.h>\n\n") < 0) {
    fclose(out);
    return false;
  }
  size_t func_count = 0;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC) {
      func_count += 1;
    }
  }
  if (func_count == 0) {
    fprintf(stderr, "error: C backend requires at least one function (expected func main)\n");
    fclose(out);
    return false;
  }
  const AstFuncDecl** funcs = calloc(func_count, sizeof(*funcs));
  if (!funcs) {
    fclose(out);
    return false;
  }
  size_t idx = 0;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC) {
      funcs[idx++] = &decl->as.func_decl;
    }
  }
  bool has_main = false;
  for (size_t i = 0; i < func_count; ++i) {
    if (!str_eq_cstr(funcs[i]->name, "main")) {
      char* proto = str_to_cstr(funcs[i]->name);
      if (!proto) {
        ok = false;
        break;
      }
      bool returns_value = funcs[i]->returns != NULL;
      if (funcs[i]->returns && funcs[i]->returns->next) {
        fprintf(stderr, "error: C backend only supports single return values per function\n");
        free(proto);
        ok = false;
        break;
      }
      if (fprintf(out, "static %s %s(", returns_value ? "int64_t" : "void", proto) < 0) {
        free(proto);
        ok = false;
        break;
      }
      if (!emit_param_list(funcs[i]->params, out)) {
        free(proto);
        ok = false;
        break;
      }
      if (fprintf(out, ");\n") < 0) {
        free(proto);
        ok = false;
        break;
      }
      free(proto);
    } else {
      has_main = true;
    }
  }
  if (ok && func_count > 1) {
    ok = fprintf(out, "\n") >= 0;
  }
  for (size_t i = 0; ok && i < func_count; ++i) {
    ok = emit_function(funcs[i], out);
  }
  if (ok && !has_main) {
    fprintf(stderr, "error: C backend could not find `func main`\n");
    ok = false;
  }
  free(funcs);
  fclose(out);
  return ok;
}
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt->as.if_stmt.condition || !stmt->as.if_stmt.then_block) {
    fprintf(stderr, "error: C backend requires complete if statements\n");
    return false;
  }
  if (fprintf(out, "  if (") < 0) {
    return false;
  }
  if (!emit_expr(ctx, stmt->as.if_stmt.condition, out)) {
    return false;
  }
  if (fprintf(out, ") {\n") < 0) {
    return false;
  }
  const AstStmt* inner = stmt->as.if_stmt.then_block->first;
  while (inner) {
    if (!emit_stmt(ctx, inner, out)) {
      return false;
    }
    inner = inner->next;
  }
  if (fprintf(out, "  }") < 0) {
    return false;
  }
  if (stmt->as.if_stmt.else_block) {
    if (fprintf(out, " else {\n") < 0) {
      return false;
    }
    const AstStmt* else_stmt = stmt->as.if_stmt.else_block->first;
    while (else_stmt) {
      if (!emit_stmt(ctx, else_stmt, out)) {
        return false;
      }
      else_stmt = else_stmt->next;
    }
    if (fprintf(out, "  }") < 0) {
      return false;
    }
  }
  if (fprintf(out, "\n") < 0) {
    return false;
  }
  return true;
}

static bool emit_while(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt->as.while_stmt.condition || !stmt->as.while_stmt.body) {
    fprintf(stderr, "error: C backend requires while condition and body\n");
    return false;
  }
  if (fprintf(out, "  while (") < 0) {
    return false;
  }
  if (!emit_expr(ctx, stmt->as.while_stmt.condition, out)) {
    return false;
  }
  if (fprintf(out, ") {\n") < 0) {
    return false;
  }
  const AstStmt* inner = stmt->as.while_stmt.body->first;
  while (inner) {
    if (!emit_stmt(ctx, inner, out)) {
      return false;
    }
    inner = inner->next;
  }
  if (fprintf(out, "  }\n") < 0) {
    return false;
  }
  return true;
}
