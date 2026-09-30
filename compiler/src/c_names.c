/* C-safe names for Rae identifiers.
 *
 * A Rae local, parameter or field may be spelled like a C keyword (`short`,
 * `long`, `signed`, `int`, `static`, ...), a macro the generated C sees
 * (`bool`, `NULL`, `errno`, `stdin`, ...) or a C library function the generated
 * code calls in the same body (`free`, `malloc`, `memcpy`, ...). Emitted
 * verbatim, `rae_String short = ...` is not C. c_rename_reserved_names runs once
 * after sema, before any C is emitted, and renames every such identifier in the
 * AST to `<name>__kw` — declaration and every use together, so the C backend
 * never sees the original spelling and needs no per-site mangling.
 *
 * Diagnostics are unaffected (sema has already run). Wherever the backend
 * prints a name AS TEXT — toJson / fromJson keys, the struct toString,
 * fieldName() — it goes through rae_source_name, which strips the suffix, so
 * the program still sees `short`.
 *
 * Not renamed: function names and anything in callee position (the `sizeof`
 * intrinsic, for one), type names (PascalCase), enum cases (emitted with their
 * enum's prefix, so `JsonKind.bool` is already valid C) and the fields of a
 * `c_struct` type (they mirror a real C struct).
 */
#include <string.h>

#include "c_backend_internal.h"

static const char* const k_reserved[] = {
    /* C11 keywords */
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool",
    "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
    "_Thread_local",
    /* macros from the headers the generated C includes */
    "bool", "true", "false", "NULL", "errno", "stdin", "stdout", "stderr",
    "EOF", "alignas", "alignof", "noreturn", "static_assert", "thread_local",
    "complex", "imaginary", "offsetof", "assert",
    /* C library functions the generated code calls inside a Rae body */
    "free", "malloc", "calloc", "realloc", "memcpy", "memmove", "memcmp",
    "memset", "strlen", "snprintf", "printf", "fprintf",
};

#define RESERVED_SUFFIX "__kw"

bool c_reserved_name(Str name) {
  for (size_t i = 0; i < sizeof(k_reserved) / sizeof(k_reserved[0]); i++) {
    if (str_eq_cstr(name, k_reserved[i])) return true;
  }
  return false;
}

Str rae_source_name(Str name) {
  size_t suffix = sizeof(RESERVED_SUFFIX) - 1;
  if (name.len > suffix && memcmp(name.data + name.len - suffix, RESERVED_SUFFIX, suffix) == 0) {
    Str base = {name.data, name.len - suffix};
    if (c_reserved_name(base)) return base;
  }
  return name;
}

typedef struct {
  CompilerContext* ctx;
} Renamer;

static void rename_str(Renamer* r, Str* name) {
  if (!name || !name->data || !c_reserved_name(*name)) return;
  size_t len = name->len + sizeof(RESERVED_SUFFIX) - 1;
  char* buf = arena_alloc(r->ctx->ast_arena, len + 1);
  memcpy(buf, name->data, name->len);
  memcpy(buf + name->len, RESERVED_SUFFIX, sizeof(RESERVED_SUFFIX));
  name->data = buf;
  name->len = len;
}

/* `Kind.case` / `Module.Kind.case`: the object names an enum, so the member is
 * an enum case and keeps its spelling. */
static bool names_enum(Renamer* r, const AstExpr* object) {
  if (!object) return false;
  Str tail;
  if (object->kind == AST_EXPR_IDENT) tail = object->as.ident;
  else if (object->kind == AST_EXPR_MEMBER) tail = object->as.member.member;
  else return false;
  for (size_t i = 0; i < r->ctx->all_decl_count; i++) {
    const AstDecl* d = r->ctx->all_decls[i];
    if (d->kind == AST_DECL_ENUM && str_eq(d->as.enum_decl.name, tail)) return true;
  }
  return false;
}

static void walk_expr(Renamer* r, AstExpr* e);
static void walk_block(Renamer* r, AstBlock* b);
static void walk_stmt(Renamer* r, AstStmt* s);

