#include "c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
  const AstParam* params;
} CFuncContext;

static bool emit_expr(const CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_function(const AstFuncDecl* func, FILE* out);
static bool emit_stmt(const CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_call(const CFuncContext* ctx, const AstExpr* expr, FILE* out);
static bool emit_log_call(const AstExpr* expr, FILE* out, bool newline);
static bool emit_string_literal(FILE* out, Str literal);
static bool emit_param_list(const AstParam* params, FILE* out);

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

static bool emit_log_arg(FILE* out, const AstExpr* arg, bool newline) {
  if (arg->kind != AST_EXPR_STRING) {
    fprintf(stderr,
            "error: C backend currently only supports log/logS with string literals (got "
            "non-string argument)\n");
    return false;
  }
  if (newline) {
    if (fprintf(out, "  puts(") < 0) return false;
    if (!emit_string_literal(out, arg->as.string_lit)) return false;
    if (fprintf(out, ");\n") < 0) return false;
  } else {
    if (fprintf(out, "  fputs(") < 0) return false;
    if (!emit_string_literal(out, arg->as.string_lit)) return false;
    if (fprintf(out, ", stdout);\n") < 0) return false;
  }
  if (fprintf(out, "  fflush(stdout);\n") < 0) return false;
  return true;
}

static bool emit_log_call(const AstExpr* expr, FILE* out, bool newline) {
  const AstCallArg* arg = expr->as.call.args;
  if (!arg || arg->next) {
    fprintf(stderr,
            "error: C backend expects exactly one argument for log/logS during codegen\n");
    return false;
  }
  if (!emit_log_arg(out, arg->value, newline)) {
    return false;
  }
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
      fprintf(stderr,
              "error: C backend currently only supports referencing function parameters (%.*s)\n",
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
      }
      fprintf(stderr, "error: C backend unsupported unary operator\n");
      return false;
    default:
      fprintf(stderr, "error: unsupported expression kind in C backend\n");
      return false;
  }
}

static bool emit_call(const CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  if (expr->kind != AST_EXPR_CALL || !expr->as.call.callee ||
      expr->as.call.callee->kind != AST_EXPR_IDENT) {
    fprintf(stderr, "error: unsupported expression in C backend (only direct calls allowed)\n");
    return false;
  }
  Str name = expr->as.call.callee->as.ident;
  if (str_eq_cstr(name, "log")) {
    return emit_log_call(expr, out, true);
  }
  if (str_eq_cstr(name, "logS")) {
    return emit_log_call(expr, out, false);
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

static bool emit_stmt(const CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  if (!stmt) return true;
  switch (stmt->kind) {
    case AST_STMT_EXPR:
      return emit_call(ctx, stmt->as.expr_stmt, out);
    default:
      fprintf(stderr, "error: C backend does not yet support this statement kind\n");
      return false;
  }
}

static bool emit_function(const AstFuncDecl* func, FILE* out) {
  if (!func->body) {
    fprintf(stderr, "error: C backend requires function bodies to be present\n");
    return false;
  }
  bool is_main = str_eq_cstr(func->name, "main");
  char* name = str_to_cstr(func->name);
  if (!name) return false;
  bool ok = true;
  if (is_main) {
    ok = fprintf(out, "int main(") >= 0;
  } else {
    ok = fprintf(out, "static void %s(", name) >= 0;
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
  CFuncContext ctx = {.params = func->params};
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
      if (fprintf(out, "static void %s(", proto) < 0) {
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