static void walk_args(Renamer* r, AstCallArg* a) {
  for (; a; a = a->next) {
    rename_str(r, &a->name);
    walk_expr(r, a->value);
  }
}

static void walk_expr(Renamer* r, AstExpr* e) {
  if (!e) return;
  switch (e->kind) {
    case AST_EXPR_IDENT:
      rename_str(r, &e->as.ident);
      break;
    case AST_EXPR_BINARY:
      walk_expr(r, e->as.binary.lhs);
      walk_expr(r, e->as.binary.rhs);
      break;
    case AST_EXPR_UNARY:
    case AST_EXPR_BOX:
    case AST_EXPR_UNBOX:
    case AST_EXPR_OWN:
      walk_expr(r, e->as.unary.operand);
      break;
    case AST_EXPR_CAST:
      walk_expr(r, e->as.cast.operand);
      break;
    case AST_EXPR_CALL:
      /* A callee is a function or an intrinsic (`sizeof`): never renamed. */
      if (e->as.call.callee && e->as.call.callee->kind != AST_EXPR_IDENT)
        walk_expr(r, e->as.call.callee);
      walk_args(r, e->as.call.args);
      break;
    case AST_EXPR_MEMBER:
      /* The enum check scans every declaration, so only for a name that
       * would be renamed at all. */
      if (c_reserved_name(e->as.member.member) && !names_enum(r, e->as.member.object))
        rename_str(r, &e->as.member.member);
      walk_expr(r, e->as.member.object);
      break;
    case AST_EXPR_OBJECT:
      for (AstObjectField* f = e->as.object_literal.fields; f; f = f->next) {
        rename_str(r, &f->name);
        walk_expr(r, f->value);
      }
      break;
    case AST_EXPR_MATCH:
      walk_expr(r, e->as.match_expr.subject);
      for (AstMatchArm* arm = e->as.match_expr.arms; arm; arm = arm->next) {
        walk_expr(r, arm->pattern);
        walk_expr(r, arm->value);
      }
      break;
    case AST_EXPR_LIST:
      for (AstExprList* it = e->as.list; it; it = it->next) walk_expr(r, it->value);
      break;
    case AST_EXPR_INDEX:
      walk_expr(r, e->as.index.target);
      walk_expr(r, e->as.index.index);
      break;
    case AST_EXPR_METHOD_CALL:
      walk_expr(r, e->as.method_call.object);
      walk_args(r, e->as.method_call.args);
      break;
    case AST_EXPR_COLLECTION_LITERAL:
      for (AstCollectionElement* el = e->as.collection.elements; el; el = el->next)
        walk_expr(r, el->value);
      break;
    case AST_EXPR_INTERP:
      for (AstInterpPart* p = e->as.interp.parts; p; p = p->next) walk_expr(r, p->value);
      break;
    default:
      break;
  }
}

static void walk_stmt(Renamer* r, AstStmt* s) {
  if (!s) return;
  switch (s->kind) {
    case AST_STMT_LET:
      rename_str(r, &s->as.let_stmt.name);
      walk_expr(r, s->as.let_stmt.value);
      break;
    case AST_STMT_DESTRUCT:
      for (AstDestructureBinding* b = s->as.destruct_stmt.bindings; b; b = b->next) {
        rename_str(r, &b->local_name);
        rename_str(r, &b->return_label);
      }
      walk_expr(r, s->as.destruct_stmt.call);
      break;
    case AST_STMT_EXPR:
      walk_expr(r, s->as.expr_stmt);
      break;
    case AST_STMT_RET:
      for (AstReturnArg* a = s->as.ret_stmt.values; a; a = a->next) {
        if (a->has_label) rename_str(r, &a->label);
        walk_expr(r, a->value);
      }
      break;
    case AST_STMT_IF:
      walk_stmt(r, s->as.if_stmt.binding);
      walk_expr(r, s->as.if_stmt.condition);
      walk_block(r, s->as.if_stmt.then_block);
      walk_block(r, s->as.if_stmt.else_block);
      break;
    case AST_STMT_LOOP:
      walk_stmt(r, s->as.loop_stmt.init);
      walk_expr(r, s->as.loop_stmt.condition);
      walk_expr(r, s->as.loop_stmt.increment);
      for (AstQueryLoopBinding* b = s->as.loop_stmt.query_bindings; b; b = b->next)
        rename_str(r, &b->name);
      walk_expr(r, s->as.loop_stmt.query_iterable);
      walk_block(r, s->as.loop_stmt.body);
      break;
    case AST_STMT_MATCH:
      walk_stmt(r, s->as.match_stmt.binding);
      walk_expr(r, s->as.match_stmt.subject);
      for (AstMatchCase* c = s->as.match_stmt.cases; c; c = c->next) {
        walk_expr(r, c->pattern);
        for (AstCasePattern* p = c->or_patterns; p; p = p->next) walk_expr(r, p->expr);
        walk_block(r, c->block);
      }
      break;
    case AST_STMT_ASSIGN:
      walk_expr(r, s->as.assign_stmt.target);
      walk_expr(r, s->as.assign_stmt.value);
      break;
    case AST_STMT_DEFER:
      walk_block(r, s->as.defer_stmt.block);
      break;
    case AST_STMT_UNSAFE:
      walk_block(r, s->as.unsafe_stmt.block);
      break;
    default:
      break;
  }
}

static void walk_block(Renamer* r, AstBlock* b) {
  if (!b) return;
  for (AstStmt* s = b->first; s; s = s->next) walk_stmt(r, s);
}

void c_rename_reserved_names(CompilerContext* ctx) {
  Renamer r = {.ctx = ctx};
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    AstDecl* d = (AstDecl*)ctx->all_decls[i];
    switch (d->kind) {
      case AST_DECL_TYPE:
        if (has_property(d->as.type_decl.properties, "c_struct")) break;
        for (AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
          rename_str(&r, &f->name);
          walk_expr(&r, f->default_value);
        }
        break;
      case AST_DECL_FUNC:
        for (AstParam* p = d->as.func_decl.params; p; p = p->next) rename_str(&r, &p->name);
        for (AstReturnItem* ri = d->as.func_decl.returns; ri; ri = ri->next)
          if (ri->has_name) rename_str(&r, &ri->name);
        walk_block(&r, d->as.func_decl.body);
        break;
      case AST_DECL_GLOBAL_LET:
        rename_str(&r, &d->as.let_decl.name);
        walk_expr(&r, d->as.let_decl.value);
        break;
      default:
        break;
    }
  }
}

/* Copy generated C to `out`, restoring the Rae spelling of a renamed name
 * wherever it sits INSIDE a C string literal — a JSON key, a key looked up by
 * fromJson. Identifiers outside literals (`this->short__kw`) are the C names
 * and stay. Only for generated helper code whose literals carry no byte length
 * (the toJson / fromJson block); a length-annotated literal must be built from
 * rae_source_name directly. */
void c_restore_names_in_literals(const char* src, size_t len, FILE* out) {
  bool in_string = false;
  size_t i = 0;
  while (i < len) {
    char c = src[i];
    if (!in_string) {
      if (c == '\'' ) {  /* a char literal: copy through its closing quote */
        fputc(c, out); i++;
        while (i < len && src[i] != '\'') {
          if (src[i] == '\\' && i + 1 < len) { fputc(src[i], out); i++; }
          fputc(src[i], out); i++;
        }
        if (i < len) { fputc(src[i], out); i++; }
        continue;
      }
      if (c == '"') in_string = true;
      fputc(c, out); i++;
      continue;
    }
    if (c == '\\' && i + 1 < len) {
      fputc(c, out); fputc(src[i + 1], out); i += 2;
      continue;
    }
    if (c == '"') { in_string = false; fputc(c, out); i++; continue; }
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      size_t start = i;
      while (i < len && (src[i] == '_' || (src[i] >= 'a' && src[i] <= 'z') ||
                         (src[i] >= 'A' && src[i] <= 'Z') || (src[i] >= '0' && src[i] <= '9')))
        i++;
      Str word = {src + start, i - start};
      Str spelled = rae_source_name(word);
      fwrite(spelled.data, 1, spelled.len, out);
      continue;
    }
    fputc(c, out); i++;
  }
}
