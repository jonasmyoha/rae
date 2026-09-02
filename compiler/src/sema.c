#include "sema.h"
#include "type.h"
#include "ast.h"
#include "diag.h"
#include "mangler.h"
#include "ownership.h"
#include "c_backend.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Origin-file of the function currently being analyzed. Set/restored
// in sema_analyze_decl's AST_DECL_FUNC arm; read by the call-site
// check that forbids raw rae_ext_rae_buf_set with cascade-drop element
// types outside stdlib. NULL = unknown / top-level scope.
static const char* s_current_decl_origin = NULL;
/* Module under analysis, so deep helpers (ensure_type_match) can flag a
 * hard error and actually fail the build rather than just printing. */
static AstModule* s_current_module = NULL;

/* True while analyzing an `if let` binding statement. There, an `opt T`
 * initializer bound to a non-optional name is the NARROWING itself (the
 * value is unwrapped inside the success branch), so ensure_type_match must
 * keep the old unbox behavior instead of erroring "optional not unwrapped".
 * Set/restored around the binding analysis in the AST_STMT_IF arm. */
static bool s_in_if_let_binding = false;

/* Loop nesting depth during statement analysis, so `break`/`continue`
 * can be rejected outside any loop. Balanced increment/decrement around
 * each loop body; there are no nested function bodies, so it returns to
 * 0 between top-level function analyses. */
static int s_loop_depth = 0;

// Per-file import/open directives (docs/module-namespacing.md). Registered from
// the module graph before sema; consulted via s_current_decl_origin so a call
// resolves names against ITS file's imports (aliases, open-vs-import). Auto-
// loaded modules are auto-opened, so they need no per-file directive.
typedef struct SemaFileScope {
    const char* file;
    AstImport* imports;
    struct SemaFileScope* next;
} SemaFileScope;
static SemaFileScope* s_file_scopes = NULL;
static AstImport* sema_imports_for_file(const char* file);  // fwd: used by eligibility helpers below

// Prelude packages (the intentional auto-load set) — opened for every file.
typedef struct SemaGlobalOpen { const char* module; struct SemaGlobalOpen* next; } SemaGlobalOpen;
static SemaGlobalOpen* s_global_opens = NULL;

void sema_reset_file_scopes(void) { s_file_scopes = NULL; s_global_opens = NULL; }

void sema_register_global_open(Arena* arena, const char* module) {
    if (!module) return;
    SemaGlobalOpen* g = arena_alloc(arena, sizeof(SemaGlobalOpen));
    g->module = module; g->next = s_global_opens; s_global_opens = g;
}

static bool sema_module_in_prelude(const char* module) {
    for (SemaGlobalOpen* g = s_global_opens; g; g = g->next)
        if (g->module && module && strcmp(g->module, module) == 0) return true;
    return false;
}

// Package identity (docs/module-namespacing.md). A file's package:
//   * stdlib root (path contains "/lib/" or starts "lib/"): the first path
//     component after lib/, with the .rae extension stripped — so lib/ui.rae,
//     lib/ui/ecs.rae, lib/ui/render.rae all map to "ui"; lib/string.rae -> "string".
//   * otherwise (under the project root): the single project package "" .
static void sema_package_token(const char* path, char* out, size_t cap) {
    out[0] = '\0';
    if (!path) return;
    const char* after = NULL;
    const char* lib = strstr(path, "/lib/");
    if (lib) after = lib + 5;
    else if (strncmp(path, "lib/", 4) == 0) after = path + 4;
    if (!after) return;                      // project file -> package "" (the project)
    size_t i = 0;
    while (after[i] && after[i] != '/' && after[i] != '\\' && i + 1 < cap) { out[i] = after[i]; i++; }
    out[i] = '\0';
    size_t n = strlen(out);
    if (n > 4 && strcmp(out + n - 4, ".rae") == 0) out[n - 4] = '\0';  // lib/string.rae -> "string"
}

static bool sema_same_package(const char* a, const char* b) {
    char pa[256], pb[256];
    sema_package_token(a, pa, sizeof(pa));
    sema_package_token(b, pb, sizeof(pb));
    return strcmp(pa, pb) == 0;
}

// A project decl's FOLDER NAMESPACE: the name of the directory that directly
// contains its source file — `.../game/enemies/tick.rae` -> "enemies",
// `.../game/ui/hud.rae` -> "ui". Derived from the file path (not the root-
// relative module name), so it is correct no matter where the lib-marker
// project root sits. This is a DISAMBIGUATION name only; it is NOT a visibility
// boundary — every project file stays mutually visible (auto-open, see
// sema_decl_opened). Lib decls have no project namespace (they use
// import/open), so this returns "" for them. (docs/module-namespacing.md)
static void sema_project_namespace(const AstDecl* d, char* out, size_t cap) {
    if (cap) out[0] = '\0';
    if (!d || !d->origin_file || !cap) return;
    char pkg[256]; sema_package_token(d->origin_file, pkg, sizeof pkg);
    if (pkg[0] != '\0') return;  // a lib decl — not a project folder namespace
    const char* path = d->origin_file;
    const char* last = strrchr(path, '/');       // slash before the filename
    if (!last || last == path) return;           // no containing directory
    const char* start = last - 1;                // scan back to the parent dir name
    while (start > path && *(start - 1) != '/') start--;
    size_t n = (size_t)(last - start);
    if (n == 0) return;
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

// First component of an import-directive path (the package it refers to), with
// any .rae stripped: "ui/ecs" -> "ui", "raylib" -> "raylib", "sys/spotify" -> "sys".
static void sema_import_package(Str path, char* out, size_t cap) {
    size_t i = 0;
    while (i < path.len && path.data[i] != '/' && path.data[i] != '\\' && i + 1 < cap) { out[i] = path.data[i]; i++; }
    out[i] = '\0';
    size_t n = strlen(out);
    if (n > 4 && strcmp(out + n - 4, ".rae") == 0) out[n - 4] = '\0';
}

// Does `file` declare `import`/`open` for the package that owns `d`? `open` only
// counts when require_open is true (bare lookup); `import` or `open` both count
// otherwise (qualified / UFCS). Directives are package-granular: opening any
// module in a package opens the whole package. (docs/module-namespacing.md)
static bool sema_file_declares_package(const char* file, const AstDecl* d, bool require_open) {
    if (!d->origin_file) return false;
    char dpkg[256]; sema_package_token(d->origin_file, dpkg, sizeof dpkg);
    for (AstImport* im = sema_imports_for_file(file); im; im = im->next) {
        if (require_open && !im->is_open) continue;
        if (!im->path.data) continue;
        char ipkg[256]; sema_import_package(im->path, ipkg, sizeof ipkg);
        if (strcmp(ipkg, dpkg) == 0) return true;
    }
    return false;
}

// Is callee `d` reachable by a BARE call from `file`? (same file | same package |
// prelude | a package `open`ed here). The open-set. (docs/module-namespacing.md)
static bool sema_decl_opened(const char* file, const AstDecl* d) {
    if (!d->origin_file) return true;
    if (file && strcmp(file, d->origin_file) == 0) return true;       // same file
    if (sema_same_package(file, d->origin_file)) return true;         // same package
    if (d->module_name && sema_module_in_prelude(d->module_name)) return true;  // prelude
    return sema_file_declares_package(file, d, /*require_open=*/true);
}

// Is callee `d` reachable by QUALIFICATION or general UFCS from `file`? The
// open-set plus `import`ed (not opened) packages (import grants qualified+UFCS).
static bool sema_decl_visible(const char* file, const AstDecl* d) {
    if (sema_decl_opened(file, d)) return true;
    return sema_file_declares_package(file, d, /*require_open=*/false);
}

void sema_register_file_imports(Arena* arena, const char* file, AstImport* imports) {
    if (!file) return;
    SemaFileScope* s = arena_alloc(arena, sizeof(SemaFileScope));
    s->file = file;
    s->imports = imports;
    s->next = s_file_scopes;
    s_file_scopes = s;
}

static AstImport* sema_imports_for_file(const char* file) {
    if (!file) return NULL;
    for (SemaFileScope* s = s_file_scopes; s; s = s->next) {
        if (s->file && strcmp(s->file, file) == 0) return s->imports;
    }
    return NULL;
}

// If `qualifier` is an `import/open X as qualifier` alias in `file`, return X's
// module name (the import path); else {0}. The alias only renames explicit
// qualification — UFCS still uses the real function name.
static Str sema_resolve_alias(const char* file, Str qualifier) {
    for (AstImport* im = sema_imports_for_file(file); im; im = im->next) {
        if (im->alias.data && im->alias.len && str_eq(im->alias, qualifier)) {
            return im->path;
        }
    }
    return (Str){0};
}

typedef struct Symbol Symbol;
// Why a binding is immutable — drives a precise reassignment diagnostic.
typedef enum {
    BIND_MUTABLE = 0,   // var / regular
    BIND_LET,           // immutable `let` binding
    BIND_CONST,         // compile-time `const`
    BIND_READONLY_REF   // read-only borrow (view param)
} BindKind;
struct Symbol {
    Str name;
    AstDecl* decl;
    TypeInfo* type;
    int scope_depth;
    bool is_immutable;
    BindKind bind_kind;
    /* The own-argument rule (docs/value-aggregates-and-ownership.md §2.2):
     * this binding does NOT own its heap storage, so passing it to an
     * `own T` parameter would hand the callee something it must not free.
     * Set for view/mod params, for globals, and — one hop only, §2.4 — for
     * a local initialised directly from one of those. It is one bit fixed
     * at declaration, never dataflow. */
    bool is_non_owning;
    /* This binding's storage lives in the current function's frame, so a
     * reference to it dies at `ret`. True for `let`/`var` inside a body;
     * false for parameters (the caller owns that storage) and for globals
     * (they outlive every frame). Drives the "reference escapes local
     * storage" check. */
    bool is_local_storage;
    // Folded value for `const` symbols (so later consts can reference them).
    bool const_is_number;
    bool const_is_float;
    double const_d;
    long long const_i;
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

static Symbol* symbol_table_define(SymbolTable* table, Arena* arena, Str name, AstDecl* decl, TypeInfo* type, bool is_immutable) {
    Symbol* sym = arena_alloc(arena, sizeof(Symbol));
    sym->name = name;
    sym->decl = decl;
    sym->type = type;
    sym->scope_depth = table->current_depth;
    sym->is_immutable = is_immutable;
    // Default: an immutable symbol is a read-only borrow (view param) unless a
    // caller refines it to BIND_LET / BIND_CONST.
    sym->bind_kind = is_immutable ? BIND_READONLY_REF : BIND_MUTABLE;
    sym->is_non_owning = false;
    sym->is_local_storage = false;
    sym->const_is_number = false;
    sym->const_is_float = false;
    sym->const_d = 0.0;
    sym->const_i = 0;
    sym->next = table->head;
    table->head = sym;
    return sym;
}

static Symbol* symbol_table_lookup(SymbolTable* table, Str name) {
    Symbol* curr = table->head;
    while (curr) {
        if (str_eq(curr->name, name)) return curr;
        curr = curr->next;
    }
    return NULL;
}

// Defined further down (near the statement analyzer); used earlier by the
// module-level global/const decl analysis.
static void sema_fold_const(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                            AstExpr* init, Symbol* sym, size_t line, size_t col);

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
static bool sema_is_list_value_accessor(const AstExpr* expr);
static void sema_analyze_decl(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* decl);
static void sema_analyze_expr(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* expr);
static void sema_analyze_stmt(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt, TypeInfo* current_return_type);
static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstTypeRef* type_ref);
static void sema_check_own_args(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, const AstFuncDecl* fd, AstCallArg* args, bool skip_receiver);
static bool expr_is_owning(SymbolTable* symbols, const AstExpr* e);
static TypeInfo* sema_array_type_from_call(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* expr);
static void ensure_type_match(CompilerContext* ctx, TypeInfo* expected, AstExpr** expr_ptr);
static bool sema_is_numeric_kind(TypeKind k);
static AstDecl* specialize_decl(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* generic_decl, TypeInfo** args, size_t arg_count, size_t line, size_t column);

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
    /* Same rule as substitute_type_ref: an Array's count lives in the
     * TypeInfo, not in its name, so clearing resolved_type would make the
     * clone unresolvable and mangle every cap to the same name. */
    res->resolved_type = (tr->resolved_type && tr->resolved_type->kind == TYPE_ARRAY)
        ? tr->resolved_type : NULL;
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
                /* resolved_type is normally cleared so the substituted ref
                 * re-resolves in its new context. Array(T, cap: N) must keep
                 * it: its count lives in the TypeInfo, not in the name, so a
                 * bare "Array" ref cannot be re-resolved and would report a
                 * malformed-Array error against synthesized, line-0 nodes. */
                result->resolved_type = (match->resolved_type && match->resolved_type->kind == TYPE_ARRAY)
                    ? match->resolved_type : NULL;
                if (type->is_view) result->is_view = true;
                if (type->is_mod) result->is_mod = true;
                if (type->is_own) result->is_own = true;
                if (type->is_copy) result->is_copy = true;
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
        new_type->resolved_type = NULL;
        if (type->is_view) new_type->is_view = true;
        if (type->is_mod) new_type->is_mod = true;
        if (type->is_opt) new_type->is_opt = true;
        return new_type;
    }
    AstTypeRef* result = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
    *result = *type;
    result->resolved_type = NULL;
    result->next = NULL;
    result->parts = clone_parts(ctx, type->parts);
    return result;
}

static AstCallArg* clone_call_args(Arena* arena, const AstCallArg* arg);
static AstExpr* clone_expr(Arena* arena, const AstExpr* expr) {
    if (!expr) return NULL;
    AstExpr* res = arena_alloc(arena, sizeof(AstExpr));
    *res = *expr;
    res->resolved_type = NULL;
    res->decl_link = NULL;
    switch (expr->kind) {
        case AST_EXPR_BINARY:
            res->as.binary.lhs = clone_expr(arena, expr->as.binary.lhs);
            res->as.binary.rhs = clone_expr(arena, expr->as.binary.rhs);
            break;
        case AST_EXPR_UNARY:
            res->as.unary.operand = clone_expr(arena, expr->as.unary.operand);
            break;
        case AST_EXPR_CAST:
            res->as.cast.operand = clone_expr(arena, expr->as.cast.operand);
            res->as.cast.target = clone_type_ref(arena, expr->as.cast.target);
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
        case AST_EXPR_INTERP: {
            AstInterpPart* head = NULL; AstInterpPart* tail = NULL;
            AstInterpPart* curr = expr->as.interp.parts;
            while (curr) {
                AstInterpPart* p = arena_alloc(arena, sizeof(AstInterpPart)); *p = *curr;
                p->value = clone_expr(arena, curr->value); p->next = NULL;
                if (!head) head = p; else tail->next = p; tail = p; curr = curr->next;
            }
            res->as.interp.parts = head;
            break;
        }
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
    // Note: some statements like let_stmt have type_info that might need clearing or re-resolving
    // but the analyzer will overwrite them.
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
            // The `if let` binding is part of the statement; without this a
            // generic instantiation would silently lose it.
            res->as.if_stmt.binding = clone_stmt(arena, stmt->as.if_stmt.binding);
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

static AstDecl* specialize_decl(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* generic_decl, TypeInfo** args, size_t arg_count, size_t line, size_t column) {
    (void)line;
    (void)column;
    AstDecl* existing = type_registry_find_specialization(ctx->type_registry, generic_decl, args, arg_count);
    if (existing) return existing;
    AstDecl* spec = arena_alloc(ctx->ast_arena, sizeof(AstDecl));
    *spec = *generic_decl; spec->next = NULL;
    if (spec->kind == AST_DECL_FUNC) {
        spec->as.func_decl.generic_template = generic_decl;
        // Specializations are NOT templates — clear generic_params.
        // Use generic_template->generic_params when param names are needed.
        spec->as.func_decl.generic_params = NULL;
    }
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

        // Re-analyze body with concrete types
        if (spec->as.func_decl.body && symbols) {
            symbol_table_push_scope(symbols);

            // Add generic parameters to scope as their concrete types
            const AstIdentifierPart* gp = generic_decl->as.func_decl.generic_params;
            const AstTypeRef* ga = args_tr;
            while (gp && ga) {
                TypeInfo* ti = ga->resolved_type;
                symbol_table_define(symbols, ctx->ast_arena, gp->text, NULL, ti, false);
                // Also define the concrete name in case it's used directly
                if (ti && ti->name.len > 0 && !str_eq(ti->name, gp->text)) {
                    symbol_table_define(symbols, ctx->ast_arena, ti->name, NULL, ti, false);
                }
                gp = gp->next; ga = ga->next;
            }

            // Add params to scope
            for (AstParam* p = spec->as.func_decl.params; p; p = p->next) {
                TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type);
                symbol_table_define(symbols, ctx->ast_arena, p->name, NULL, pt, false);
            }
            TypeInfo* ret_type = NULL;
            if (spec->as.func_decl.returns) ret_type = sema_resolve_type_internal(ctx, module, symbols, spec->as.func_decl.returns->type);
            sema_analyze_stmt(ctx, module, symbols, spec->as.func_decl.body->first, ret_type);
            symbol_table_pop_scope(symbols);
        }
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
        
        TypeInfo* spec_ti = type_get_struct(ctx->type_registry, generic_decl, args, arg_count);
        spec_ti->as.structure.generic_count = arg_count;
        spec_ti->as.structure.generic_args = arena_alloc(ctx->ast_arena, sizeof(TypeInfo*) * arg_count);
        memcpy(spec_ti->as.structure.generic_args, args, sizeof(TypeInfo*) * arg_count);
        
        // Generate a consistent specialized name (e.g. List_int64_t)
        char name_buf[1024]; size_t name_pos = 0;
        name_pos += snprintf(name_buf + name_pos, sizeof(name_buf) - name_pos, "%.*s", (int)generic_decl->as.type_decl.name.len, generic_decl->as.type_decl.name.data);
        if (arg_count > 0) {
            name_pos += snprintf(name_buf + name_pos, sizeof(name_buf) - name_pos, "_");
            for (size_t i = 0; i < arg_count; i++) {
                Str arg_mangled = type_mangle_name(ctx->ast_arena, args[i]);
                // Strip 'rae_' prefix from arg name for the internal TypeInfo name
                const char* arg_data = arg_mangled.data; size_t arg_len = arg_mangled.len;
                if (arg_len > 4 && memcmp(arg_data, "rae_", 4) == 0) { arg_data += 4; arg_len -= 4; }
                
                name_pos += snprintf(name_buf + name_pos, sizeof(name_buf) - name_pos, "%.*s", (int)arg_len, arg_data);
                if (i < arg_count - 1) name_pos += snprintf(name_buf + name_pos, sizeof(name_buf) - name_pos, "_");
            }
        }
        char* spec_name = arena_alloc(ctx->ast_arena, name_pos + 1);
        memcpy(spec_name, name_buf, name_pos + 1);
        spec_ti->name = (Str){ .data = spec_name, .len = name_pos };

        spec->resolved_type = spec_ti;

        AstTypeRef* tr = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
        tr->parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
        tr->parts->text = generic_decl->as.type_decl.name;
        tr->generic_args = args_tr;
        tr->resolved_type = spec->resolved_type;
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
    if (!type) return (Str){0};
    if (type->parts) return type->parts->text;
    if (type->resolved_type) {
        TypeInfo* t = type->resolved_type;
        while (t && (t->kind == TYPE_REF || t->kind == TYPE_OPT)) {
            if (t->kind == TYPE_REF) t = t->as.ref.base;
            else if (t->kind == TYPE_OPT) t = t->as.opt.base;
            else break;
        }
        if (t) {
            if (t->kind == TYPE_STRUCT && t->as.structure.decl && t->as.structure.decl->kind == AST_DECL_TYPE) {
                const AstTypeDecl* td = &t->as.structure.decl->as.type_decl;
                if (td->generic_template && td->generic_template->kind == AST_DECL_TYPE) {
                    return td->generic_template->as.type_decl.name;
                }
            }
            return t->name;
        }
    }
    return (Str){0};
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
    // Bases need only align when the type params are NESTED (ComponentTable(A) vs
    // ComponentTable(Pos)); a BARE generic-param pattern (`world: W`) binds W to
    // the whole concrete type and deliberately does NOT require the bases equal.
    bool bases_align = str_eq(pattern_base, receiver_base);
    AstTypeRef* inferred_list = NULL; AstTypeRef* last_inferred = NULL;
    for (const AstIdentifierPart* gp = func->generic_params; gp; gp = gp->next) {
        AstTypeRef* match = NULL;
        if (str_eq(pattern_base, gp->text)) {
            // Bare generic-param pattern: bind the param to the whole concrete
            // type. view/mod/own on the pattern are PARAMETER modes, not part of
            // the type argument, so strip them.
            AstTypeRef* bare = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
            *bare = *concrete_type; bare->next = NULL;
            bare->is_view = false; bare->is_mod = false; bare->is_own = false;
            bare->is_copy = false; bare->is_val = false; bare->is_opt = false;
            match = bare;
        } else if (bases_align) {
        const AstTypeRef* p_arg = pattern->generic_args; const AstTypeRef* r_arg = concrete_type->generic_args;
        while (p_arg && r_arg) {
            Str p_name = get_base_type_name(p_arg);
            if (str_eq(p_name, gp->text)) { match = (AstTypeRef*)r_arg; break; }
            p_arg = p_arg->next; r_arg = r_arg->next;
        }
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

// Multi-type-param inference: bind EVERY generic param of `func` by matching all
// its param patterns against the corresponding call-argument types, accumulating
// across params. `patterns[k]`/`concretes[k]` are the k-th param's declared type
// and its call argument's inferred type (parallel arrays of length `pair_count`).
// Unlike infer_generic_args — which requires a SINGLE pattern to bind every param
// and so only ever works for one type param — this binds each param from
// whichever argument mentions it, so query2(A, B, a: T(A), b: T(B)) binds A from
// `a` and B from `b`. Returns the full ordered concrete-args list iff every
// generic param got bound, else NULL. For a one-type-param function it yields the
// same result as the single-pattern path.
AstTypeRef* infer_generic_args_multi(CompilerContext* ctx, const AstFuncDecl* func,
                                     const AstTypeRef** patterns, const AstTypeRef** concretes,
                                     size_t pair_count) {
    if (!func || !func->generic_params) return NULL;
    size_t gcount = 0;
    for (const AstIdentifierPart* gp = func->generic_params; gp; gp = gp->next) gcount++;
    if (gcount == 0 || gcount > 32) return NULL;
    const AstTypeRef* binding[32];
    for (size_t i = 0; i < gcount; i++) binding[i] = NULL;
    size_t gi = 0;
    for (const AstIdentifierPart* gp = func->generic_params; gp; gp = gp->next, gi++) {
        for (size_t k = 0; k < pair_count && !binding[gi]; k++) {
            const AstTypeRef* pat = patterns[k];
            const AstTypeRef* conc = concretes[k];
            if (!pat || !conc) continue;
            // Bare generic-param pattern (`world: W`): the whole parameter type
            // IS the type param, so bind it to the concrete argument type. Modes
            // (view/mod) are parameter modes, not part of the type argument.
            if (str_eq(get_base_type_name(pat), gp->text)) {
                AstTypeRef* bare = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                *bare = *conc; bare->next = NULL;
                bare->is_view = false; bare->is_mod = false; bare->is_own = false;
                bare->is_copy = false; bare->is_val = false; bare->is_opt = false;
                binding[gi] = bare; break;
            }
            // Base names must align so the generic_args match positionally
            // (e.g. pattern ComponentTable(A) vs concrete ComponentTable(Pos)).
            if (!str_eq(get_base_type_name(pat), get_base_type_name(conc))) continue;
            const AstTypeRef* p_arg = pat->generic_args;
            const AstTypeRef* r_arg = conc->generic_args;
            while (p_arg && r_arg) {
                if (str_eq(get_base_type_name(p_arg), gp->text)) { binding[gi] = r_arg; break; }
                p_arg = p_arg->next; r_arg = r_arg->next;
            }
        }
        if (!binding[gi]) return NULL;  // an unbound param -> inference fails
    }
    AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
    for (size_t i = 0; i < gcount; i++) {
        AstTypeRef* copy = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
        *copy = *binding[i]; copy->next = NULL;
        if (!head) head = copy; else tail->next = copy;
        tail = copy;
    }
    return head;
}

// Is this call argument a bare TYPE name (a primitive like `Float`, or a
// user/enum type), i.e. a positional type argument filling a `T: type`
// generic parameter rather than a value? Used to tell `createList(Float)`
// (type arg only, value param `cap` missing) from `createList(Float, cap: 0)`.
static bool sema_arg_is_type_name(SymbolTable* symbols, const AstExpr* v) {
    if (!v || v->kind != AST_EXPR_IDENT) return false;
    Str n = v->as.ident;
    static const char* const prims[] = {
        "Int","Int64","Int32","Int16","Int8","UInt64","UInt32","UInt16","UInt8",
        "Float","Float32","Float64","Bool","String","Char","Char32","Any","Void","Ptr"};
    for (size_t i = 0; i < sizeof(prims)/sizeof(prims[0]); i++)
        if (str_eq_cstr(n, prims[i])) return true;
    Symbol* s = symbol_table_lookup(symbols, n);
    return s && s->decl && (s->decl->kind == AST_DECL_TYPE || s->decl->kind == AST_DECL_ENUM);
}

static AstDecl* resolve_function_overload(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, Str name, AstCallArg* args, AstTypeRef* explicit_generic_args, size_t line, size_t column) {
    size_t arg_count = 0;
    for (AstCallArg* a = args; a; a = a->next) arg_count++;

    // Project folder-namespace ambiguity. Every project file is auto-open (one
    // visible tree), so a bare name that is defined in TWO different project
    // folders would otherwise resolve silently to whichever the symbol table
    // lists first. Reject it and tell the programmer to qualify the call with
    // the folder name (e.g. `enemies.tick()`). A name unique across the project
    // resolves normally, even from a subfolder. (docs/module-namespacing.md)
    {
        char spaces[8][256]; size_t space_count = 0;
        for (Symbol* curr = symbols->head; curr; curr = curr->next) {
            if (!str_eq(curr->name, name)) continue;
            if (!curr->decl || curr->decl->kind != AST_DECL_FUNC) continue;
            if (curr->decl->as.func_decl.specialization_args) continue;
            if (!sema_decl_opened(s_current_decl_origin, curr->decl)) continue;
            size_t pc = 0;
            for (AstParam* p = curr->decl->as.func_decl.params; p; p = p->next) pc++;
            if (pc != arg_count) continue;
            if (!curr->decl->origin_file) continue;  // synthesized/prelude — not a project folder
            char pkg[256]; sema_package_token(curr->decl->origin_file, pkg, sizeof pkg);
            if (pkg[0] != '\0') continue;            // a lib decl — governed by import/open, not this rule
            char ns[256]; sema_project_namespace(curr->decl, ns, sizeof ns);
            bool seen = false;
            for (size_t i = 0; i < space_count; i++) if (strcmp(spaces[i], ns) == 0) { seen = true; break; }
            if (!seen && space_count < 8) { snprintf(spaces[space_count], 256, "%s", ns); space_count++; }
        }
        if (space_count >= 2) {
            char list[400]; size_t pos = 0;
            for (size_t i = 0; i < space_count; i++) {
                const char* label = spaces[i][0] ? spaces[i] : "the project root";
                int written = snprintf(list + pos, pos < sizeof list ? sizeof list - pos : 0,
                                       "%s%s", i ? ", " : "", label);
                if (written > 0) pos += (size_t)written;
            }
            const char* qual = NULL;
            for (size_t i = 0; i < space_count; i++) if (spaces[i][0]) { qual = spaces[i]; break; }
            char buf[600];
            if (qual)
                snprintf(buf, sizeof buf,
                    "'%.*s' is defined in multiple project folders (%s); qualify the call with the folder name — e.g. `%s.%.*s(...)`",
                    (int)name.len, name.data, list, qual, (int)name.len, name.data);
            else
                snprintf(buf, sizeof buf,
                    "'%.*s' is defined in multiple project folders (%s); qualify the call with the folder name",
                    (int)name.len, name.data, list);
            diag_error(s_current_decl_origin ? s_current_decl_origin : module->file_path,
                       (int)line, (int)column, buf);
            if (module) module->had_error = true;
            return NULL;
        }
    }

    Symbol* best_sym = NULL;
    AstTypeRef* best_inferred = NULL;
    const AstDecl* ineligible = NULL;  // name matched but its package isn't open here
    // A single name+arity candidate whose ONLY problem is a numeric-type
    // mismatch (e.g. Float passed where Float64 is wanted) is still the callee
    // the programmer meant — remember it so we can bind to it and let the
    // caller's ensure_type_match report the precise conversion error, rather
    // than returning NULL and letting the C backend re-resolve by name with the
    // wrong numeric type (#410).
    AstDecl* numeric_mismatch_only = NULL;
    int nongeneric_arity_matches = 0;
    // A generic call that fills a value-parameter slot with a positional TYPE
    // argument and so leaves value parameters unprovided (e.g. `createList(Float)`
    // — `Float` is the type arg, `cap` is missing). Remember it so we can report
    // a clean "missing argument" error instead of letting the backend emit a
    // bare, undeclared `rae_<name>()` that only fails in gcc (#415).
    bool generic_value_arity_issue = false;
    size_t generic_expected_values = 0, generic_got_values = 0;

    for (Symbol* curr = symbols->head; curr; curr = curr->next) {
        if (!str_eq(curr->name, name)) continue;
        if (!curr->decl || curr->decl->kind != AST_DECL_FUNC) continue;
        if (!sema_decl_opened(s_current_decl_origin, curr->decl)) { if (!ineligible) ineligible = curr->decl; continue; }

        AstFuncDecl* fd = &curr->decl->as.func_decl;
        size_t param_count = 0;
        for (AstParam* p = fd->params; p; p = p->next) param_count++;

        if (param_count != arg_count) continue;

        if (fd->generic_params) {
            // This generic function has value parameters; make sure the call
            // actually provides value arguments for them and didn't spend a
            // slot on a positional type argument (#415).
            size_t value_args = 0;
            for (AstCallArg* va = args; va; va = va->next)
                if (!sema_arg_is_type_name(symbols, va->value)) value_args++;
            if (param_count > 0 && value_args < param_count) {
                if (!generic_value_arity_issue) {
                    generic_value_arity_issue = true;
                    generic_expected_values = param_count;
                    generic_got_values = value_args;
                }
                continue;
            }
            AstTypeRef* inferred = explicit_generic_args;
            if (!inferred) {
                // Try to infer generic args from param/arg pairs
                AstParam* p = fd->params;
                AstCallArg* a = args;
                while (p && a && !inferred) {
                    if (a->value && a->value->resolved_type) {
                        TypeInfo* arg_t = a->value->resolved_type;
                        if (arg_t->kind == TYPE_REF) arg_t = arg_t->as.ref.base;
                        if (arg_t->kind == TYPE_STRUCT || arg_t->kind == TYPE_BUFFER) {
                            AstTypeRef rec_tr = { .parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart)) };
                            rec_tr.parts->text = (arg_t->kind == TYPE_BUFFER) ? str_from_cstr("Buffer") : arg_t->name;
                            
                            size_t g_count = (arg_t->kind == TYPE_BUFFER) ? 1 : arg_t->as.structure.generic_count;
                            TypeInfo** g_args = (arg_t->kind == TYPE_BUFFER) ? &arg_t->as.buffer.base : arg_t->as.structure.generic_args;

                            if (g_count > 0) {
                                AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
                                for (size_t i = 0; i < g_count; i++) {
                                    AstTypeRef* ga = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                                    ga->resolved_type = g_args[i]; ga->next = NULL;
                                    ga->parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
                                    ga->parts->text = g_args[i]->name;
                                    if (!head) head = ga; else tail->next = ga; tail = ga;
                                }
                                rec_tr.generic_args = head;
                            }
                            inferred = infer_generic_args(ctx, fd, p->type, &rec_tr);
                        }
                    }
                    p = p->next; a = a->next;
                }
            }

            if (inferred || !fd->generic_params) { // inferred or no generics
                if (!best_sym) {
                    best_sym = curr;
                    best_inferred = inferred;
                }
            }
        } else {
            // Non-generic: check parameter types if possible
            bool mismatch = false;
            bool numeric_mismatch = false; // the mismatch is only a numeric-kind difference
            AstParam* p = fd->params;
            AstCallArg* a = args;
            while (p && a) {
                TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type);
                TypeInfo* at = a->value->resolved_type;
                if (pt && at) {
                    // Unwrap references for comparison
                    TypeInfo* pt_base = pt; while (pt_base->kind == TYPE_REF) pt_base = pt_base->as.ref.base;
                    TypeInfo* at_base = at; while (at_base->kind == TYPE_REF) at_base = at_base->as.ref.base;

                    // Very basic type check: allow Any to match anything, and matching kinds
                    if (pt_base->kind != TYPE_ANY && at_base->kind != TYPE_ANY) {
                        if (pt_base->kind != at_base->kind) {
                            mismatch = true;
                            numeric_mismatch = sema_is_numeric_kind(pt_base->kind)
                                            && sema_is_numeric_kind(at_base->kind);
                            break;
                        }
                        if (pt_base->kind == TYPE_STRUCT && !str_eq(pt_base->name, at_base->name)) { mismatch = true; break; }
                    }
                }
                p = p->next; a = a->next;
            }
            if (!mismatch) return curr->decl;
            nongeneric_arity_matches++;
            if (numeric_mismatch && !numeric_mismatch_only) numeric_mismatch_only = curr->decl;
        }
    }

    // Only a numeric type stands between the call and its sole candidate: bind
    // to it so the argument-conversion check (ensure_type_match) fires with the
    // precise "cannot implicitly convert" diagnostic. Guarded to a SINGLE
    // arity candidate so genuine overload sets still fail to resolve. (#410)
    if (!best_sym && nongeneric_arity_matches == 1 && numeric_mismatch_only)
        return numeric_mismatch_only;

    if (best_sym) {
        if (best_inferred) {
            TypeInfo* type_args[16]; size_t ac = 0;
            for (AstTypeRef* tr = best_inferred; tr && ac < 16; tr = tr->next) type_args[ac++] = sema_resolve_type_internal(ctx, module, symbols, tr);
            AstDecl* spec = type_registry_find_specialization(ctx->type_registry, best_sym->decl, type_args, ac);
            if (!spec) spec = specialize_decl(ctx, module, symbols, best_sym->decl, type_args, ac, line, column);
            return spec;
        }
        return best_sym->decl;
    }

    // The name only matched a function in a package that isn't open here. Diagnose
    // so it fails (rather than the C backend silently re-resolving by name).
    if (ineligible && ineligible->module_name) {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "'%.*s' is in package '%s', which is not open here; use `open %s` for a bare call, or %s.%.*s(...)",
            (int)name.len, name.data, ineligible->module_name,
            ineligible->module_name, ineligible->module_name, (int)name.len, name.data);
        const char* err_file = s_current_decl_origin ? s_current_decl_origin : module->file_path;
        diag_error(err_file, (int)line, (int)column, buf);
        if (module) module->had_error = true;
    } else if (generic_value_arity_issue) {
        // The only same-name candidate is a generic function whose value
        // parameters weren't all supplied — a positional type argument filled a
        // value slot (#415: `createList(Float)` gives the type but omits `cap`).
        char buf[256];
        snprintf(buf, sizeof(buf),
            "call to '%.*s' is missing value arguments: it needs %zu after its type argument%s, but %zu %s given",
            (int)name.len, name.data, generic_expected_values,
            generic_expected_values == 1 ? "" : "s",
            generic_got_values, generic_got_values == 1 ? "was" : "were");
        const char* err_file = s_current_decl_origin ? s_current_decl_origin : module->file_path;
        diag_error(err_file, (int)line, (int)column, buf);
        if (module) module->had_error = true;
    }
    return NULL;
}

// Resolve a namespace-qualified call `qualifier.name(args)` to a function decl
// in module `qualifier`. Searches module->decls directly (not the symbol table),
// so it keeps working once non-core stdlib stops being flat-registered. Matches
// by module_name + name + arity, preferring an exact (non-Any) type match and
// falling back to the first arity match. Non-generic only — namespaced stdlib
// functions are non-generic; generic stays in flat-visible `core`.
// (docs/module-namespacing.md)
static AstDecl* resolve_qualified_function(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                                           Str qualifier, Str name, AstCallArg* args) {
    size_t arg_count = 0;
    for (AstCallArg* a = args; a; a = a->next) arg_count++;
    AstDecl* arity_match = NULL;
    for (AstDecl* d = module->decls; d; d = d->next) {
        if (d->kind != AST_DECL_FUNC) continue;
        // Match either the full module path (`ui/ecs` — the lib/qualified form)
        // or a project folder namespace (`enemies` for `enemies/tick`). The
        // latter lets project code qualify a colliding name by its folder
        // without any `import`/`open`. (docs/module-namespacing.md)
        bool qual_match = d->module_name && str_eq_cstr(qualifier, d->module_name);
        if (!qual_match) {
            char ns[256]; sema_project_namespace(d, ns, sizeof ns);
            qual_match = ns[0] != '\0' && str_eq_cstr(qualifier, ns);
        }
        if (!qual_match) continue;
        if (!str_eq(d->as.func_decl.name, name)) continue;
        if (d->as.func_decl.specialization_args) continue;
        if (!sema_decl_visible(s_current_decl_origin, d)) continue;  // module must be imported/opened/same-pkg here
        AstFuncDecl* fd = &d->as.func_decl;
        size_t param_count = 0;
        for (AstParam* p = fd->params; p; p = p->next) param_count++;
        if (param_count != arg_count) continue;
        if (!arity_match) arity_match = d;
        bool mismatch = false;
        AstParam* p = fd->params;
        AstCallArg* a = args;
        while (p && a) {
            TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type);
            TypeInfo* at = a->value ? a->value->resolved_type : NULL;
            if (pt && at) {
                TypeInfo* pb = pt; while (pb->kind == TYPE_REF) pb = pb->as.ref.base;
                TypeInfo* ab = at; while (ab->kind == TYPE_REF) ab = ab->as.ref.base;
                if (pb->kind != TYPE_ANY && ab->kind != TYPE_ANY) {
                    if (pb->kind != ab->kind) { mismatch = true; break; }
                    if (pb->kind == TYPE_STRUCT && !str_eq(pb->name, ab->name)) { mismatch = true; break; }
                }
            }
            p = p->next; a = a->next;
        }
        if (!mismatch) return d;
    }
    return arity_match;
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
            // Stage 6 (Memory-safety overhaul): bare parameter types
            // are a hard compile error for every non-extern function.
            // Every parameter must use an explicit ownership mode:
            //   view T  — read-only borrow (callee may not mutate)
            //   mod T   — mutating borrow (callee may mutate in place)
            //   copy T  — independent deep copy (callee owns its copy)
            //   own T   — takes ownership (caller's binding is moved)
            //
            // The legacy `val T` syntax is also rejected here with a
            // suggestion to use `copy T`.
            //
            // Externs are skipped: their parameter lists describe the
            // FFI/ABI shape, not Rae's ownership model.
            //
            // `Any` parameters are also allowed bare: Any is a tagged
            // union value type (not an owning heap thing on its own)
            // and is conventionally passed by value at the source
            // level. The four modes carry no extra information for
            // Any, so requiring one would just be noise.
            //
            // For primitives (Int/Float/Bool/Char/...) the backend is
            // free to lower view/copy/mod/own to identical pass-by-
            // value machine code — but the source-level annotation
            // still expresses intent (read-only vs fresh-copy vs
            // mutable borrow vs consume), which matters when a
            // function later changes to a non-primitive type.
            if (!decl->as.func_decl.is_extern) {
                // After merge + generic specialization, module->file_path
                // is whichever file was merged last — not the file the
                // decl actually came from. Prefer decl->origin_file when
                // available so the diagnostic points at the right source.
                const char* err_file = decl->origin_file
                    ? decl->origin_file
                    : (module && module->file_path ? module->file_path : "<unknown>");
                for (AstParam* p2 = decl->as.func_decl.params; p2; p2 = p2->next) {
                    if (!p2->type) continue;
                    AstTypeRef* pt = p2->type;
                    if (pt->is_val) {
                        Str base = (pt->parts) ? pt->parts->text : (Str){0};
                        char buffer[256];
                        snprintf(buffer, sizeof(buffer),
                            "parameter '%.*s' uses deprecated 'val %.*s'; use 'copy %.*s' instead",
                            (int)p2->name.len, p2->name.data,
                            (int)base.len, base.data,
                            (int)base.len, base.data);
                        diag_error(err_file, (int)pt->line, (int)pt->column, buffer);
                        module->had_error = true;
                        continue;
                    }
                    bool has_mode = pt->is_view || pt->is_mod || pt->is_own || pt->is_copy;
                    if (has_mode) continue;
                    // Any and enum types are bare-allowed: they're
                    // value-types (tagged union / int tag) with no
                    // heap ownership of their own, so the four modes
                    // don't carry useful extra information. This
                    // matches Stage 5's behaviour and keeps enum-only
                    // signatures (e.g. `alignMain: AlignKind`) clean.
                    Str base = (pt->parts) ? pt->parts->text : (Str){0};
                    if (str_eq_cstr(base, "Any")) continue;
                    TypeInfo* resolved = sema_resolve_type_internal(ctx, module, symbols, pt);
                    if (resolved && resolved->kind == TYPE_ANY) continue;
                    {
                        Symbol* type_sym = symbol_table_lookup(symbols, base);
                        if (type_sym && type_sym->decl && type_sym->decl->kind == AST_DECL_ENUM) {
                            continue;
                        }
                    }
                    char buffer[256];
                    snprintf(buffer, sizeof(buffer),
                        "parameter '%.*s' must specify view/copy/mod/own; bare parameter types are not allowed",
                        (int)p2->name.len, p2->name.data);
                    diag_error(err_file, (int)pt->line, (int)pt->column, buffer);
                    module->had_error = true;
                }
            }
            AstParam* param = decl->as.func_decl.params;
            while (param) {
                TypeInfo* t = sema_resolve_type_internal(ctx, module, symbols, param->type);
                // Only struct params without 'mod' are immutable (view by default)
                // Primitives (Int, Float, Bool, String, Char) are value-copy params — always mutable
                bool is_view_param = false;
                if (param->type && !param->type->is_mod && !param->type->is_view) {
                    // Check if it's a struct type (not primitive)
                    if (t && t->kind == TYPE_STRUCT) is_view_param = true;
                } else if (param->type && param->type->is_view) {
                    is_view_param = true;
                }
                Symbol* psym = symbol_table_define(symbols, ctx->ast_arena, param->name, NULL, t, is_view_param);
                /* A borrow is someone else's value: the callee may read or
                 * mutate it but never free it, so it can't satisfy `own`. */
                if (psym && param->type && (param->type->is_view || param->type->is_mod)) {
                    psym->is_non_owning = true;
                }
                param = param->next;
            }
            if (decl->as.func_decl.body) {
                const char* saved_origin = s_current_decl_origin;
                s_current_decl_origin = decl->origin_file;
                AstStmt* stmt = decl->as.func_decl.body->first;
                while (stmt) { sema_analyze_stmt(ctx, module, symbols, stmt, current_return_type); stmt = stmt->next; }
                s_current_decl_origin = saved_origin;
            }
            symbol_table_pop_scope(symbols);
            break;
        }
        case AST_DECL_GLOBAL_LET: {
            if (decl->as.let_decl.value) sema_analyze_expr(ctx, module, symbols, decl->as.let_decl.value);
            // Mark the already-registered global symbol's mutability, and fold a
            // `const` initializer to a literal (decls are analyzed in source
            // order, so a const may reference earlier consts).
            Symbol* gs = symbol_table_lookup(symbols, decl->as.let_decl.name);
            if (gs) {
                bool immut = !decl->as.let_decl.is_var;
                gs->is_immutable = immut;
                /* A global outlives every callee — handing one to `own`
                 * frees storage the rest of the program still refers to.
                 * This is the #222 shape.
                 *
                 * EXCEPT a global bound to a string literal, which is the
                 * overwhelmingly common `let actionId: String = "x"`
                 * constant. A literal's buffer is static: rae_string_drop
                 * is guarded on `is_owned`, so a callee "freeing" it is a
                 * no-op and there is nothing to dangle. Marking those
                 * non-owning produced six false positives in
                 * 106_mobile_ui alone — a rule that fires on the
                 * idiomatic spelling of a constant would just be
                 * turned off. */
                gs->is_non_owning = !(decl->as.let_decl.value
                                      && decl->as.let_decl.value->kind == AST_EXPR_STRING);
                if (decl->as.let_decl.is_const) {
                    gs->bind_kind = BIND_CONST;
                    sema_fold_const(ctx, module, symbols, decl->as.let_decl.value, gs, decl->line, decl->column);
                } else {
                    gs->bind_kind = immut ? BIND_LET : BIND_MUTABLE;
                }
            }
            /* #763 no-globals rule (docs/globals-and-app-ownership.md), WARNING
             * phase: a module-level `var` is mutable global state; a module-level
             * `let` that owns heap (String/List/Map/struct-with-heap) is global
             * heap with no owner. Both warn (not error — the #731-#750 ECS
             * refactor lands under the rule). `const` and POD-literal `let` (Int/
             * Float/Bool/plain struct, no heap) are fine. */
            if (!decl->as.let_decl.is_const) {
                const char* why = NULL;
                /* A `let` initialised from a STRING LITERAL is a compile-time
                 * constant with a STATIC buffer (is_owned=false) — it owns no
                 * runtime heap and has no lifetime, so it is fine (it should just
                 * be spelled `const`; #765). Only a `let` whose initialiser
                 * ALLOCATES (createList/a heap-returning call) is a real global. */
                bool literal_init = decl->as.let_decl.value
                                    && decl->as.let_decl.value->kind == AST_EXPR_STRING;
                const AstTypeRef* gt = decl->as.let_decl.type;
                bool owns_heap = gt
                    && (type_owns_heap_storage(ctx, module, gt, 0)
                        /* String owns a heap buffer too, but ownership.c leaves it
                         * out; count it here (a literal init is exempted above). */
                        || (!gt->is_view && !gt->is_mod
                            && str_eq_cstr(get_base_type_name(gt), "String")));
                if (decl->as.let_decl.is_var) {
                    why = "module-level `var` is mutable global state";
                } else if (owns_heap && !literal_init) {
                    why = "module-level `let` owns heap (a global with no owner)";
                }
                if (why) {
                    char nog[320];
                    snprintf(nog, sizeof(nog),
                        "%s: '%.*s' — no globals: make it a component, a resource on the "
                        "App/World, or a `const`. See docs/globals-and-app-ownership.md",
                        why, (int)decl->as.let_decl.name.len, decl->as.let_decl.name.data);
                    diag_warn(module->file_path, (int)decl->line, (int)decl->column, nog);
                }
            }
            break;
        }
        case AST_DECL_ALIAS:
            // Resolve the aliased target once so a bad target (`alias X = Nope`)
            // is diagnosed at the alias site and the TypeInfo is cached.
            if (decl->as.alias_decl.target)
                sema_resolve_type_internal(ctx, module, symbols, decl->as.alias_decl.target);
            break;
        default: break;
    }
}

// ---- Compile-time constant evaluation (for `const`) ----
typedef struct {
    bool ok;          // is this a valid compile-time constant expression?
    bool numeric;     // ok AND a number we can fold (Int/Float). Non-numeric
                      // consts (enum case, string, bool) are valid but left as-is.
    bool is_float;
    double d;
    long long i;
} ConstResult;

static double cr_num(ConstResult r) { return r.is_float ? r.d : (double)r.i; }

static ConstResult const_eval(SymbolTable* symbols, AstExpr* e) {
    ConstResult fail = {0};
    ConstResult ok_nonnum = { .ok = true };
    if (!e) return fail;
    switch (e->kind) {
        case AST_EXPR_INTEGER: {
            char buf[64]; size_t n = e->as.integer.len < 63 ? e->as.integer.len : 63;
            memcpy(buf, e->as.integer.data, n); buf[n] = '\0';
            return (ConstResult){ .ok = true, .numeric = true, .is_float = false, .i = strtoll(buf, NULL, 0) };
        }
        case AST_EXPR_FLOAT: {
            char buf[64]; size_t n = e->as.floating.len < 63 ? e->as.floating.len : 63;
            memcpy(buf, e->as.floating.data, n); buf[n] = '\0';
            return (ConstResult){ .ok = true, .numeric = true, .is_float = true, .d = strtod(buf, NULL) };
        }
        case AST_EXPR_BOOL:
        case AST_EXPR_STRING:
            return ok_nonnum;  // valid const, not foldable arithmetic
        case AST_EXPR_MEMBER:
            // Enum case (e.g. RenderMode.pathTraced) — a compile-time value the
            // backends already lower to a constant. Accept without folding.
            return ok_nonnum;
        case AST_EXPR_IDENT: {
            Symbol* s = symbol_table_lookup(symbols, e->as.ident);
            if (!s || s->bind_kind != BIND_CONST) return fail;
            if (s->const_is_number) {
                return (ConstResult){ .ok = true, .numeric = true, .is_float = s->const_is_float, .d = s->const_d, .i = s->const_i };
            }
            return ok_nonnum;
        }
        case AST_EXPR_UNARY: {
            if (e->as.unary.op != AST_UNARY_NEG) return fail;
            ConstResult r = const_eval(symbols, e->as.unary.operand);
            if (!r.ok || !r.numeric) return fail;
            if (r.is_float) return (ConstResult){ .ok=true, .numeric=true, .is_float=true, .d = -r.d };
            return (ConstResult){ .ok=true, .numeric=true, .is_float=false, .i = -r.i };
        }
        case AST_EXPR_BINARY: {
            ConstResult l = const_eval(symbols, e->as.binary.lhs);
            ConstResult r = const_eval(symbols, e->as.binary.rhs);
            if (!l.ok || !r.ok || !l.numeric || !r.numeric) return fail;
            bool isf = l.is_float || r.is_float;
            switch (e->as.binary.op) {
                case AST_BIN_ADD: return isf ? (ConstResult){.ok=1,.numeric=1,.is_float=1,.d=cr_num(l)+cr_num(r)} : (ConstResult){.ok=1,.numeric=1,.i=l.i+r.i};
                case AST_BIN_SUB: return isf ? (ConstResult){.ok=1,.numeric=1,.is_float=1,.d=cr_num(l)-cr_num(r)} : (ConstResult){.ok=1,.numeric=1,.i=l.i-r.i};
                case AST_BIN_MUL: return isf ? (ConstResult){.ok=1,.numeric=1,.is_float=1,.d=cr_num(l)*cr_num(r)} : (ConstResult){.ok=1,.numeric=1,.i=l.i*r.i};
                case AST_BIN_DIV:
                    if (isf) { if (cr_num(r) == 0.0) return fail; return (ConstResult){.ok=1,.numeric=1,.is_float=1,.d=cr_num(l)/cr_num(r)}; }
                    if (r.i == 0) return fail; return (ConstResult){.ok=1,.numeric=1,.i=l.i/r.i};
                case AST_BIN_MOD:
                    if (isf || r.i == 0) return fail; return (ConstResult){.ok=1,.numeric=1,.i=l.i % r.i};
                // Bitwise ops fold on integer operands only (Int-only by design).
                case AST_BIN_BITAND: if (isf) return fail; return (ConstResult){.ok=1,.numeric=1,.i=l.i & r.i};
                case AST_BIN_BITOR:  if (isf) return fail; return (ConstResult){.ok=1,.numeric=1,.i=l.i | r.i};
                case AST_BIN_BITXOR: if (isf) return fail; return (ConstResult){.ok=1,.numeric=1,.i=l.i ^ r.i};
                case AST_BIN_SHL:  if (isf) return fail; return (ConstResult){.ok=1,.numeric=1,.i=l.i << r.i};
                case AST_BIN_SHR:  if (isf) return fail; return (ConstResult){.ok=1,.numeric=1,.i=l.i >> r.i};
                default: return fail;
            }
        }
        default:
            return fail;
    }
}

// Evaluate a `const` initializer; on success record the folded value on `sym`
// and rewrite a numeric initializer into a literal node (so the backends emit a
// plain literal — required because C `static const` initializers must be
// constant expressions). On failure emit a clear diagnostic.
static void sema_fold_const(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                            AstExpr* init, Symbol* sym, size_t line, size_t col) {
    if (!init) return;
    ConstResult r = const_eval(symbols, init);
    if (!r.ok) {
        diag_error(module->file_path, (int)line, (int)col,
                   "'const' initializer must be evaluable at compile time (only literals, earlier constants, enum cases, and arithmetic on them are allowed)");
        module->had_error = true;
        return;
    }
    if (sym) {
        sym->const_is_number = r.numeric;
        sym->const_is_float = r.is_float;
        sym->const_d = r.d;
        sym->const_i = r.i;
    }
    if (r.numeric) {
        char buf[64];
        if (r.is_float) {
            snprintf(buf, sizeof buf, "%.17g", r.d);
            // ensure it reads as a float literal
            if (!strpbrk(buf, ".eEnN")) { size_t l = strlen(buf); snprintf(buf + l, sizeof(buf) - l, ".0"); }
            init->kind = AST_EXPR_FLOAT;
            init->as.floating = str_dup_arena(ctx->ast_arena, str_from_cstr(buf));
        } else {
            snprintf(buf, sizeof buf, "%lld", r.i);
            init->kind = AST_EXPR_INTEGER;
            init->as.integer = str_dup_arena(ctx->ast_arena, str_from_cstr(buf));
        }
    }
}

/* Is this struct type an instantiation of the named generic?
 *
 * A specialized `List(String)` does NOT carry the name "List", so comparing
 * `t->name` silently never matches — which is why the List branch in the
 * index handler below was dead code for as long as it existed. Ask the
 * generic template for its name instead. */
static bool sema_struct_template_is(const TypeInfo* t, const char* name) {
    if (!t || t->kind != TYPE_STRUCT) return false;
    Str tn = t->name;
    if (t->as.structure.decl && t->as.structure.decl->kind == AST_DECL_TYPE) {
        AstDecl* tmpl = t->as.structure.decl->as.type_decl.generic_template;
        tn = (tmpl && tmpl->kind == AST_DECL_TYPE)
             ? tmpl->as.type_decl.name
             : t->as.structure.decl->as.type_decl.name;
    }
    return str_eq_cstr(tn, name);
}

/* Does this expression produce a fresh temporary — a value with no storage
 * of its own that outlives the enclosing statement? */
static bool sema_expr_is_temporary(const AstExpr* e) {
    if (!e) return false;
    switch (e->kind) {
        case AST_EXPR_OBJECT:
        case AST_EXPR_COLLECTION_LITERAL:
            return true;
        default:
            return false;
    }
}

/* `ret view x` / `ret mod x`: the caller receives a reference, so whatever it
 * points at must outlive this frame. Parameters and globals do; a `let`/`var`
 * in this body does not, and neither does a temporary produced right here.
 *
 * This is a semantic rule, not a codegen concern, so it lives in sema and
 * therefore fires for every target. It used to exist only in the VM compiler,
 * which meant the Compiled path silently emitted a dangling pointer (or, for
 * a returned value, C that would not even build). */
static void sema_check_returned_ref(CompilerContext* ctx, AstModule* module,
                                    SymbolTable* symbols, AstExpr* value) {
    (void)ctx;
    if (!value || value->kind != AST_EXPR_UNARY) return;
    if (value->as.unary.op != AST_UNARY_VIEW && value->as.unary.op != AST_UNARY_MOD) return;

    const AstExpr* base = value->as.unary.operand;
    while (base && (base->kind == AST_EXPR_MEMBER || base->kind == AST_EXPR_INDEX)) {
        base = (base->kind == AST_EXPR_MEMBER) ? base->as.member.object
                                               : base->as.index.target;
    }
    if (!base) return;

    const char* file = module ? module->file_path : NULL;
    if (base->kind == AST_EXPR_IDENT) {
        Symbol* sym = symbol_table_lookup(symbols, base->as.ident);
        if (sym && sym->is_local_storage) {
            diag_report(file, (int)value->line, (int)value->column,
                        "reference escapes local storage");
            if (module) module->had_error = true;
        }
        return;
    }
    if (sema_expr_is_temporary(base)) {
        diag_report(file, (int)value->line, (int)value->column,
                    "cannot take reference to a temporary literal");
        if (module) module->had_error = true;
        return;
    }
    /* A reference to a CALL's result is decided in the C backend, not
     * here: the distinguishing fact is whether the callee lowers to an
     * lvalue, and sema does not resolve UFCS method calls or intrinsics
     * yet. See the is_ref_return arm of emit_return in c_stmt.c. */
}

static bool sema_same_place_expr(const AstExpr* left, const AstExpr* right) {
    if (!left || !right || left->kind != right->kind) return false;
    if (left->kind == AST_EXPR_IDENT) return str_eq(left->as.ident, right->as.ident);
    if (left->kind == AST_EXPR_MEMBER) {
        return str_eq(left->as.member.member, right->as.member.member)
            && sema_same_place_expr(left->as.member.object, right->as.member.object);
    }
    return false;
}

static bool sema_expr_mutates_collection(const AstExpr* expr,
                                         const AstExpr* collection) {
    if (!expr) return false;
    if (expr->kind == AST_EXPR_METHOD_CALL) {
        Str method = expr->as.method_call.method_name;
        bool mutates = str_eq_cstr(method, "add") || str_eq_cstr(method, "set")
            || str_eq_cstr(method, "insert") || str_eq_cstr(method, "remove")
            || str_eq_cstr(method, "swapRemove") || str_eq_cstr(method, "clear")
            || str_eq_cstr(method, "drop") || str_eq_cstr(method, "grow")
            || str_eq_cstr(method, "pop");
        if (mutates && sema_same_place_expr(expr->as.method_call.object, collection)) {
            return true;
        }
        if (sema_expr_mutates_collection(expr->as.method_call.object, collection)) return true;
        for (const AstCallArg* arg = expr->as.method_call.args; arg; arg = arg->next) {
            if (sema_expr_mutates_collection(arg->value, collection)) return true;
        }
        return false;
    }
    switch (expr->kind) {
        case AST_EXPR_BINARY:
            return sema_expr_mutates_collection(expr->as.binary.lhs, collection)
                || sema_expr_mutates_collection(expr->as.binary.rhs, collection);
        case AST_EXPR_UNARY:
        case AST_EXPR_BOX:
        case AST_EXPR_UNBOX:
            return sema_expr_mutates_collection(expr->as.unary.operand, collection);
        case AST_EXPR_CAST:
            return sema_expr_mutates_collection(expr->as.cast.operand, collection);
        case AST_EXPR_MEMBER:
            return sema_expr_mutates_collection(expr->as.member.object, collection);
        case AST_EXPR_INDEX:
            return sema_expr_mutates_collection(expr->as.index.target, collection)
                || sema_expr_mutates_collection(expr->as.index.index, collection);
        case AST_EXPR_CALL:
            if (sema_expr_mutates_collection(expr->as.call.callee, collection)) return true;
            for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) {
                if (sema_expr_mutates_collection(arg->value, collection)) return true;
            }
            return false;
        default:
            return false;
    }
}

static bool sema_stmt_mutates_collection(const AstStmt* stmt,
                                         const AstExpr* collection) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case AST_STMT_EXPR:
            return sema_expr_mutates_collection(stmt->as.expr_stmt, collection);
        case AST_STMT_LET:
            return sema_expr_mutates_collection(stmt->as.let_stmt.value, collection);
        case AST_STMT_ASSIGN:
            return sema_same_place_expr(stmt->as.assign_stmt.target, collection)
                || sema_expr_mutates_collection(stmt->as.assign_stmt.value, collection);
        case AST_STMT_IF:
            if (stmt->as.if_stmt.binding
                && sema_stmt_mutates_collection(stmt->as.if_stmt.binding, collection)) return true;
            if (sema_expr_mutates_collection(stmt->as.if_stmt.condition, collection)) return true;
            for (const AstStmt* branch = stmt->as.if_stmt.then_block
                     ? stmt->as.if_stmt.then_block->first : NULL;
                 branch; branch = branch->next) {
                if (sema_stmt_mutates_collection(branch, collection)) return true;
            }
            for (const AstStmt* branch = stmt->as.if_stmt.else_block
                     ? stmt->as.if_stmt.else_block->first : NULL;
                 branch; branch = branch->next) {
                if (sema_stmt_mutates_collection(branch, collection)) return true;
            }
            return false;
        case AST_STMT_LOOP:
            if (sema_expr_mutates_collection(stmt->as.loop_stmt.condition, collection)) return true;
            for (const AstStmt* body = stmt->as.loop_stmt.body
                     ? stmt->as.loop_stmt.body->first : NULL;
                 body; body = body->next) {
                if (sema_stmt_mutates_collection(body, collection)) return true;
            }
            return false;
        default:
            return false;
    }
}

// Does a match-case pattern expr name the enum member `enum_name.member`?
// Used for exhaustiveness, checking each pattern in an or-pattern arm.
static bool sema_pattern_names_member(const AstExpr* pat, Str enum_name, Str member) {
    return pat && pat->kind == AST_EXPR_MEMBER
        && pat->as.member.object->kind == AST_EXPR_IDENT
        && str_eq(pat->as.member.object->as.ident, enum_name)
        && str_eq(pat->as.member.member, member);
}

/* ===== Compile-time field reflection (#772) =================================
 *
 * `loop let x: <mode> T in fields(value) { body }` iterates the FIELDS of a
 * struct value at compile time. It is not a runtime loop: sema unrolls it into
 * one scoped copy of `body` per matching field, with `x` bound as an alias to
 * `value.<field>`, and folds `fieldName(x)` to that field's name as a String
 * literal. The unrolled form is ordinary AST, so BOTH backends emit it with no
 * special case (docs/compile-time-reflection.md).
 *
 * The binding's explicit type is the FILTER: `ComponentTable(any)` matches every
 * ComponentTable field regardless of element type; a concrete `ComponentTable(T)`
 * matches only that one; `any` alone matches every field. `any` is the wildcard.
 */

// Last identifier part's text — the base type name ("ComponentTable", "any").
static Str reflect_base_name(const AstTypeRef* tr) {
    Str empty = { .data = NULL, .len = 0 };
    if (!tr || !tr->parts) return empty;
    const AstIdentifierPart* p = tr->parts;
    while (p->next) p = p->next;
    return p->text;
}

// The wildcard is exactly `any` with no generic arguments.
static bool reflect_is_wildcard(const AstTypeRef* tr) {
    return tr && !tr->generic_args && str_eq_cstr(reflect_base_name(tr), "any");
}

// Structural match of a binding-type PATTERN against a concrete FIELD type,
// ignoring reference modes (view/mod/opt) on both. `any` in the pattern (at any
// depth) is a wildcard. Names must otherwise be equal and generic arguments
// must match pairwise.
static bool reflect_type_matches(const AstTypeRef* pattern, const AstTypeRef* field) {
    if (!pattern || !field) return false;
    if (reflect_is_wildcard(pattern)) return true;
    if (!str_eq(reflect_base_name(pattern), reflect_base_name(field))) return false;
    const AstTypeRef* pa = pattern->generic_args;
    const AstTypeRef* fa = field->generic_args;
    while (pa && fa) {
        if (pa->is_value_arg || fa->is_value_arg) {
            // Value generic args (`cap: N`) don't occur in ComponentTable
            // patterns; be conservative and require both to be value args of
            // the same name. Numeric equality is out of scope for v1.
            if (!(pa->is_value_arg && fa->is_value_arg && str_eq(pa->value_name, fa->value_name)))
                return false;
        } else if (!reflect_type_matches(pa, fa)) {
            return false;
        }
        pa = pa->next; fa = fa->next;
    }
    return pa == NULL && fa == NULL;
}

// Is `e` the reflection call `fieldName(binding)` or `binding.fieldName()`?
static bool reflect_is_field_name_call(const AstExpr* e, Str binding) {
    if (!e) return false;
    if (e->kind == AST_EXPR_CALL && e->as.call.callee
        && e->as.call.callee->kind == AST_EXPR_IDENT
        && str_eq_cstr(e->as.call.callee->as.ident, "fieldName")
        && e->as.call.args && !e->as.call.args->next
        && e->as.call.args->name.len == 0
        && e->as.call.args->value
        && e->as.call.args->value->kind == AST_EXPR_IDENT
        && str_eq(e->as.call.args->value->as.ident, binding)) {
        return true;
    }
    if (e->kind == AST_EXPR_METHOD_CALL
        && str_eq_cstr(e->as.method_call.method_name, "fieldName")
        && !e->as.method_call.args
        && e->as.method_call.object
        && e->as.method_call.object->kind == AST_EXPR_IDENT
        && str_eq(e->as.method_call.object->as.ident, binding)) {
        return true;
    }
    return false;
}

static void reflect_fold_field_name_block(AstBlock* block, Str binding, Str field_name);

// Rewrite every `fieldName(binding)` inside `e` (in place) to the String literal
// `field_name`, recursing through the whole expression tree.
static void reflect_fold_field_name_expr(AstExpr* e, Str binding, Str field_name) {
    if (!e) return;
    if (reflect_is_field_name_call(e, binding)) {
        e->kind = AST_EXPR_STRING;
        e->as.string_lit = field_name;
        e->resolved_type = NULL;
        e->decl_link = NULL;
        return;
    }
    switch (e->kind) {
        case AST_EXPR_BINARY:
            reflect_fold_field_name_expr(e->as.binary.lhs, binding, field_name);
            reflect_fold_field_name_expr(e->as.binary.rhs, binding, field_name);
            break;
        case AST_EXPR_UNARY:
            reflect_fold_field_name_expr(e->as.unary.operand, binding, field_name);
            break;
        case AST_EXPR_CAST:
            reflect_fold_field_name_expr(e->as.cast.operand, binding, field_name);
            break;
        case AST_EXPR_CALL:
            reflect_fold_field_name_expr(e->as.call.callee, binding, field_name);
            for (AstCallArg* a = e->as.call.args; a; a = a->next)
                reflect_fold_field_name_expr(a->value, binding, field_name);
            break;
        case AST_EXPR_METHOD_CALL:
            reflect_fold_field_name_expr(e->as.method_call.object, binding, field_name);
            for (AstCallArg* a = e->as.method_call.args; a; a = a->next)
                reflect_fold_field_name_expr(a->value, binding, field_name);
            break;
        case AST_EXPR_MEMBER:
            reflect_fold_field_name_expr(e->as.member.object, binding, field_name);
            break;
        case AST_EXPR_INDEX:
            reflect_fold_field_name_expr(e->as.index.target, binding, field_name);
            reflect_fold_field_name_expr(e->as.index.index, binding, field_name);
            break;
        case AST_EXPR_OBJECT:
            for (AstObjectField* f = e->as.object_literal.fields; f; f = f->next)
                reflect_fold_field_name_expr(f->value, binding, field_name);
            break;
        case AST_EXPR_INTERP:
            for (AstInterpPart* p = e->as.interp.parts; p; p = p->next)
                reflect_fold_field_name_expr(p->value, binding, field_name);
            break;
        case AST_EXPR_LIST:
            for (AstExprList* l = e->as.list; l; l = l->next)
                reflect_fold_field_name_expr(l->value, binding, field_name);
            break;
        case AST_EXPR_MATCH:
            reflect_fold_field_name_expr(e->as.match_expr.subject, binding, field_name);
            for (AstMatchArm* arm = e->as.match_expr.arms; arm; arm = arm->next) {
                reflect_fold_field_name_expr(arm->pattern, binding, field_name);
                reflect_fold_field_name_expr(arm->value, binding, field_name);
            }
            break;
        default: break;
    }
}

static void reflect_fold_field_name_stmt(AstStmt* s, Str binding, Str field_name) {
    if (!s) return;
    switch (s->kind) {
        case AST_STMT_EXPR: reflect_fold_field_name_expr(s->as.expr_stmt, binding, field_name); break;
        case AST_STMT_LET: reflect_fold_field_name_expr(s->as.let_stmt.value, binding, field_name); break;
        case AST_STMT_DESTRUCT: reflect_fold_field_name_expr(s->as.destruct_stmt.call, binding, field_name); break;
        case AST_STMT_RET:
            for (AstReturnArg* a = s->as.ret_stmt.values; a; a = a->next)
                reflect_fold_field_name_expr(a->value, binding, field_name);
            break;
        case AST_STMT_IF:
            reflect_fold_field_name_stmt(s->as.if_stmt.binding, binding, field_name);
            reflect_fold_field_name_expr(s->as.if_stmt.condition, binding, field_name);
            reflect_fold_field_name_block(s->as.if_stmt.then_block, binding, field_name);
            reflect_fold_field_name_block(s->as.if_stmt.else_block, binding, field_name);
            break;
        case AST_STMT_LOOP:
            reflect_fold_field_name_stmt(s->as.loop_stmt.init, binding, field_name);
            reflect_fold_field_name_expr(s->as.loop_stmt.condition, binding, field_name);
            reflect_fold_field_name_expr(s->as.loop_stmt.increment, binding, field_name);
            reflect_fold_field_name_block(s->as.loop_stmt.body, binding, field_name);
            break;
        case AST_STMT_MATCH:
            reflect_fold_field_name_expr(s->as.match_stmt.subject, binding, field_name);
            for (AstMatchCase* c = s->as.match_stmt.cases; c; c = c->next) {
                reflect_fold_field_name_expr(c->pattern, binding, field_name);
                for (AstCasePattern* op = c->or_patterns; op; op = op->next)
                    reflect_fold_field_name_expr(op->expr, binding, field_name);
                reflect_fold_field_name_block(c->block, binding, field_name);
            }
            break;
        case AST_STMT_ASSIGN:
            reflect_fold_field_name_expr(s->as.assign_stmt.target, binding, field_name);
            reflect_fold_field_name_expr(s->as.assign_stmt.value, binding, field_name);
            break;
        case AST_STMT_DEFER:
            reflect_fold_field_name_block(s->as.defer_stmt.block, binding, field_name);
            break;
        default: break;
    }
}

static void reflect_fold_field_name_block(AstBlock* block, Str binding, Str field_name) {
    if (!block) return;
    for (AstStmt* s = block->first; s; s = s->next)
        reflect_fold_field_name_stmt(s, binding, field_name);
}

// Does this range-loop iterate `fields(...)` — i.e. is it a field loop at all?
static bool reflect_loop_is_fields(const AstStmt* stmt) {
    const AstExpr* it = stmt->as.loop_stmt.condition;
    return it && it->kind == AST_EXPR_CALL && it->as.call.callee
        && it->as.call.callee->kind == AST_EXPR_IDENT
        && str_eq_cstr(it->as.call.callee->as.ident, "fields");
}

// Fill `out` with one scoped `if true { let x => value.field; body }` per struct
// field matching `pattern`. This is the shared unrolling core used by BOTH the
// non-generic sema path (#772) and the generic-instantiation C-backend path
// (#773), so every field loop lowers to identical ordinary AST regardless of how
// its concrete struct became known.
static AstExpr* reflect_make_true(CompilerContext* ctx, size_t line, size_t column);
static void reflect_fold_field_name_block(AstBlock* block, Str binding, Str field_name);
static void reflect_fill_unrolled(CompilerContext* ctx, AstBlock* out,
                                  AstTypeField* fields, const AstTypeRef* pattern,
                                  bool want_view, bool want_mod, Str bind_name,
                                  const AstExpr* value, const AstBlock* body,
                                  size_t line, size_t column) {
    AstStmt* out_tail = NULL;
    for (AstTypeField* f = fields; f; f = f->next) {
        if (!f->type || !reflect_type_matches(pattern, f->type)) continue;
        AstTypeRef* ftype = clone_type_ref(ctx->ast_arena, f->type);
        ftype->is_view = want_view; ftype->is_mod = want_mod;
        ftype->is_val = false; ftype->is_own = false; ftype->is_copy = false; ftype->is_opt = false;
        AstExpr* member = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
        memset(member, 0, sizeof *member);
        member->kind = AST_EXPR_MEMBER;
        member->line = value->line; member->column = value->column;
        member->as.member.object = clone_expr(ctx->ast_arena, value);
        member->as.member.member = f->name;
        AstStmt* alias = arena_alloc(ctx->ast_arena, sizeof(AstStmt));
        memset(alias, 0, sizeof *alias);
        alias->kind = AST_STMT_LET;
        alias->line = line; alias->column = column;
        alias->as.let_stmt.name = bind_name;
        alias->as.let_stmt.type = ftype;
        alias->as.let_stmt.is_bind = true;
        alias->as.let_stmt.value = member;
        AstBlock* iter_block = arena_alloc(ctx->ast_arena, sizeof(AstBlock));
        iter_block->first = alias;
        AstBlock* iter_body = clone_block(ctx->ast_arena, body);
        reflect_fold_field_name_block(iter_body, bind_name, f->name);
        alias->next = iter_body ? iter_body->first : NULL;
        AstStmt* wrapper = arena_alloc(ctx->ast_arena, sizeof(AstStmt));
        memset(wrapper, 0, sizeof *wrapper);
        wrapper->kind = AST_STMT_IF;
        wrapper->line = line; wrapper->column = column;
        wrapper->as.if_stmt.condition = reflect_make_true(ctx, line, column);
        wrapper->as.if_stmt.then_block = iter_block;
        if (!out_tail) out->first = wrapper; else out_tail->next = wrapper;
        out_tail = wrapper;
    }
}

static AstExpr* reflect_make_true(CompilerContext* ctx, size_t line, size_t column) {
    AstExpr* e = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
    e->kind = AST_EXPR_BOOL;
    e->as.boolean = true;
    e->line = line; e->column = column;
    return e;
}

// Rewrite a `fields(value)` range loop IN PLACE into `if true { <unrolled> }`,
// where each matching field contributes one scoped `if true { let x => value.f;
// body }`. On any error it becomes an empty `if true {}` and sets had_error, so
// downstream passes see well-formed AST. Returns nothing — the caller then
// analyses the rewritten (now AST_STMT_IF) statement normally.
static void reflect_expand_field_loop(CompilerContext* ctx, AstModule* module,
                                      SymbolTable* symbols, AstStmt* stmt) {
    AstExpr* collection = stmt->as.loop_stmt.condition;
    AstStmt* binding = stmt->as.loop_stmt.init;
    AstBlock* body = stmt->as.loop_stmt.body;
    size_t line = stmt->line, column = stmt->column;

    // Rewrite to an empty `if true {}` up front; fill its block on success.
    AstBlock* out = arena_alloc(ctx->ast_arena, sizeof(AstBlock));
    out->first = NULL;
    stmt->kind = AST_STMT_IF;
    stmt->as.if_stmt.condition = reflect_make_true(ctx, line, column);
    stmt->as.if_stmt.then_block = out;
    stmt->as.if_stmt.else_block = NULL;
    stmt->as.if_stmt.binding = NULL;

    #define REFLECT_FAIL(ln, col, msg) do { \
        diag_error(module->file_path, (int)(ln), (int)(col), (msg)); \
        module->had_error = true; return; } while (0)

    // `fields(value)` must have exactly one positional argument.
    AstCallArg* args = collection->as.call.args;
    if (!args || args->next || args->name.len != 0 || !args->value)
        REFLECT_FAIL(line, column, "fields(...) takes exactly one struct value argument");
    // v2 (#774) will accept a type here; v1 is values only.
    if (sema_arg_is_type_name(symbols, args->value)) {
        REFLECT_FAIL(line, column,
            "fields(Type) over a type is not supported yet (#774); pass a struct VALUE");
    }
    AstExpr* value = args->value;
    sema_analyze_expr(ctx, module, symbols, value);
    TypeInfo* vt = value->resolved_type;
    bool value_is_mod = true; // a bare place/param with no ref wrapper is mutable
    if (vt && vt->kind == TYPE_REF) { value_is_mod = vt->as.ref.is_mod; vt = vt->as.ref.base; }
    if (!vt || (vt->kind != TYPE_STRUCT && vt->kind != TYPE_GENERIC_INST))
        REFLECT_FAIL(value->line, value->column,
            "fields(...) requires a concrete struct value (a generic 'W' type is #773)");
    AstDecl* decl = vt->as.structure.decl;
    if (!decl || decl->kind != AST_DECL_TYPE || !decl->as.type_decl.fields)
        REFLECT_FAIL(value->line, value->column, "fields(...) value has no struct fields");

    // The binding: `let x: <mode> Pattern`. `var` is a copy of a heap struct and
    // is rejected by the same rule the runtime collection loop uses.
    if (!binding || binding->kind != AST_STMT_LET)
        REFLECT_FAIL(line, column, "field loop requires a 'let' binding");
    if (binding->as.let_stmt.is_var)
        REFLECT_FAIL(binding->line, binding->column,
            "collection reference bindings are aliases; use 'let', not 'var'");
    AstTypeRef* pattern = binding->as.let_stmt.type;
    if (!pattern)
        REFLECT_FAIL(binding->line, binding->column,
            "collection loop bindings require an explicit type");
    bool want_mod = pattern->is_mod;
    bool want_view = pattern->is_view;
    if (!want_mod && !want_view)
        REFLECT_FAIL(binding->line, binding->column,
            "a field loop binding must be 'view' or 'mod' (it aliases the field)");
    if (want_mod && !value_is_mod)
        REFLECT_FAIL(binding->line, binding->column,
            "a 'mod' field loop requires a 'mod' struct value");
    Str bind_name = binding->as.let_stmt.name;

    // Unroll via the shared core.
    reflect_fill_unrolled(ctx, out, decl->as.type_decl.fields, pattern,
                          want_view, want_mod, bind_name, value, body, line, column);
    #undef REFLECT_FAIL
}

/* ---- #773: field reflection through a GENERIC world parameter --------------
 *
 * A generic `func f(W: type, world: mod W, ...) { loop ... in fields(world) }`
 * cannot be expanded on the template — the field set depends on the concrete W.
 * The C backend emits a generic body once per concrete instantiation, walking
 * the template with a (generic_params -> concrete_args) substitution context, so
 * THAT is where the per-instantiation expansion must hook in. These helpers take
 * a cloned template body plus the substitution and rewrite each fields() loop
 * using the same core as #772, so the emitted, concrete body is identical to a
 * hand-written per-world clear/serialize. */

// A top-level struct type decl by name in the merged module.
static AstDecl* reflect_find_struct_decl(const AstModule* module, Str name) {
    for (AstDecl* d = module ? module->decls : NULL; d; d = d->next)
        if (d->kind == AST_DECL_TYPE && str_eq(d->as.type_decl.name, name)) return d;
    return NULL;
}

// The concrete type ref of a fields() value expression, resolved from the
// enclosing function's params and the active generic substitution. v1 supports
// the value being a bare parameter identifier (`fields(world)`) — the whole
// point of the generic helper.
static const AstTypeRef* reflect_value_concrete_type(CompilerContext* ctx,
        const AstExpr* value, const AstParam* params,
        const AstIdentifierPart* gparams, const AstTypeRef* cargs) {
    if (!value || value->kind != AST_EXPR_IDENT) return NULL;
    for (const AstParam* p = params; p; p = p->next)
        if (str_eq(p->name, value->as.ident))
            return substitute_type_ref(ctx, gparams, cargs, p->type);
    return NULL;
}

static bool reflect_block_has_fields_loop(const AstBlock* block);
static bool reflect_stmt_has_fields_loop(const AstStmt* s) {
    if (!s) return false;
    if (s->kind == AST_STMT_LOOP && s->as.loop_stmt.is_range && reflect_loop_is_fields(s)) return true;
    switch (s->kind) {
        case AST_STMT_IF:
            return reflect_block_has_fields_loop(s->as.if_stmt.then_block)
                || reflect_block_has_fields_loop(s->as.if_stmt.else_block);
        case AST_STMT_LOOP: return reflect_block_has_fields_loop(s->as.loop_stmt.body);
        case AST_STMT_MATCH:
            for (AstMatchCase* c = s->as.match_stmt.cases; c; c = c->next)
                if (reflect_block_has_fields_loop(c->block)) return true;
            return false;
        case AST_STMT_DEFER: return reflect_block_has_fields_loop(s->as.defer_stmt.block);
        default: return false;
    }
}
static bool reflect_block_has_fields_loop(const AstBlock* block) {
    for (AstStmt* s = block ? block->first : NULL; s; s = s->next)
        if (reflect_stmt_has_fields_loop(s)) return true;
    return false;
}

static void reflect_expand_block_concrete(CompilerContext* ctx, const AstModule* module,
        AstBlock* block, const AstParam* params,
        const AstIdentifierPart* gparams, const AstTypeRef* cargs);
static void reflect_expand_stmt_concrete(CompilerContext* ctx, const AstModule* module,
        AstStmt* stmt, const AstParam* params,
        const AstIdentifierPart* gparams, const AstTypeRef* cargs) {
    if (!stmt) return;
    if (stmt->kind == AST_STMT_LOOP && stmt->as.loop_stmt.is_range && reflect_loop_is_fields(stmt)) {
        AstExpr* collection = stmt->as.loop_stmt.condition;
        AstCallArg* call_args = collection->as.call.args;
        AstStmt* binding = stmt->as.loop_stmt.init;
        if (call_args && !call_args->next && call_args->value && binding
            && binding->kind == AST_STMT_LET && binding->as.let_stmt.type) {
            const AstTypeRef* wt = reflect_value_concrete_type(ctx, call_args->value, params, gparams, cargs);
            AstDecl* decl = wt ? reflect_find_struct_decl(module, reflect_base_name(wt)) : NULL;
            AstTypeRef* pattern = binding->as.let_stmt.type;
            bool want_mod = pattern->is_mod, want_view = pattern->is_view;
            if (decl && decl->kind == AST_DECL_TYPE && decl->as.type_decl.fields
                && (want_mod || want_view)) {
                size_t line = stmt->line, column = stmt->column;
                AstStmt* saved_next = stmt->next;
                AstBlock* out = arena_alloc(ctx->ast_arena, sizeof(AstBlock)); out->first = NULL;
                reflect_fill_unrolled(ctx, out, decl->as.type_decl.fields, pattern,
                                      want_view, want_mod, binding->as.let_stmt.name,
                                      call_args->value, stmt->as.loop_stmt.body, line, column);
                memset(&stmt->as, 0, sizeof stmt->as);
                stmt->kind = AST_STMT_IF;
                stmt->as.if_stmt.condition = reflect_make_true(ctx, line, column);
                stmt->as.if_stmt.then_block = out;
                stmt->next = saved_next;
                // Nested field loops inside the freshly emitted bodies, too.
                reflect_expand_block_concrete(ctx, module, out, params, gparams, cargs);
                return;
            }
        }
        return; // leave unexpanded — the backend emits its usual fallback
    }
    switch (stmt->kind) {
        case AST_STMT_IF:
            reflect_expand_block_concrete(ctx, module, stmt->as.if_stmt.then_block, params, gparams, cargs);
            reflect_expand_block_concrete(ctx, module, stmt->as.if_stmt.else_block, params, gparams, cargs);
            break;
        case AST_STMT_LOOP:
            reflect_expand_block_concrete(ctx, module, stmt->as.loop_stmt.body, params, gparams, cargs);
            break;
        case AST_STMT_MATCH:
            for (AstMatchCase* c = stmt->as.match_stmt.cases; c; c = c->next)
                reflect_expand_block_concrete(ctx, module, c->block, params, gparams, cargs);
            break;
        case AST_STMT_DEFER:
            reflect_expand_block_concrete(ctx, module, stmt->as.defer_stmt.block, params, gparams, cargs);
            break;
        default: break;
    }
}
static void reflect_expand_block_concrete(CompilerContext* ctx, const AstModule* module,
        AstBlock* block, const AstParam* params,
        const AstIdentifierPart* gparams, const AstTypeRef* cargs) {
    for (AstStmt* s = block ? block->first : NULL; s; s = s->next)
        reflect_expand_stmt_concrete(ctx, module, s, params, gparams, cargs);
}

// Public (#773): if `template_body` contains a fields() loop, return a CLONE with
// every loop expanded for the concrete generic args; else NULL (emit as-is).
AstBlock* reflect_instantiate_body(CompilerContext* ctx, const AstModule* module,
        const AstBlock* template_body, const AstParam* params,
        const AstIdentifierPart* gparams, const AstTypeRef* cargs) {
    if (!template_body || !reflect_block_has_fields_loop(template_body)) return NULL;
    AstBlock* cloned = clone_block(ctx->ast_arena, template_body);
    reflect_expand_block_concrete(ctx, module, cloned, params, gparams, cargs);
    return cloned;
}

static void sema_analyze_stmt(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt, TypeInfo* current_return_type) {
    // #772: a `loop ... in fields(value)` is compile-time field reflection, not
    // a runtime loop. Unroll it into `if true { ... }` here — BEFORE the switch
    // — so the switch (and both backends) see ordinary statements.
    if (stmt && stmt->kind == AST_STMT_LOOP && stmt->as.loop_stmt.is_range
        && reflect_loop_is_fields(stmt)) {
        reflect_expand_field_loop(ctx, module, symbols, stmt);
    }
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
            // A `mod` binding mints mutable access, so its source must be
            // mutable: an owned place, a mod param, a mod binding, or a call
            // returning `mod T`. A view of any shape cannot be promoted —
            // before this check, `let y: mod T => x` with `x: view T`
            // compiled and the writes silently vanished (#460).
            if (stmt->as.let_stmt.is_bind && stmt->as.let_stmt.type
                && stmt->as.let_stmt.type->is_mod && stmt->as.let_stmt.value) {
                const AstExpr* src = stmt->as.let_stmt.value;
                while (src && (src->kind == AST_EXPR_MEMBER || src->kind == AST_EXPR_INDEX)) {
                    src = (src->kind == AST_EXPR_MEMBER) ? src->as.member.object
                                                         : src->as.index.target;
                }
                bool src_is_view = false;
                if (src && src->kind == AST_EXPR_IDENT) {
                    Symbol* ssym = symbol_table_lookup(symbols, src->as.ident);
                    if (ssym && ((ssym->type && ssym->type->kind == TYPE_REF
                                  && !ssym->type->as.ref.is_mod)
                                 || (ssym->is_immutable
                                     && ssym->bind_kind == BIND_READONLY_REF))) {
                        src_is_view = true;
                    }
                } else if (src && src->kind == AST_EXPR_CALL && src->decl_link
                           && src->decl_link->kind == AST_DECL_FUNC) {
                    const AstFuncDecl* sfd = &src->decl_link->as.func_decl;
                    if (sfd->returns && sfd->returns->type
                        && sfd->returns->type->is_view && !sfd->returns->type->is_mod) {
                        src_is_view = true;
                    }
                }
                if (src_is_view) {
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                               "cannot bind 'mod' to a read-only view: a view cannot be promoted to mutable access");
                    module->had_error = true;
                }
            }
            // Spec 2.3.1: an owned binding cannot take a reference-returning
            // call — "copy what the view refers to" would hide a potentially
            // deep copy behind a call whose viewness is only visible at its
            // declaration. The two-step form keeps the copy next to a locally
            // declared view; copying from a NAMED view binding stays legal.
            if (!stmt->as.let_stmt.is_bind && stmt->as.let_stmt.type
                && !stmt->as.let_stmt.type->is_view && !stmt->as.let_stmt.type->is_mod
                && stmt->as.let_stmt.value
                && stmt->as.let_stmt.value->kind == AST_EXPR_CALL
                && stmt->as.let_stmt.value->decl_link
                && stmt->as.let_stmt.value->decl_link->kind == AST_DECL_FUNC) {
                const AstFuncDecl* vfd = &stmt->as.let_stmt.value->decl_link->as.func_decl;
                if (vfd->returns && vfd->returns->type
                    && (vfd->returns->type->is_view || vfd->returns->type->is_mod)
                    && !vfd->returns->type->is_opt) {
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                               "this call returns a reference; copying what it refers to must be spelled out: "
                               "bind it first ('let source: view T => ...'), then copy from the named binding");
                    module->had_error = true;
                }
            }
            // Spec 2.3.1: a reference binding cannot take an owned result. A
            // fresh value needs an owner, and a view/mod binding refuses
            // ownership by definition — accepting this would mean inventing a
            // hidden owner (#454/#455, reversing #443's materialisation).
            // Optional-reference bindings (is_opt) are excluded: their legal
            // sources are reference-returning calls, checked elsewhere, and
            // the if-let narrowing forms are handled by that construct.
            if (stmt->as.let_stmt.is_bind && stmt->as.let_stmt.type
                && (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod)
                && !stmt->as.let_stmt.type->is_opt && stmt->as.let_stmt.value) {
                const AstExpr* bv = stmt->as.let_stmt.value;
                if (bv->kind == AST_EXPR_OBJECT || bv->kind == AST_EXPR_COLLECTION_LITERAL) {
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                               "cannot bind a reference to a literal: a literal is a fresh value with no storage to view; "
                               "write 'let name: T = ...' to own it");
                    module->had_error = true;
                } else if (bv->kind == AST_EXPR_CALL && bv->decl_link
                           && bv->decl_link->kind == AST_DECL_FUNC) {
                    const AstFuncDecl* bfd = &bv->decl_link->as.func_decl;
                    bool returns_ref = bfd->returns && bfd->returns->type
                        && (bfd->returns->type->is_view || bfd->returns->type->is_mod);
                    if (bfd->returns && !returns_ref) {
                        diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                                   "cannot bind a reference to a call that returns ownership; "
                                   "write 'let name: T = ...' to take the value, or use a reference-returning "
                                   "accessor (viewAt, modAt, viewGet, modGet)");
                        module->had_error = true;
                    }
                }
            }
            // #661: an anonymous `{ }`/`{ ... }` list literal on an
            // Array/Buffer-typed binding was lowered as a List (createList),
            // misgenerating C (a `rae_List_<T>` initialising a
            // `rae_Array_<T>_<N>`, or an undeclared list-of-struct helper).
            // These containers carry their capacity in their type, so they are
            // constructed with `Array(T, cap: N)` / `Buffer(T, cap: N)` — which
            // zero-initializes — not with a list literal. Reject with a message
            // that names the right form.
            if (!stmt->as.let_stmt.is_bind && stmt->as.let_stmt.type
                && stmt->as.let_stmt.value
                && stmt->as.let_stmt.value->kind == AST_EXPR_COLLECTION_LITERAL) {
                Str dbase = get_base_type_name(stmt->as.let_stmt.type);
                if (str_eq_cstr(dbase, "Array") || str_eq_cstr(dbase, "Buffer")) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "a '{ }' list literal cannot initialize an %.*s: its capacity is part of its type — "
                        "construct it with '%.*s(T, cap: N)' (which zero-initializes), then assign elements by index",
                        (int)dbase.len, dbase.data, (int)dbase.len, dbase.data);
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column, buf);
                    module->had_error = true;
                }
            }
            // `let` and `const` are immutable bindings; only `var` may be
            // reassigned. (Mutating the value a `let` points at — container
            // methods, field/index writes — is unaffected; this only governs
            // rebinding the local itself.)
            bool immut = !stmt->as.let_stmt.is_var;
            Symbol* sym = symbol_table_define(symbols, ctx->ast_arena, stmt->as.let_stmt.name, NULL, t, immut);
            /* A `let`/`var` in a body owns frame storage — unless it is
             * itself a reference binding, which merely names storage that
             * lives somewhere else and can be forwarded on. */
            if (sym && !(stmt->as.let_stmt.type
                         && (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod))) {
                sym->is_local_storage = true;
            }
            /* No alias marking on `let` bindings.
             *
             * §2.4 proposed one-hop tracking on the assumption that
             * `let x = <borrow>` aliases. Measuring codegen showed it does
             * not: ident, field and element initialisers all deep-copy —
             * `=` copies, which is the language rule. A binding therefore
             * always owns what it holds, and marking it non-owning was a
             * false positive that rejected working code.
             *
             * The hazards that remain are at CALL SITES, where an `own`
             * parameter does not copy: passing a borrow, or a field read
             * through one, straight into `own`. Those are caught by
             * expr_is_owning. */
            if (stmt->as.let_stmt.is_const) {
                sym->bind_kind = BIND_CONST;
                sema_fold_const(ctx, module, symbols, stmt->as.let_stmt.value, sym, stmt->line, stmt->column);
            } else {
                sym->bind_kind = immut ? BIND_LET : BIND_MUTABLE;
            }
            break;
        }
        case AST_STMT_RET: {
            AstReturnArg* arg = stmt->as.ret_stmt.values;
            while (arg) {
                if (arg->value) {
                    sema_analyze_expr(ctx, module, symbols, arg->value);
                    sema_check_returned_ref(ctx, module, symbols, arg->value);
                    if (current_return_type) ensure_type_match(ctx, current_return_type, &arg->value);
                }
                arg = arg->next;
            }
            break;
        }
        case AST_STMT_IF: {
            if (stmt->as.if_stmt.binding) {
                // The binding is introduced before the condition, and its name
                // is visible in the then-branch. Scope handling below.
                bool saved_if_let = s_in_if_let_binding;
                s_in_if_let_binding = true;
                sema_analyze_stmt(ctx, module, symbols, stmt->as.if_stmt.binding, current_return_type);
                s_in_if_let_binding = saved_if_let;
                // Spec 4.2: `if let` is the 2.3.1 matrix with a presence test
                // in front — which binding form is legal follows from what
                // the optional holds and where it lives.
                AstStmt* b = stmt->as.if_stmt.binding;
                const AstTypeRef* btr = b->as.let_stmt.type;
                if (btr && btr->is_opt && (btr->is_view || btr->is_mod)) {
                    /* The declaration carries `opt` so its initializer can be
                     * checked, but inside the successful branch the name is
                     * the narrowed reference, not an optional. Keeping the
                     * optional TypeInfo here previously relied on implicit
                     * unboxing in every expression that used the binding. */
                    AstTypeRef narrowed = *btr;
                    narrowed.is_opt = false;
                    narrowed.resolved_type = NULL;
                    Symbol* narrowed_symbol = symbol_table_lookup(
                        symbols, b->as.let_stmt.name);
                    if (narrowed_symbol) {
                        narrowed_symbol->type = sema_resolve_type_internal(
                            ctx, module, symbols, &narrowed);
                    }
                }
                const AstExpr* bsrc = b->as.let_stmt.value;
                if (bsrc && bsrc->kind == AST_EXPR_UNBOX) bsrc = bsrc->as.unary.operand;
                bool is_call_src = bsrc && (bsrc->kind == AST_EXPR_CALL
                                            || bsrc->kind == AST_EXPR_METHOD_CALL);
                /* `if let x: view/mod T => list.copyAt(...)` (#644): `copyAt`
                 * returns a COPY (opt T by value); a reference binding cannot
                 * alias it. This is the cross-form the old peephole silently
                 * aliased. `viewAt`/`modAt` return `opt view/mod T` and are
                 * unaffected (they are not List value accessors). */
                /* Plain-call form `copyAt(list, index:)` may not have its
                 * decl_link resolved here, so also match the callee name — after
                 * #643 `copyAt` is defined only on `List`, so it is unambiguous. */
                bool bsrc_is_copy_accessor = sema_is_list_value_accessor(bsrc)
                    || (bsrc && bsrc->kind == AST_EXPR_CALL && bsrc->as.call.callee
                        && bsrc->as.call.callee->kind == AST_EXPR_IDENT
                        && str_eq_cstr(bsrc->as.call.callee->as.ident, "copyAt"));
                if (btr && (btr->is_view || btr->is_mod) && bsrc_is_copy_accessor) {
                    diag_error(module->file_path, (int)b->line, (int)b->column,
                               "`copyAt` returns a copy; use `viewAt` or `modAt` to alias storage");
                    module->had_error = true;
                }
                if (btr && !(btr->is_view || btr->is_mod)) {
                    // Owned narrowing takes ownership, so its source must be
                    // a PRODUCED optional. From a place it would be a hidden
                    // deep copy of a payload that stays behind — view the
                    // place and copy inside the branch, where the copy is
                    // visible.
                    if (!is_call_src) {
                        diag_error(module->file_path, (int)b->line, (int)b->column,
                                   "'if let <name>: T = ...' takes ownership, so it needs a call producing "
                                   "'opt T'; narrowing a stored optional copies — bind 'view T =>' and copy "
                                   "inside the branch if a copy is wanted (spec 4.2)");
                        module->had_error = true;
                    } else if (bsrc->kind == AST_EXPR_CALL && bsrc->decl_link
                               && bsrc->decl_link->kind == AST_DECL_FUNC) {
                        const AstFuncDecl* cfd = &bsrc->decl_link->as.func_decl;
                        const AstTypeRef* rt = cfd->returns ? cfd->returns->type : NULL;
                        if (rt && rt->is_opt && (rt->is_view || rt->is_mod)) {
                            diag_error(module->file_path, (int)b->line, (int)b->column,
                                       "this call returns an optional REFERENCE; narrow it with "
                                       "'if let <name>: view T => ...' (or mod) instead of taking ownership");
                            module->had_error = true;
                        } else if (rt && !rt->is_opt) {
                            diag_error(module->file_path, (int)b->line, (int)b->column,
                                       "'if let' narrows an optional, but this call does not return 'opt'");
                            module->had_error = true;
                        }
                    }
                } else if (btr && is_call_src && bsrc->kind == AST_EXPR_CALL
                           && bsrc->decl_link && bsrc->decl_link->kind == AST_DECL_FUNC) {
                    // Reference narrowing of a PRODUCED optional is legal only
                    // when the optional holds a reference. A produced owned
                    // optional has no owner a view could lean on — `if` adds a
                    // presence test, not an owner. Take it with `=`.
                    const AstFuncDecl* cfd = &bsrc->decl_link->as.func_decl;
                    const AstTypeRef* rt = cfd->returns ? cfd->returns->type : NULL;
                    if (rt && rt->is_opt && !(rt->is_view || rt->is_mod)) {
                        diag_error(module->file_path, (int)b->line, (int)b->column,
                                   "this call returns an owned optional; a view/mod binding refuses ownership "
                                   "and nothing else would own the value — take it with "
                                   "'if let <name>: T = ...' (spec 4.2)");
                        module->had_error = true;
                    }
                }
                // #658: aliasing borrow check for `viewAt`/`modAt` bindings.
                // Both return `opt view/mod T` — a raw pointer into the List's
                // backing storage, captured ONCE when the binding is
                // established. A structural mutation of the SAME List inside the
                // live then-branch can reallocate that storage (`add` past
                // capacity -> grow -> realloc), leaving the reference dangling:
                // a silent use-after-free. Reuse the collection-loop borrow
                // check to reject the structural mutators on the aliased List in
                // the then-branch. `copyAt` returns a detached value (no alias)
                // and is exempt; a DIFFERENT List is unaffected.
                const AstExpr* aliased = NULL;
                if (bsrc && bsrc->kind == AST_EXPR_METHOD_CALL
                    && (str_eq_cstr(bsrc->as.method_call.method_name, "viewAt")
                        || str_eq_cstr(bsrc->as.method_call.method_name, "modAt"))) {
                    aliased = bsrc->as.method_call.object;
                } else if (bsrc && bsrc->kind == AST_EXPR_CALL && bsrc->as.call.callee
                           && bsrc->as.call.callee->kind == AST_EXPR_IDENT
                           && (str_eq_cstr(bsrc->as.call.callee->as.ident, "viewAt")
                               || str_eq_cstr(bsrc->as.call.callee->as.ident, "modAt"))
                           && bsrc->as.call.args) {
                    // Plain-call form `viewAt(list, index:)`: the List is the
                    // first positional argument.
                    aliased = bsrc->as.call.args->value;
                }
                if (aliased) {
                    for (const AstStmt* s = stmt->as.if_stmt.then_block
                             ? stmt->as.if_stmt.then_block->first : NULL;
                         s; s = s->next) {
                        if (sema_stmt_mutates_collection(s, aliased)) {
                            const AstExpr* err_expr = s->kind == AST_STMT_EXPR
                                ? s->as.expr_stmt : NULL;
                            int err_line = (int)(err_expr ? err_expr->line : s->line);
                            int err_col = (int)(err_expr ? err_expr->column : s->column);
                            diag_error(module->file_path, err_line, err_col,
                                       "cannot mutate a List while a `viewAt`/`modAt` binding aliases one of its elements");
                            module->had_error = true;
                        }
                    }
                }
            }
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
             if (stmt->as.loop_stmt.is_range) {
                 AstExpr* collection = stmt->as.loop_stmt.condition;
                 if (collection) sema_analyze_expr(ctx, module, symbols, collection);
                 TypeInfo* collection_type = collection ? collection->resolved_type : NULL;
                 if (collection_type && collection_type->kind == TYPE_REF) {
                     collection_type = collection_type->as.ref.base;
                 }
                 if (!sema_struct_template_is(collection_type, "List")
                     || collection_type->as.structure.generic_count != 1) {
                     diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                                "collection loops currently require a List(T) expression");
                     module->had_error = true;
                 } else if (stmt->as.loop_stmt.init
                            && stmt->as.loop_stmt.init->kind == AST_STMT_LET) {
                     AstStmt* binding = stmt->as.loop_stmt.init;
                     TypeInfo* element_type = collection_type->as.structure.generic_args[0];
                     if (!binding->as.let_stmt.type) {
                         diag_error(module->file_path, (int)binding->line,
                                    (int)binding->column,
                                    "collection loop bindings require an explicit type");
                         module->had_error = true;
                     } else {
                         TypeInfo* binding_type = sema_resolve_type_internal(
                             ctx, module, symbols, binding->as.let_stmt.type);
                         TypeInfo* binding_value_type = binding_type;
                         if (binding_value_type && binding_value_type->kind == TYPE_REF) {
                             binding_value_type = binding_value_type->as.ref.base;
                         }
                         if (!type_is_same(binding_value_type, element_type)) {
                             diag_error(module->file_path, (int)binding->line,
                                        (int)binding->column,
                                        "collection loop binding type must match the List element type");
                             module->had_error = true;
                         }
                         if (binding->as.let_stmt.type->is_mod
                             && collection && collection->resolved_type
                             && collection->resolved_type->kind == TYPE_REF
                             && !collection->resolved_type->as.ref.is_mod) {
                             diag_error(module->file_path, (int)binding->line,
                                        (int)binding->column,
                                        "a 'mod' collection loop requires mutable List storage");
                             module->had_error = true;
                         }
                         sema_analyze_stmt(ctx, module, symbols, binding,
                                           current_return_type);
                     }
                 }
             } else {
                 if (stmt->as.loop_stmt.init) sema_analyze_stmt(ctx, module, symbols, stmt->as.loop_stmt.init, current_return_type);
                 if (stmt->as.loop_stmt.condition) sema_analyze_expr(ctx, module, symbols, stmt->as.loop_stmt.condition);
             }
             if (stmt->as.loop_stmt.increment) sema_analyze_expr(ctx, module, symbols, stmt->as.loop_stmt.increment);
             s_loop_depth++;
             if (stmt->as.loop_stmt.body) {
                 AstStmt* s = stmt->as.loop_stmt.body->first;
                 while (s) {
                     if (stmt->as.loop_stmt.is_range
                         && sema_stmt_mutates_collection(s, stmt->as.loop_stmt.condition)) {
                         const AstExpr* error_expr = s->kind == AST_STMT_EXPR
                             ? s->as.expr_stmt : NULL;
                         int error_line = (int)(error_expr ? error_expr->line : s->line);
                         int error_column = (int)(error_expr ? error_expr->column : s->column);
                         diag_error(module->file_path, error_line, error_column,
                                    "cannot mutate a List while a collection loop borrows its elements");
                         module->had_error = true;
                     }
                     sema_analyze_stmt(ctx, module, symbols, s, current_return_type);
                     s = s->next;
                 }
             }
             s_loop_depth--;
             symbol_table_pop_scope(symbols);
             break;
        }
        case AST_STMT_BREAK:
        case AST_STMT_CONTINUE:
             if (s_loop_depth == 0) {
                 diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                            stmt->kind == AST_STMT_BREAK
                                ? "'break' is only valid inside a loop"
                                : "'continue' is only valid inside a loop");
                 module->had_error = true;
             }
             break;
        case AST_STMT_MATCH: {
            if (stmt->as.match_stmt.subject) sema_analyze_expr(ctx, module, symbols, stmt->as.match_stmt.subject);
            bool has_default = false;
            const AstDecl* enum_decl = NULL;
            Str enum_name = {0};
            for (AstMatchCase* mc = stmt->as.match_stmt.cases; mc; mc = mc->next) {
                if (!mc->pattern) { has_default = true; }
                else {
                    sema_analyze_expr(ctx, module, symbols, mc->pattern);
                    // Analyze every or-pattern too (`case A, B, C`).
                    for (AstCasePattern* op = mc->or_patterns; op; op = op->next)
                        sema_analyze_expr(ctx, module, symbols, op->expr);
                    if (!enum_decl && mc->pattern->kind == AST_EXPR_MEMBER &&
                        mc->pattern->as.member.object->kind == AST_EXPR_IDENT) {
                        Str obj_name = mc->pattern->as.member.object->as.ident;
                        Symbol* sym = symbol_table_lookup(symbols, obj_name);
                        if (sym && sym->decl && sym->decl->kind == AST_DECL_ENUM) {
                            enum_decl = sym->decl;
                            enum_name = obj_name;
                        }
                    }
                }
                if (mc->block) {
                    symbol_table_push_scope(symbols);
                    AstStmt* s = mc->block->first;
                    while (s) { sema_analyze_stmt(ctx, module, symbols, s, current_return_type); s = s->next; }
                    symbol_table_pop_scope(symbols);
                }
            }
            // The `default` rule is chosen by the subject's value space
            // (docs/match-and-sum-types.md §"default: the subject's type
            // decides"). A CLOSED set (enum / Bool) forbids `default` and must
            // name every case; an OPEN set (Int / String) requires one, since
            // no finite list of cases is exhaustive.
            if (enum_decl) {
                if (has_default) {
                    // Flipping the old escape hatch: a `default` on an enum
                    // would silently swallow a newly added variant, which is
                    // exactly the review-forcing property we want to keep.
                    char buffer[256];
                    snprintf(buffer, sizeof(buffer),
                        "match on enum '%.*s' must not use a 'default' arm; "
                        "name every variant (remove 'default' and add the missing case%s)",
                        (int)enum_name.len, enum_name.data,
                        "s, or group them with an or-pattern");
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column, buffer);
                    module->had_error = true;
                } else {
                    for (AstEnumMember* em = enum_decl->as.enum_decl.members; em; em = em->next) {
                        bool covered = false;
                        for (AstMatchCase* mc = stmt->as.match_stmt.cases; mc && !covered; mc = mc->next) {
                            if (sema_pattern_names_member(mc->pattern, enum_name, em->name)) { covered = true; break; }
                            for (AstCasePattern* op = mc->or_patterns; op; op = op->next)
                                if (sema_pattern_names_member(op->expr, enum_name, em->name)) { covered = true; break; }
                        }
                        if (!covered) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer),
                                "non-exhaustive match on enum '%.*s': missing case '%.*s.%.*s' (add it)",
                                (int)enum_name.len, enum_name.data,
                                (int)enum_name.len, enum_name.data,
                                (int)em->name.len, em->name.data);
                            diag_error(module->file_path, (int)stmt->line, (int)stmt->column, buffer);
                            module->had_error = true;
                            break;
                        }
                    }
                }
            } else {
                // Non-enum subject: an open value space (Int/String) requires a
                // `default`; Bool is a closed 2-state set and forbids one.
                TypeInfo* subj = stmt->as.match_stmt.subject
                    ? stmt->as.match_stmt.subject->resolved_type : NULL;
                if (subj && subj->kind == TYPE_REF) subj = subj->as.ref.base;
                bool is_open = subj && (subj->kind == TYPE_INT || subj->kind == TYPE_STRING);
                bool is_bool = subj && subj->kind == TYPE_BOOL;
                if (is_open && !has_default) {
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                        subj->kind == TYPE_STRING
                            ? "match on String requires a 'default' arm (its value space is open-ended)"
                            : "match on Int requires a 'default' arm (its value space is open-ended)");
                    module->had_error = true;
                } else if (is_bool && has_default) {
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column,
                        "match on Bool must not use a 'default' arm; handle 'case true' and 'case false'");
                    module->had_error = true;
                }
            }
            break;
        }
        case AST_STMT_ASSIGN: {
            sema_analyze_expr(ctx, module, symbols, stmt->as.assign_stmt.target);
            sema_analyze_expr(ctx, module, symbols, stmt->as.assign_stmt.value);
            if (stmt->as.assign_stmt.target->resolved_type) ensure_type_match(ctx, stmt->as.assign_stmt.target->resolved_type, &stmt->as.assign_stmt.value);
            // View restriction checks
            if (stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT) {
                // Rebinding the local itself: legal only for `var`.
                Symbol* sym = symbol_table_lookup(symbols, stmt->as.assign_stmt.target->as.ident);
                // ASSIGNING THROUGH A `mod` ALIAS IS NOT REBINDING IT.
                //
                // Spec 2.2: "Copying through a mod alias using `=` is legal and
                // modifies the underlying target", and its example binds with
                // `let`. What `let` forbids is REBINDING the name, which
                // bind-once already forbids for every alias regardless. So a
                // `let`-bound `mod` alias may be written through; the write
                // lands in the aliased storage, not in the binding.
                bool is_mod_alias = sym && sym->type
                    && ((sym->type->kind == TYPE_REF && sym->type->as.ref.is_mod)
                        || (sym->type->kind == TYPE_OPT && sym->type->as.opt.base
                            && sym->type->as.opt.base->kind == TYPE_REF
                            && sym->type->as.opt.base->as.ref.is_mod));
                if (sym && sym->is_immutable && !is_mod_alias) {
                    char buffer[200];
                    int nl = (int)stmt->as.assign_stmt.target->as.ident.len;
                    const char* nm = stmt->as.assign_stmt.target->as.ident.data;
                    bool is_view_alias = sym->type && sym->type->kind == TYPE_REF
                                         && !sym->type->as.ref.is_mod;
                    if (sym->bind_kind == BIND_CONST) {
                        snprintf(buffer, sizeof(buffer), "cannot assign to constant '%.*s'", nl, nm);
                    } else if (is_view_alias && sym->bind_kind == BIND_READONLY_REF) {
                        snprintf(buffer, sizeof(buffer), "cannot assign to read-only view parameter '%.*s'; declare it 'mod' to allow writes", nl, nm);
                    } else if (is_view_alias) {
                        // Suggesting `var` here would be a second error:
                        // aliases are bind-once. The problem is the VIEW.
                        snprintf(buffer, sizeof(buffer), "cannot assign through read-only view '%.*s'; bind with 'mod' to write through an alias", nl, nm);
                    } else if (sym->bind_kind == BIND_LET) {
                        snprintf(buffer, sizeof(buffer), "cannot reassign immutable 'let' binding '%.*s'; declare it with 'var' to allow reassignment", nl, nm);
                    } else {
                        snprintf(buffer, sizeof(buffer), "cannot assign to read-only view identifier '%.*s'", nl, nm);
                    }
                    // Report the ENCLOSING function's file, not module->file_path:
                    // sibling files merge into one AstModule that remembers only
                    // one path, so a statement in a non-recorded file would be
                    // blamed on the wrong file (#221). s_current_decl_origin
                    // tracks the file the current function was parsed from.
                    diag_error(s_current_decl_origin ? s_current_decl_origin : module->file_path,
                               (int)stmt->line, (int)stmt->column, buffer);
                    module->had_error = true;
                }
            } else if (stmt->as.assign_stmt.target->kind == AST_EXPR_MEMBER
                       || stmt->as.assign_stmt.target->kind == AST_EXPR_INDEX) {
                // Mutating a FIELD or element. Two read-only shapes forbid it:
                //   - a view PARAM (bind_kind BIND_READONLY_REF), and
                //   - a view BINDING (`let v: view T => x`), whose bind_kind is
                //     BIND_LET but whose TYPE is a non-mod reference. The old
                //     check only knew the first, so writes through a view
                //     binding compiled and mutated the original (#459).
                // Walk nested access (a.b.c, a[i].b) to the base name; the
                // read-only property belongs to the base.
                const AstExpr* base = stmt->as.assign_stmt.target;
                while (base && (base->kind == AST_EXPR_MEMBER || base->kind == AST_EXPR_INDEX)) {
                    base = (base->kind == AST_EXPR_MEMBER) ? base->as.member.object
                                                           : base->as.index.target;
                }
                Symbol* sym = (base && base->kind == AST_EXPR_IDENT)
                              ? symbol_table_lookup(symbols, base->as.ident) : NULL;
                bool is_view_binding = sym && sym->type
                    && ((sym->type->kind == TYPE_REF && !sym->type->as.ref.is_mod)
                        || (sym->type->kind == TYPE_OPT && sym->type->as.opt.base
                            && sym->type->as.opt.base->kind == TYPE_REF
                            && !sym->type->as.opt.base->as.ref.is_mod));
                if (sym && ((sym->is_immutable && sym->bind_kind == BIND_READONLY_REF)
                            || is_view_binding)) {
                    char buffer[160];
                    snprintf(buffer, sizeof(buffer), "cannot mutate through read-only view '%.*s'",
                        (int)base->as.ident.len, base->as.ident.data);
                    diag_error(module->file_path, (int)stmt->line, (int)stmt->column, buffer);
                    module->had_error = true;
                }
            }
            break;
        }
        default: break;
    }
}

/* Distinct numeric types never convert implicitly — see
 * docs/primitive-types.md. `Float` and `Float32` are the SAME type so they
 * are not a conversion; `Float`(f32) and `Float64` are different types and
 * require an explicit `as`. */
static bool sema_is_numeric_kind(TypeKind k) {
    return k == TYPE_INT || k == TYPE_FLOAT || k == TYPE_FLOAT64;
}

// True if the type ref names an enum declaration. Enums have no dedicated
// TypeKind (they resolve to Void in sema), so they are recognised by the written
// type name resolving to an AST_DECL_ENUM symbol.
static bool sema_typeref_is_enum(SymbolTable* symbols, const AstTypeRef* tr) {
    if (!tr || !tr->parts || tr->parts->text.len == 0) return false;
    Symbol* sym = symbol_table_lookup(symbols, tr->parts->text);
    return sym && sym->decl && sym->decl->kind == AST_DECL_ENUM;
}

/* An unsuffixed literal is not a typed runtime value being converted — the
 * compiler materialises it directly in the destination type, so
 * `let x: Float64 = 1.5` needs no cast. Only already-typed values do. */
static bool sema_is_numeric_literal(const AstExpr* e) {
    if (!e) return false;
    if (e->kind == AST_EXPR_FLOAT || e->kind == AST_EXPR_INTEGER) return true;
    /* -1.5 and +1.5 are still literals. */
    if (e->kind == AST_EXPR_UNARY && e->as.unary.op == AST_UNARY_NEG) {
        return sema_is_numeric_literal(e->as.unary.operand);
    }
    /* Constant-folded literal arithmetic stays literal (1.0 / 3.0). */
    if (e->kind == AST_EXPR_BINARY) {
        return sema_is_numeric_literal(e->as.binary.lhs) &&
               sema_is_numeric_literal(e->as.binary.rhs);
    }
    return false;
}

/* See through `view T` / `mod T` for the numeric-conversion check.
 *
 * A reference-mode parameter's TypeInfo is TYPE_REF wrapping the numeric
 * type, so a check that looked only at the outer kind silently skipped every
 * value arriving as `view Float64` / `mod Float64` — which is exactly how
 * four epoch timestamps ended up narrowed into `Float` fields in the mobile
 * UI (#407). Reading through a reference does not change the value's type,
 * so neither should the rule. */
static TypeInfo* sema_strip_ref(TypeInfo* t) {
    while (t && t->kind == TYPE_REF && t->as.ref.base) t = t->as.ref.base;
    return t;
}

static const char* sema_numeric_name(TypeKind k) {
    switch (k) {
        case TYPE_INT: return "Int";
        case TYPE_FLOAT: return "Float";
        case TYPE_FLOAT64: return "Float64";
        default: return "?";
    }
}

// A List value-accessor call — `list.at(index:)` / `list.get(index:)` — that
// yields `opt T`. Unlike the reference accessors (viewAt/modAt), these copy the
// element out, so a non-optional binding would silently unwrap. Detected
// STRUCTURALLY (method name + List receiver), because generic List method calls
// are not resolved to `opt T` in sema — the C backend resolves them — so the
// call node's own resolved_type is often null and never reaches the TYPE_OPT
// branch in ensure_type_match.
static bool sema_is_list_value_accessor(const AstExpr* expr) {
    if (!expr) return false;
    if (expr->kind == AST_EXPR_METHOD_CALL) {
        Str method = expr->as.method_call.method_name;
        if (!str_eq_cstr(method, "copyAt")) return false;
        if (!expr->as.method_call.object) return false;
        TypeInfo* receiver = expr->as.method_call.object->resolved_type;
        while (receiver && (receiver->kind == TYPE_REF || receiver->kind == TYPE_OPT)) {
            receiver = receiver->kind == TYPE_REF ? receiver->as.ref.base
                                                  : receiver->as.opt.base;
        }
        return sema_struct_template_is(receiver, "List");
    }
    if (expr->kind == AST_EXPR_CALL && expr->decl_link
        && expr->decl_link->kind == AST_DECL_FUNC) {
        const AstFuncDecl* function = &expr->decl_link->as.func_decl;
        if (!str_eq_cstr(function->name, "copyAt"))
            return false;
        // The receiver is the first non-type param; core.rae accessors lead
        // with `T: type`, so skip that to reach `this: view List(T)`.
        const AstParam* receiver = function->params;
        if (receiver && receiver->type
            && str_eq_cstr(get_base_type_name(receiver->type), "type")) {
            receiver = receiver->next;
        }
        return receiver && receiver->type
            && str_eq_cstr(get_base_type_name(receiver->type), "List");
    }
    return false;
}

static void ensure_type_match(CompilerContext* ctx, TypeInfo* expected, AstExpr** expr_ptr) {
    if (!expected || !expr_ptr || !*expr_ptr) return;
    AstExpr* expr = *expr_ptr;
    /* A List value-accessor result (`opt T`) cannot land in a non-optional
     * binding, argument, or return. These generic calls are not resolved to
     * `opt T` in sema — their call node's resolved_type is often null — so
     * they slip past both the early return below and the TYPE_OPT branch and
     * would silently unwrap; catch them by shape here, BEFORE the
     * resolved-type checks. Consume with `if let`. */
    if (!s_in_if_let_binding && expected->kind != TYPE_OPT
        && sema_is_list_value_accessor(expr)) {
        const char* err_file = s_current_decl_origin ? s_current_decl_origin : NULL;
        if (expected->kind == TYPE_REF) {
            /* `let x: view/mod T => list.copyAt(...)` (#644): `copyAt` returns a
             * COPY of the element (opt T by value), so a reference binding can't
             * alias it — the accessor's name and return type must agree. */
            diag_error(err_file, (int)expr->line, (int)expr->column,
                       "`copyAt` returns a copy; use `viewAt` or `modAt` to alias storage");
        } else {
            diag_error(err_file, (int)expr->line, (int)expr->column,
                       "optional not unwrapped — use `if let`");
        }
        if (s_current_module) s_current_module->had_error = true;
        return;
    }
    if (!expr->resolved_type) return;
    /* HARD ERROR on implicit numeric conversion (both widening and
     * narrowing). Silently narrowing a Float64 epoch to f32 destroyed
     * animation timing and FPS in the gpu3d examples; a warning would not
     * have stopped it. Literals are exempt: they are materialised directly
     * in the destination type, not converted. */
    TypeInfo* want_num = sema_strip_ref(expected);
    TypeInfo* got_num = sema_strip_ref(expr->resolved_type);
    if (want_num && got_num &&
        sema_is_numeric_kind(want_num->kind) &&
        sema_is_numeric_kind(got_num->kind) &&
        want_num->kind != got_num->kind &&
        expr->kind != AST_EXPR_CAST &&
        !sema_is_numeric_literal(expr)) {
        char buf[240];
        snprintf(buf, sizeof(buf),
            "cannot implicitly convert %s to %s; Rae has no implicit numeric conversions - write `value as %s`",
            sema_numeric_name(got_num->kind),
            sema_numeric_name(want_num->kind),
            sema_numeric_name(want_num->kind));
        const char* err_file = s_current_decl_origin ? s_current_decl_origin : NULL;
        diag_error(err_file, (int)expr->line, (int)expr->column, buf);
        if (s_current_module) s_current_module->had_error = true;
        return;
    }
    /* Distinct user structs (including different generic specializations, which
     * intern to different TypeInfo) are not assignable/convertible. Catch it
     * here so the diagnostic names the .rae site instead of letting the C
     * compiler flag the generated code with a .c line (#414). Types are
     * interned, so pointer inequality after stripping refs IS a real struct
     * mismatch. Names are required non-empty to skip unresolved placeholders. */
    TypeInfo* want_s = sema_strip_ref(expected);
    TypeInfo* got_s = sema_strip_ref(expr->resolved_type);
    if (want_s && got_s && want_s != got_s
        && want_s->kind == TYPE_STRUCT && got_s->kind == TYPE_STRUCT
        && want_s->name.len > 0 && got_s->name.len > 0) {
        char buf[240];
        snprintf(buf, sizeof(buf),
            "cannot convert %.*s to %.*s; they are different types",
            (int)got_s->name.len, got_s->name.data,
            (int)want_s->name.len, want_s->name.data);
        const char* err_file = s_current_decl_origin ? s_current_decl_origin : NULL;
        diag_error(err_file, (int)expr->line, (int)expr->column, buf);
        if (s_current_module) s_current_module->had_error = true;
        return;
    }
    if (expected->kind == TYPE_ANY && expr->resolved_type->kind != TYPE_ANY) {
        AstExpr* box = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
        *box = (AstExpr){.kind = AST_EXPR_BOX, .resolved_type = expected, .line = expr->line, .column = expr->column};
        box->as.unary.operand = expr; *expr_ptr = box;
    } else if (expected->kind != TYPE_ANY && expr->resolved_type->kind == TYPE_ANY) {
        AstExpr* unbox = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
        *unbox = (AstExpr){.kind = AST_EXPR_UNBOX, .resolved_type = expected, .line = expr->line, .column = expr->column};
        unbox->as.unary.operand = expr; *expr_ptr = unbox;
    } else if (expected->kind != TYPE_OPT && expr->resolved_type->kind == TYPE_OPT) {
        if (s_in_if_let_binding) {
            /* An `if let <name>: T = <opt T>` binding: the optional is
             * narrowed here — the name is the unwrapped value inside the
             * success branch. Keep the historical unbox so narrowing works. */
            AstExpr* unbox = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
            *unbox = (AstExpr){.kind = AST_EXPR_UNBOX, .resolved_type = expected,
                               .line = expr->line, .column = expr->column};
            unbox->as.unary.operand = expr;
            *expr_ptr = unbox;
        } else {
            /* An optional value cannot be assigned to a non-optional binding.
             * Consume it with `if let` (and write your own else value) — the
             * compiler never invents a sentinel by silently unwrapping. Uniform
             * hard error for every T: scalar accessors, struct accessors, and
             * user functions that return `opt`. */
            const char* err_file = s_current_decl_origin ? s_current_decl_origin : NULL;
            diag_error(err_file, (int)expr->line, (int)expr->column,
                       "optional not unwrapped — use `if let`");
            if (s_current_module) s_current_module->had_error = true;
        }
    }
}

// True if `name` is the logical name of an imported module (e.g. "math",
// "filesystem", "gpu") — i.e. a namespace qualifier rather than a value. Used to
// tell `module.func(args)` (qualified call) apart from `value.method(args)`
// (UFCS). See docs/module-namespacing.md.
static bool sema_is_module_name(AstModule* module, Str name) {
    for (AstDecl* d = module->decls; d; d = d->next) {
        if (d->module_name && str_eq_cstr(name, d->module_name)) return true;
        // A project folder name (`enemies`) also qualifies, so `enemies.tick()`
        // resolves with no import/open. (docs/module-namespacing.md)
        char ns[256]; sema_project_namespace(d, ns, sizeof ns);
        if (ns[0] != '\0' && str_eq_cstr(name, ns)) return true;
    }
    for (const AstImport* imp = module->imports; imp; imp = imp->next) {
        if (!imp->module) continue;
        for (AstDecl* d = imp->module->decls; d; d = d->next) {
            if (d->module_name && str_eq_cstr(name, d->module_name)) return true;
        }
    }
    return false;
}

// Find a module-level `const`/`let` decl named `name` in module `modname`
// (checking both this module's own decls and its imports). Backs
// module-qualified value access `modname.name` (e.g. `keys.keyW`) — the const
// counterpart of resolve_qualified_function. Returns NULL if there is no such
// global, so the caller falls back to ordinary struct-field handling.
static AstDecl* sema_find_module_global(AstModule* module, Str modname, Str name) {
    for (AstDecl* d = module->decls; d; d = d->next) {
        if (d->kind == AST_DECL_GLOBAL_LET && d->module_name
            && str_eq_cstr(modname, d->module_name)
            && str_eq(d->as.let_decl.name, name)) return d;
    }
    for (const AstImport* imp = module->imports; imp; imp = imp->next) {
        if (!imp->module) continue;
        for (AstDecl* d = imp->module->decls; d; d = d->next) {
            if (d->kind == AST_DECL_GLOBAL_LET && d->module_name
                && str_eq_cstr(modname, d->module_name)
                && str_eq(d->as.let_decl.name, name)) return d;
        }
    }
    return NULL;
}

static char sema_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// A near-miss name for the "did you mean" suggestion: `typed` is a case-
// insensitive PREFIX or SUFFIX of `candidate`. This matches the camelCase
// accessor rename (`at` -> `copyAt`/`viewAt`/`modAt`, all suffix `at`) and
// prefix typos (`ad` -> `add`) without flagging unrelated names that merely
// contain the letters (`at` inside `truncate`).
static bool sema_name_near(Str candidate, Str typed) {
    if (typed.len == 0 || typed.len > candidate.len) return false;
    bool is_prefix = true, is_suffix = true;
    for (size_t i = 0; i < typed.len; i++) {
        if (sema_lower(candidate.data[i]) != sema_lower(typed.data[i])) is_prefix = false;
        if (sema_lower(candidate.data[candidate.len - typed.len + i]) != sema_lower(typed.data[i])) is_suffix = false;
    }
    return is_prefix || is_suffix;
}

// The template base name of a type: "List" for a specialized `List_int64_t`,
// the plain name otherwise. Method receivers are declared against the template
// (`this: mod List(T)`), so receiver matching must compare template to template.
static Str sema_struct_template_name(const TypeInfo* type) {
    if (!type) return (Str){0};
    if (type->kind == TYPE_STRUCT && type->as.structure.decl
        && type->as.structure.decl->kind == AST_DECL_TYPE) {
        AstDecl* tmpl = type->as.structure.decl->as.type_decl.generic_template;
        return (tmpl && tmpl->kind == AST_DECL_TYPE)
                   ? tmpl->as.type_decl.name
                   : type->as.structure.decl->as.type_decl.name;
    }
    return type->name;
}

// Render a receiver type for a diagnostic, e.g. "List(Int)". Uses the template
// base name (not the mangled `List_int64_t`) and appends resolved generic
// arguments when the base does not already carry them.
static void sema_receiver_display(const TypeInfo* type, char* buf, size_t cap) {
    if (!type || type->name.len == 0) { snprintf(buf, cap, "the receiver"); return; }
    Str base = sema_struct_template_name(type);
    bool already_generic = memchr(base.data, '(', base.len) != NULL;
    if (!already_generic && type->kind == TYPE_STRUCT
        && type->as.structure.generic_count > 0) {
        size_t pos = 0;
        int n = snprintf(buf, cap, "%.*s(", (int)base.len, base.data);
        pos += n > 0 ? (size_t)n : 0;
        for (size_t i = 0; i < type->as.structure.generic_count && pos < cap; i++) {
            const TypeInfo* arg = type->as.structure.generic_args[i];
            const char* an = (arg && arg->name.len) ? arg->name.data : "?";
            int al = (arg && arg->name.len) ? (int)arg->name.len : 1;
            int m = snprintf(buf + pos, cap - pos, "%s%.*s", i ? ", " : "", al, an);
            pos += m > 0 ? (size_t)m : 0;
        }
        if (pos < cap) snprintf(buf + pos, cap - pos, ")");
    } else {
        snprintf(buf, cap, "%.*s", (int)type->name.len, type->name.data);
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
            /* Arithmetic on references yields a VALUE, not a reference. Taking
             * the lhs type verbatim made `a - b` inherit `view Float64` when a
             * was a `view` parameter, which then read as a non-numeric type:
             * `(a - b) as Float` was rejected as an unsupported conversion,
             * while the identical expression over locals compiled. */
            else if (expr->as.binary.lhs && expr->as.binary.lhs->resolved_type) expr->resolved_type = sema_strip_ref(expr->as.binary.lhs->resolved_type);
            break;
        case AST_EXPR_CAST: {
            sema_analyze_expr(ctx, module, symbols, expr->as.cast.operand);
            TypeInfo* target = sema_resolve_type_internal(ctx, module, symbols, expr->as.cast.target);
            expr->resolved_type = target;
            /* Strip refs: casting a value read through `view`/`mod` is a cast
             * of the value. Without this, `now as Float` on a `view Float64`
             * parameter was rejected outright. */
            TypeInfo* src = expr->as.cast.operand ? sema_strip_ref(expr->as.cast.operand->resolved_type) : NULL;
            if (target && src) {
                /* `as` converts numeric representations. It is not a
                 * type-system escape hatch: anything else is rejected. */
                bool ok = sema_is_numeric_kind(target->kind) && sema_is_numeric_kind(src->kind);
                if (!ok && target->kind == src->kind) ok = true; /* Float as Float32: same type, no-op */
                if (!ok) {
                    /* Enums are int-backed, so `index as ClipKind` is valid — the
                     * C cast is a no-op (both are int64_t); an out-of-range Int
                     * simply matches no `case` and toString()s to "". Enums have
                     * no dedicated TypeInfo (they resolve to Void in sema), so
                     * detect the target enum from its syntactic type ref. */
                    if (sema_typeref_is_enum(symbols, expr->as.cast.target)
                        && sema_is_numeric_kind(src->kind)) ok = true;
                }
                if (!ok) {
                    const char* cf = s_current_decl_origin ? s_current_decl_origin : NULL;
                    diag_error(cf, (int)expr->line, (int)expr->column,
                               "unsupported conversion: `as` converts between numeric types");
                    if (s_current_module) s_current_module->had_error = true;
                }
            }
            break;
        }
        case AST_EXPR_UNARY:
            sema_analyze_expr(ctx, module, symbols, expr->as.unary.operand);
            if (expr->as.unary.op == AST_UNARY_NOT) expr->resolved_type = type_get_bool(ctx->type_registry);
            else if (expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
                if (expr->as.unary.operand->resolved_type) expr->resolved_type = type_get_ref(ctx->type_registry, expr->as.unary.operand->resolved_type, expr->as.unary.op == AST_UNARY_MOD);
            } else if (expr->as.unary.op == AST_UNARY_SPAWN) {
                // `spawn f()` evaluates to a Task(T) where T is f's return
                // type. Reading the result is the explicit `task.get()`. A
                // void-returning callee still yields a Task (Task(void)) so a
                // bare `spawn f()` statement is recognised and joined on drop.
                TypeInfo* inner = expr->as.unary.operand->resolved_type;
                if (!inner) inner = type_get_void(ctx->type_registry);
                expr->resolved_type = type_get_task(ctx->type_registry, inner);
                // Spawn-boundary capture safety. OP_SPAWN shallow-moves each
                // argument onto the new thread, so a borrow (view/mod) of
                // heap data would alias the parent across threads. Borrowed
                // params are only safe for value types with no heap payload:
                // scalars (Int/Float/Bool/Char) and enums (tagged integers),
                // which the VM treats as value types. String/List/Map/struct/
                // Buffer borrows are rejected — pass them as own or copy.
                AstExpr* sp_call = expr->as.unary.operand;
                if (sp_call && sp_call->kind == AST_EXPR_CALL && sp_call->decl_link &&
                    sp_call->decl_link->kind == AST_DECL_FUNC) {
                    for (AstParam* p = sp_call->decl_link->as.func_decl.params; p; p = p->next) {
                        if (!p->type || !(p->type->is_view || p->type->is_mod)) continue;
                        TypeInfo* bt = sema_resolve_type_internal(ctx, module, symbols, p->type);
                        if (bt && bt->kind == TYPE_REF) bt = bt->as.ref.base;
                        bool scalar = bt && (bt->kind == TYPE_INT || bt->kind == TYPE_FLOAT ||
                                             bt->kind == TYPE_BOOL || bt->kind == TYPE_CHAR);
                        // Enums are value types (no heap), so a borrowed enum is
                        // as safe as a borrowed scalar. Detect by the param's
                        // type name resolving to an enum declaration.
                        bool is_enum = false;
                        if (!scalar && p->type->parts && p->type->parts->text.len > 0) {
                            Symbol* tsym = symbol_table_lookup(symbols, p->type->parts->text);
                            if (tsym && tsym->decl && tsym->decl->kind == AST_DECL_ENUM) is_enum = true;
                        }
                        if (!scalar && !is_enum) {
                            char buf[256];
                            snprintf(buf, sizeof(buf),
                                "cannot spawn: parameter '%.*s' is a %s of non-scalar data, "
                                "which would be shared across the task boundary; pass it as own or copy",
                                (int)p->name.len, p->name.data, p->type->is_mod ? "mod" : "view");
                            diag_error(module->file_path, (int)expr->line, (int)expr->column, buf);
                            module->had_error = true;
                        }
                    }
                }
            } else if (expr->as.unary.operand->resolved_type) expr->resolved_type = expr->as.unary.operand->resolved_type;
            break;
        case AST_EXPR_CALL: {
            sema_analyze_expr(ctx, module, symbols, expr->as.call.callee);
            AstCallArg* arg = expr->as.call.args;
            while (arg) { sema_analyze_expr(ctx, module, symbols, arg->value); arg = arg->next; }
            if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
                Str name = expr->as.call.callee->as.ident;
                if (str_eq_cstr(name, "sizeof")) { expr->is_builtin_sizeof = true; expr->resolved_type = type_get_int(ctx->type_registry); }
                /* `Array(Float, cap: 16)` doubles as a zero-initialising
                 * constructor, so the type application and the value share
                 * one spelling. That is what makes replacing a
                 * `createList(T, cap: N)` line with an Array a one-token
                 * edit instead of a rewrite. */
                else if (str_eq_cstr(name, "Array")) { expr->resolved_type = sema_array_type_from_call(ctx, module, symbols, expr); }
                else {
                    AstDecl* resolved = resolve_function_overload(ctx, module, symbols, name, expr->as.call.args, expr->as.call.generic_args, expr->line, expr->column);
                    if (resolved) {
                        expr->decl_link = resolved;
                        if (resolved->as.func_decl.returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, resolved->as.func_decl.returns->type);
                        AstParam* p = resolved->as.func_decl.params; AstCallArg* a = expr->as.call.args;
                        while (p && a) { TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type); ensure_type_match(ctx, pt, &a->value); p = p->next; a = a->next; }
                        sema_check_own_args(ctx, module, symbols, &resolved->as.func_decl, expr->as.call.args, false);
                        /* If this call hands back a reference, that reference
                         * may point into any of its `view`/`mod` arguments. A
                         * temporary argument dies at the end of this
                         * statement, so the returned reference would outlive
                         * what it names.
                         *
                         * Note the rule is about the RETURNED reference, not
                         * about temporaries as such: passing `{ x: 1 }` to a
                         * `view Point` parameter of a value-returning function
                         * is fine, because the temporary outlives the call. */
                        if (resolved->as.func_decl.returns
                            && resolved->as.func_decl.returns->type
                            && (resolved->as.func_decl.returns->type->is_view
                                || resolved->as.func_decl.returns->type->is_mod)) {
                            AstParam* rp = resolved->as.func_decl.params;
                            AstCallArg* ra = expr->as.call.args;
                            while (rp && ra) {
                                AstExpr* av = ra->value;
                                if (av && av->kind == AST_EXPR_UNARY
                                    && (av->as.unary.op == AST_UNARY_VIEW
                                        || av->as.unary.op == AST_UNARY_MOD)) {
                                    av = av->as.unary.operand;
                                }
                                if (rp->type && (rp->type->is_view || rp->type->is_mod)
                                    && sema_expr_is_temporary(av)) {
                                    diag_report(module ? module->file_path : NULL,
                                                (int)ra->value->line, (int)ra->value->column,
                                                "cannot take reference to a temporary literal");
                                    if (module) module->had_error = true;
                                }
                                rp = rp->next; ra = ra->next;
                            }
                        }
                        // Forbid raw rae_ext_rae_buf_set with a cascade-drop V
                        // outside of stdlib. Stdlib (lib/core.rae and friends)
                        // pairs each raw set with a rae_ext_rae_buf_drop_at or
                        // knows the slot is freshly initialised; user code
                        // can't be trusted with the slot-drop contract.
                        // The "is in stdlib?" check uses the *enclosing*
                        // function's origin_file, not module->file_path,
                        // because the merged AstModule only remembers one
                        // file but the call may live inside a stdlib body
                        // (including a specialization of a stdlib generic).
                        if (str_eq_cstr(name, "rae_ext_rae_buf_set")) {
                            const char* origin = s_current_decl_origin;
                            // Match both absolute paths ("/.../lib/...") and
                            // relative paths starting with "lib/". The latter
                            // is what shows up when the compiler is launched
                            // from the project root by build tooling rather
                            // than via realpath.
                            bool in_stdlib = origin && (
                                strstr(origin, "/lib/") != NULL
                                || strncmp(origin, "lib/", 4) == 0);
                            if (!in_stdlib) {
                                AstParam* vp = resolved->as.func_decl.params;
                                if (vp) vp = vp->next;
                                if (vp) vp = vp->next;
                                if (vp && vp->type
                                    && type_needs_cascade_drop(ctx, module, vp->type, 0)) {
                                    Str vbase = (vp->type->parts) ? vp->type->parts->text : (Str){"<unknown>", 9};
                                    char buffer[320];
                                    snprintf(buffer, sizeof(buffer),
                                        "raw rae_ext_rae_buf_set with cascade-drop element type '%.*s' is not allowed in user code (it would leak the existing slot's owned fields); use a stdlib helper like List.set / StringMap.set instead",
                                        (int)vbase.len, vbase.data);
                                    const char* err_file = origin ? origin : (module ? module->file_path : NULL);
                                    diag_error(err_file, (int)expr->line, (int)expr->column, buffer);
                                    if (module) module->had_error = true;
                                }
                            }
                        }
                    } else {
                        /* Unresolved call. If an argument is a raw
                         * `Array(T, cap: N)`, that is almost certainly the
                         * known gap (#361) rather than a typo: the same
                         * spelling is both a type and a value, and a
                         * `T: type` parameter slot cannot currently take the
                         * value form. Say so, and name the fix, instead of
                         * letting it fall through to a wall of C compiler
                         * errors from the generated file. */
                        for (AstCallArg* ca = expr->as.call.args; ca; ca = ca->next) {
                            if (ca->value && ca->value->kind == AST_EXPR_CALL
                                && ca->value->as.call.callee
                                && ca->value->as.call.callee->kind == AST_EXPR_IDENT
                                && str_eq_cstr(ca->value->as.call.callee->as.ident, "Array")) {
                                diag_error(module ? module->file_path : NULL,
                                           (int)expr->line, (int)expr->column,
                                           "a bare 'Array(T, cap: N)' cannot be used as a type argument here; "
                                           "wrap it in a named type (e.g. `type Mat4 { m: Array(Float, cap: 16) }`) "
                                           "and pass that instead");
                                if (module) module->had_error = true;
                                break;
                            }
                        }
                    }
                }
            }
            break;
        }
        case AST_EXPR_MEMBER:
            // Module-qualified value access: `keys.keyW`, where `keys` is an
            // imported module and `keyW` a module-level const. Mirrors the
            // module.func() path above — rewrite to a plain IDENT bound to the
            // const decl, so codegen emits the flat global and the type is
            // known. A local binding that shadows the module name wins (it's a
            // value), so only take this path when no symbol is in scope.
            if (expr->as.member.object->kind == AST_EXPR_IDENT) {
                Str lhs = expr->as.member.object->as.ident;
                // A genuine VALUE binding named `lhs` (a local/param/global
                // variable or const) shadows the module and wins. A FUNCTION
                // of the same name (e.g. core's `keys()` on a map) does NOT —
                // `keys.keyW` cannot mean a call — so it must not block the
                // module-qualified reading. Symbol carries its origin decl.
                Symbol* lsym = symbol_table_lookup(symbols, lhs);
                bool value_shadow = lsym && !(lsym->decl && lsym->decl->kind == AST_DECL_FUNC);
                Str modname = (Str){0};
                if (!value_shadow && sema_is_module_name(module, lhs)) {
                    modname = lhs;
                } else if (!value_shadow) {
                    Str aliased = sema_resolve_alias(s_current_decl_origin, lhs);
                    if (aliased.data && sema_is_module_name(module, aliased)) modname = aliased;
                }
                if (modname.data) {
                    AstDecl* gd = sema_find_module_global(module, modname, expr->as.member.member);
                    if (gd) {
                        Str cname = expr->as.member.member;  // read before the union write
                        expr->kind = AST_EXPR_IDENT;
                        expr->as.ident = cname;
                        expr->decl_link = gd;
                        expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, gd->as.let_decl.type);
                        break;
                    }
                }
            }
            sema_analyze_expr(ctx, module, symbols, expr->as.member.object);
            if (expr->as.member.object->resolved_type) {
                TypeInfo* t = expr->as.member.object->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                if (t->kind == TYPE_STRUCT) {
                    AstDecl* d = t->as.structure.decl; AstTypeDecl* td = &d->as.type_decl;
                    AstIdentifierPart* params = td->generic_params; 
                    if (!params && d->as.type_decl.generic_template) params = d->as.type_decl.generic_template->as.type_decl.generic_params;
                    
                    // Rebuild AstTypeRef from concrete TypeInfo generic args
                    AstTypeRef* concrete_args = NULL; AstTypeRef* last_ca = NULL;
                    for (size_t i = 0; i < t->as.structure.generic_count; i++) {
                        AstTypeRef* ca = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                        ca->resolved_type = t->as.structure.generic_args[i]; ca->next = NULL;
                        ca->parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
                        ca->parts->text = t->as.structure.generic_args[i]->name;
                        if (!concrete_args) concrete_args = ca; else last_ca->next = ca;
                        last_ca = ca;
                    }

                    AstTypeField* f = td->fields;
                    while (f) {
                        if (str_eq(f->name, expr->as.member.member)) {
                            AstTypeRef* sub = substitute_type_ref(ctx, params, concrete_args, f->type);
                            expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, sub);
                            break;
                        }
                        f = f->next;
                    }
                }
            }
            break;
        case AST_EXPR_METHOD_CALL:
            // Namespace-qualified stdlib call: `module.func(args)`. If the LHS is
            // a bare identifier that is NOT a value in scope but IS an imported
            // module name, this is a qualified call, not UFCS — rewrite it to a
            // plain call of `func` (no receiver prepended) and resolve normally.
            // A local binding that shadows a module name wins (it's a value), so
            // we only take this path when the lookup finds no symbol.
            // (docs/module-namespacing.md)
            if (expr->as.method_call.object->kind == AST_EXPR_IDENT
                && !symbol_table_lookup(symbols, expr->as.method_call.object->as.ident)) {
                Str lhs = expr->as.method_call.object->as.ident;
                // The LHS is a namespace qualifier if it is a module name, or an
                // `import/open X as lhs` alias resolving to module X. (Aliases are
                // per-file; auto-loaded modules need no directive.)
                Str modname = (Str){0};
                if (sema_is_module_name(module, lhs)) {
                    modname = lhs;
                } else {
                    Str aliased = sema_resolve_alias(s_current_decl_origin, lhs);
                    if (aliased.data && sema_is_module_name(module, aliased)) modname = aliased;
                }
                if (modname.data) {
                Str fname = expr->as.method_call.method_name;
                AstCallArg* qargs = expr->as.method_call.args;
                AstTypeRef* qgen = expr->as.method_call.generic_args;
                // Analyze args first so the resolver can use their types.
                for (AstCallArg* a = qargs; a; a = a->next) sema_analyze_expr(ctx, module, symbols, a->value);
                AstDecl* resolved = resolve_qualified_function(ctx, module, symbols, modname, fname, qargs);
                // Rewrite to a plain call (no receiver) bound directly to the
                // resolved in-module decl — codegen emits from decl_link, so this
                // survives non-core stdlib losing its flat symbols (step 3).
                AstExpr* callee = arena_alloc(ctx->ast_arena, sizeof(AstExpr));
                memset(callee, 0, sizeof(*callee));
                callee->kind = AST_EXPR_IDENT;
                callee->as.ident = fname;
                callee->line = expr->line;
                callee->column = expr->column;
                callee->decl_link = resolved;
                expr->kind = AST_EXPR_CALL;
                expr->as.call.callee = callee;
                expr->as.call.args = qargs;
                expr->as.call.generic_args = qgen;
                expr->decl_link = resolved;
                if (resolved && resolved->as.func_decl.returns) {
                    expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, resolved->as.func_decl.returns->type);
                    AstParam* p = resolved->as.func_decl.params;
                    AstCallArg* a = qargs;
                    while (p && a) { TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type); ensure_type_match(ctx, pt, &a->value); p = p->next; a = a->next; }
                    /* A UFCS/qualified method call is REWRITTEN into a plain
                     * CALL here, receiver included as the first argument —
                     * so it reaches neither the ordinary call hook (which
                     * runs under resolve_function_overload) nor the
                     * method-call one. `xs.add(value: text)` arrives here. */
                    sema_check_own_args(ctx, module, symbols, &resolved->as.func_decl, qargs, false);
                }
                break;
                } // if (modname.data)
            }
            sema_analyze_expr(ctx, module, symbols, expr->as.method_call.object);
            AstCallArg* marg = expr->as.method_call.args;
            while (marg) { sema_analyze_expr(ctx, module, symbols, marg->value); marg = marg->next; }
            if (expr->as.method_call.object->resolved_type) {
                TypeInfo* t = expr->as.method_call.object->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                // Built-in Task(T).get() : T. Waits for the task and yields
                // its result. No user decl_link — the compiler emits
                // OP_TASK_GET for a Task receiver with method "get".
                if (t->kind == TYPE_TASK && str_eq_cstr(expr->as.method_call.method_name, "get")) {
                    expr->resolved_type = t->as.task.base;
                    break;
                }
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
                    // First try simple symbol lookup (works for non-overloaded methods)
                    // But verify receiver type matches to avoid wrong overload
                    Symbol* sym = symbol_table_lookup(symbols, expr->as.method_call.method_name);
                    if (sym && sym->decl && sym->decl->kind == AST_DECL_FUNC) {
                        AstFuncDecl* fd = &sym->decl->as.func_decl;
                        if (!fd->generic_params && fd->params) {
                            // Check if this param type matches receiver
                            TypeInfo* param_t = sema_resolve_type_internal(ctx, module, symbols, fd->params->type);
                            TypeInfo* recv_t = expr->as.method_call.object->resolved_type;
                            if (recv_t && recv_t->kind == TYPE_REF) recv_t = recv_t->as.ref.base;
                            if (param_t && param_t->kind == TYPE_REF) param_t = param_t->as.ref.base;
                            if (!recv_t || !param_t || recv_t == param_t) {
                                expr->decl_link = sym->decl;
                                if (fd->returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, fd->returns->type);
                                found = true;
                            }
                        } else if (!fd->generic_params && !fd->params) {
                            expr->decl_link = sym->decl;
                            if (fd->returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, fd->returns->type);
                            found = true;
                        }
                    }
                }
                if (!found) {
                    // Search ALL functions with matching name for generic overload resolution.
                    // Methods like set/get/has exist on multiple types (List, StringMap, IntMap).
                    TypeInfo* rec = expr->as.method_call.object->resolved_type;
                    if (rec && rec->kind == TYPE_REF) rec = rec->as.ref.base;
                    AstTypeRef rec_tr = {0};
                    if (rec) {
                        rec_tr.parts = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
                        rec_tr.parts->text = rec->name;
                        if (rec->kind == TYPE_STRUCT && rec->as.structure.generic_count > 0) {
                            AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
                            for (size_t j = 0; j < rec->as.structure.generic_count; j++) {
                                AstTypeRef* arg_tr = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
                                arg_tr->resolved_type = rec->as.structure.generic_args[j]; arg_tr->next = NULL;
                                if (!head) head = arg_tr; else tail->next = arg_tr; tail = arg_tr;
                            }
                            rec_tr.generic_args = head;
                        }
                    }
                    // Iterate all decls to find the best matching overload for this receiver type
                    AstDecl* best_decl = NULL;
                    for (AstDecl* dd = module->decls; dd; dd = dd->next) {
                        if (dd->kind != AST_DECL_FUNC) continue;
                        if (!str_eq(dd->as.func_decl.name, expr->as.method_call.method_name)) continue;
                        if (dd->as.func_decl.specialization_args) continue; // skip existing specializations
                        AstFuncDecl* fd = &dd->as.func_decl;
                        if (!fd->params || !str_eq_cstr(fd->params->name, "this")) continue;
                        if (fd->generic_params && rec) {
                            AstTypeRef* ga = infer_generic_args(ctx, fd, fd->params->type, &rec_tr);
                            if (ga) { best_decl = dd; break; }
                        } else if (!fd->generic_params && rec) {
                            // Non-generic: check receiver type matches this param
                            TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, fd->params->type);
                            if (pt && pt->kind == TYPE_REF) pt = pt->as.ref.base;
                            if (pt == rec || (pt && rec && pt->kind == rec->kind && str_eq(pt->name, rec->name))) {
                                best_decl = dd; break;
                            }
                        }
                    }
                    // Also search imports
                    if (!best_decl) {
                        for (const AstImport* imp = module->imports; imp && !best_decl; imp = imp->next) {
                            if (!imp->module) continue;
                            for (AstDecl* dd = imp->module->decls; dd; dd = dd->next) {
                                if (dd->kind != AST_DECL_FUNC) continue;
                                if (!str_eq(dd->as.func_decl.name, expr->as.method_call.method_name)) continue;
                                if (dd->as.func_decl.specialization_args) continue;
                                AstFuncDecl* fd = &dd->as.func_decl;
                                if (!fd->params || !str_eq_cstr(fd->params->name, "this")) continue;
                                if (fd->generic_params && rec) {
                                    AstTypeRef* ga = infer_generic_args(ctx, fd, fd->params->type, &rec_tr);
                                    if (ga) { best_decl = dd; break; }
                                } else if (!fd->generic_params && rec) {
                                    TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, fd->params->type);
                                    if (pt && pt->kind == TYPE_REF) pt = pt->as.ref.base;
                                    if (pt == rec || (pt && rec && pt->kind == rec->kind && str_eq(pt->name, rec->name))) {
                                        best_decl = dd; break;
                                    }
                                }
                            }
                        }
                    }
                    if (best_decl) {
                        AstFuncDecl* fd = &best_decl->as.func_decl;
                        if (fd->generic_params && rec) {
                            AstTypeRef* ga = infer_generic_args(ctx, fd, fd->params->type, &rec_tr);
                            if (ga) {
                                TypeInfo* type_args[16]; size_t ac = 0;
                                for (AstTypeRef* tr = ga; tr && ac < 16; tr = tr->next) type_args[ac++] = sema_resolve_type_internal(ctx, module, symbols, tr);
                                AstDecl* spec = type_registry_find_specialization(ctx->type_registry, best_decl, type_args, ac);
                                if (!spec) spec = specialize_decl(ctx, module, symbols, best_decl, type_args, ac, expr->line, expr->column);
                                expr->decl_link = spec;
                            } else {
                                expr->decl_link = best_decl;
                            }
                        } else expr->decl_link = best_decl;
                        if (expr->decl_link->as.func_decl.returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, expr->decl_link->as.func_decl.returns->type);
                        found = true;
                    }
                }
                if (!found) {
                    // General UFCS: value.func(args) -> func(value, args) for ANY
                    // visible function whose first parameter type accepts the
                    // receiver — not just `this`-named methods. This lets a
                    // namespaced function be called UFCS-style (angle.sin(),
                    // path.exists()). If matches come from more than one module it
                    // is ambiguous -> error; use the qualified form to pick.
                    // (docs/module-namespacing.md)
                    TypeInfo* urec = expr->as.method_call.object->resolved_type;
                    if (urec && urec->kind == TYPE_REF) urec = urec->as.ref.base;
                    size_t want = 1;
                    for (AstCallArg* a = expr->as.method_call.args; a; a = a->next) want++;
                    AstDecl* ufcs_match = NULL;
                    const char* ufcs_mod = NULL;
                    bool ufcs_ambiguous = false;
                    for (AstDecl* dd = module->decls; dd; dd = dd->next) {
                        if (dd->kind != AST_DECL_FUNC) continue;
                        AstFuncDecl* fd = &dd->as.func_decl;
                        if (fd->generic_params || fd->specialization_args || !fd->params) continue;
                        if (!str_eq(fd->name, expr->as.method_call.method_name)) continue;
                        if (!sema_decl_visible(s_current_decl_origin, dd)) continue;  // module must be visible here
                        size_t pc = 0; for (AstParam* p = fd->params; p; p = p->next) pc++;
                        if (pc != want) continue;
                        TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, fd->params->type);
                        if (pt && pt->kind == TYPE_REF) pt = pt->as.ref.base;
                        bool ok = !urec || !pt || pt->kind == TYPE_ANY || urec->kind == TYPE_ANY
                                  || pt == urec
                                  || (pt->kind == urec->kind && (pt->kind != TYPE_STRUCT || str_eq(pt->name, urec->name)));
                        if (!ok) continue;
                        if (!ufcs_match) { ufcs_match = dd; ufcs_mod = dd->module_name; }
                        else if (ufcs_match != dd) {
                            bool same_mod = (ufcs_mod && dd->module_name && strcmp(ufcs_mod, dd->module_name) == 0) || (!ufcs_mod && !dd->module_name);
                            if (!same_mod) ufcs_ambiguous = true;
                        }
                    }
                    if (ufcs_ambiguous) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "ambiguous UFCS call '.%.*s(...)' matches functions in multiple modules; use the qualified form (module.%.*s(...))",
                                 (int)expr->as.method_call.method_name.len, expr->as.method_call.method_name.data,
                                 (int)expr->as.method_call.method_name.len, expr->as.method_call.method_name.data);
                        diag_error(module->file_path, (int)expr->line, (int)expr->column, buf);
                    } else if (ufcs_match) {
                        expr->decl_link = ufcs_match;
                        if (ufcs_match->as.func_decl.returns) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, ufcs_match->as.func_decl.returns->type);
                        found = true;
                    }
                }
                if (!found && !expr->decl_link) {
                    // #657: no method resolved for this receiver. sema's method
                    // matching intentionally leaves many valid calls unresolved
                    // (a SPECIALIZED receiver like `List_int64_t` doesn't match a
                    // method's generic template `this: mod List(T)`), so the C
                    // backend is the real resolver. Only report an error when a
                    // same-named `this`-method exists but NONE targets this
                    // receiver's template — the wrong-receiver misresolution the
                    // backend would otherwise bind to a different type, emitting a
                    // confusing downstream C error (e.g. `list.at(index:)` after
                    // `at` was removed from List binds `String.at`). Compiler
                    // intrinsics with no user decl (toString/toJson/fromJson/
                    // Task.get) have no same-named method and reach the backend.
                    TypeInfo* erec = expr->as.method_call.object->resolved_type;
                    if (erec && erec->kind == TYPE_REF) erec = erec->as.ref.base;
                    Str mname = expr->as.method_call.method_name;
                    Str erec_base = sema_struct_template_name(erec);
                    // Only judge a CONCRETELY typed receiver. When the receiver is
                    // itself a call sema left unresolved (backend-resolved), its
                    // type is Void/Unknown — `text.at(index:i).toInt()` types the
                    // inner `.at` as Void — and we cannot know the method is
                    // absent, so defer to the backend rather than false-positive.
                    bool erec_concrete = erec && erec->kind != TYPE_VOID
                        && erec->kind != TYPE_UNKNOWN && erec->kind != TYPE_ANY;
                    if (erec_concrete && erec_base.len > 0) {
                        bool same_name_other_receiver = false; // method mname on a DIFFERENT template
                        bool same_name_this_receiver = false;  // method mname on THIS template
                        Str suggestion = {0};                  // near-miss name on THIS template
                        AstModule* scan_mods[64]; size_t scan_n = 0;
                        scan_mods[scan_n++] = module;
                        for (const AstImport* imp = module->imports; imp && scan_n < 64; imp = imp->next)
                            if (imp->module) scan_mods[scan_n++] = imp->module;
                        for (size_t si = 0; si < scan_n; si++) {
                            for (AstDecl* dd = scan_mods[si]->decls; dd; dd = dd->next) {
                                if (dd->kind != AST_DECL_FUNC) continue;
                                AstFuncDecl* fd = &dd->as.func_decl;
                                if (fd->specialization_args || !fd->params) continue;
                                if (!str_eq_cstr(fd->params->name, "this")) continue;
                                Str recv_base = get_base_type_name(fd->params->type);
                                bool this_receiver = str_eq(recv_base, erec_base);
                                if (str_eq(fd->name, mname)) {
                                    if (this_receiver) same_name_this_receiver = true;
                                    else same_name_other_receiver = true;
                                } else if (this_receiver && !suggestion.len) {
                                    // A near-miss name on the SAME receiver
                                    // (case-insensitive prefix/suffix, e.g.
                                    // at -> copyAt).
                                    if (sema_name_near(fd->name, mname))
                                        suggestion = fd->name;
                                }
                            }
                        }
                        if (same_name_other_receiver && !same_name_this_receiver) {
                            char tbuf[128];
                            sema_receiver_display(erec, tbuf, sizeof tbuf);
                            char buf[256];
                            if (suggestion.len)
                                snprintf(buf, sizeof buf,
                                         "no method `%.*s` on `%s`; did you mean `%.*s`?",
                                         (int)mname.len, mname.data, tbuf,
                                         (int)suggestion.len, suggestion.data);
                            else
                                snprintf(buf, sizeof buf, "no method `%.*s` on `%s`",
                                         (int)mname.len, mname.data, tbuf);
                            diag_error(s_current_decl_origin ? s_current_decl_origin : module->file_path,
                                       (int)expr->line, (int)expr->column, buf);
                            module->had_error = true;
                        }
                    }
                }
                if (found && expr->decl_link) {
                    AstParam* p = expr->decl_link->as.func_decl.params; if (p) p = p->next;
                    AstCallArg* a = expr->as.method_call.args;
                    while (p && a) { TypeInfo* pt = sema_resolve_type_internal(ctx, module, symbols, p->type); ensure_type_match(ctx, pt, &a->value); p = p->next; a = a->next; }
                }
            }
            /* Own-argument rule for method calls — `xs.add(value: text)` is
             * the log_overlay shape. Keyed on decl_link rather than on the
             * `found` flag above, because a method can be bound through
             * several routes (method table, generic overload search, general
             * UFCS) and only some of them set it. The receiver occupies the
             * first parameter, hence skip_receiver. */
            if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) {
                sema_check_own_args(ctx, module, symbols, &expr->decl_link->as.func_decl,
                                    expr->as.method_call.args, true);
            }
            break;
        case AST_EXPR_INDEX:
            sema_analyze_expr(ctx, module, symbols, expr->as.index.target); sema_analyze_expr(ctx, module, symbols, expr->as.index.index);
            if (expr->as.index.target->resolved_type) {
                TypeInfo* t = expr->as.index.target->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                if (t->kind == TYPE_BUFFER) expr->resolved_type = t->as.buffer.base;
                else if (t->kind == TYPE_STRUCT && sema_struct_template_is(t, "List")) {
                    /* Bracket indexing is NOT part of List's API.
                     *
                     * `[]` belongs to Array(T, cap: N) and to the internal
                     * Buffer, where the length is part of the type and a
                     * constant index is checked at compile time. A List's
                     * length is a runtime value, so brackets could never
                     * offer that. List access is explicitly optional through
                     * `at`, `viewAt`, or `modAt`; brackets cannot express the
                     * required handling of absence.
                     *
                     * No Rae code ever used it; it just parsed. */
                    diag_error(s_current_decl_origin ? s_current_decl_origin : (module ? module->file_path : NULL),
                               (int)expr->line, (int)expr->column,
                               "a List is not indexed with '[]'; use '.copyAt(index: i)', '.viewAt(index: i)', "
                               "or '.modAt(index: i)' and handle the optional result. '[]' is for "
                               "Array(T, cap: N), whose length is part of its type");
                    if (module) module->had_error = true;
                    if (t->as.structure.generic_count > 0) expr->resolved_type = t->as.structure.generic_args[0];
                }
                else if (t->kind == TYPE_ARRAY) {
                    expr->resolved_type = t->as.array.base;
                    /* Bounds policy (docs/value-aggregates-and-ownership.md §1.7):
                     * a CONSTANT index is checked at compile time, always, in
                     * every build. Matrix code is full of constant indices
                     * (m[5], m[10]) and verifying them costs nothing, so an
                     * out-of-range constant must never reach runtime. Dynamic
                     * indices are a separate policy — checked in debug,
                     * unchecked in release — and are not handled here. */
                    ConstResult ci = const_eval(symbols, expr->as.index.index);
                    if (ci.ok && ci.numeric && !ci.is_float &&
                        (ci.i < 0 || ci.i >= t->as.array.count)) {
                        char buf[192];
                        snprintf(buf, sizeof buf,
                                 "index %lld is out of bounds for Array(cap: %lld); valid indices are 0..%lld",
                                 (long long)ci.i, (long long)t->as.array.count,
                                 (long long)(t->as.array.count - 1));
                        diag_error(module ? module->file_path : NULL,
                                   (int)expr->line, (int)expr->column, buf);
                        if (module) module->had_error = true;
                    }
                }
            }
            break;
        case AST_EXPR_BOX: sema_analyze_expr(ctx, module, symbols, expr->as.unary.operand); break;
        case AST_EXPR_UNBOX: sema_analyze_expr(ctx, module, symbols, expr->as.unary.operand); break;
        case AST_EXPR_OWN:
            // `own x` ownership-transfer marker. Type is the inner expression's
            // type; sema runs through. Stage 3 of docs/scope-exit-dealloc.md
            // adds the move-tracking + use-after-move check.
            sema_analyze_expr(ctx, module, symbols, expr->as.unary.operand);
            if (expr->as.unary.operand) expr->resolved_type = expr->as.unary.operand->resolved_type;
            break;
        case AST_EXPR_OBJECT: if (expr->as.object_literal.type) expr->resolved_type = sema_resolve_type_internal(ctx, module, symbols, expr->as.object_literal.type); for (AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) sema_analyze_expr(ctx, module, symbols, f->value); break;
        case AST_EXPR_INTERP: {
            AstInterpPart* part = expr->as.interp.parts;
            while (part) {
                if (part->value) sema_analyze_expr(ctx, module, symbols, part->value);
                part = part->next;
            }
            expr->resolved_type = type_get_string(ctx->type_registry);
            break;
        }
        default: break;
    }
}

TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref) { return sema_resolve_type_internal(ctx, NULL, NULL, type_ref); }

/* Fold a VALUE generic argument (`cap: 16`) to a compile-time Int.
 *
 * Reuses the same `const_eval` that backs `const` bindings, so the accepted
 * grammar is identical — literals, earlier constants, enum cases and
 * arithmetic on them — rather than a second, subtly different notion of
 * "compile-time" existing only for type arguments.
 *
 * Returns false and reports a diagnostic if the expression is not a
 * compile-time Int. */
static bool sema_check_value_arg(AstModule* module, ConstResult r, Str arg_name,
                                 size_t line, size_t column, int64_t* out) {
    const char* file = module ? module->file_path : NULL;
    AstTypeRef arg_stack = {0}; arg_stack.value_name = arg_name;
    AstTypeRef* arg = &arg_stack;
    arg->line = line; arg->column = column;
    if (!r.ok || !r.numeric) {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "generic value argument '%.*s' must be a compile-time constant "
                 "(a literal, a 'const' binding, or arithmetic on them)",
                 (int)arg->value_name.len, arg->value_name.data);
        diag_error(file, (int)arg->line, (int)arg->column, buf);
        if (module) module->had_error = true;
        return false;
    }
    if (r.is_float) {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "generic value argument '%.*s' must be an Int, not a Float",
                 (int)arg->value_name.len, arg->value_name.data);
        diag_error(file, (int)arg->line, (int)arg->column, buf);
        if (module) module->had_error = true;
        return false;
    }
    if (r.i < 0) {
        char buf[192];
        snprintf(buf, sizeof buf,
                 "generic value argument '%.*s' must not be negative (got %lld)",
                 (int)arg->value_name.len, arg->value_name.data, (long long)r.i);
        diag_error(file, (int)arg->line, (int)arg->column, buf);
        if (module) module->had_error = true;
        return false;
    }
    *out = (int64_t)r.i;
    return true;
}

static bool sema_resolve_value_arg(AstModule* module, SymbolTable* symbols,
                                   AstTypeRef* arg, int64_t* out) {
    if (arg->value_is_folded) { *out = arg->value_folded; return true; }
    if (!sema_check_value_arg(module, const_eval(symbols, arg->value_expr),
                              arg->value_name, arg->line, arg->column, out)) return false;
    arg->value_folded = *out;
    arg->value_is_folded = true;
    return true;
}

/* Array(T, cap: N) — the built-in fixed-size by-value aggregate.
 *
 * Spelled like a user generic but implemented as a builtin, because no Rae
 * type can express inline N-element storage; that is the feature being added.
 * See docs/value-aggregates-and-ownership.md §1.4. */
static TypeInfo* sema_resolve_array_type(CompilerContext* ctx, AstModule* module,
                                         SymbolTable* symbols, AstTypeRef* type_ref,
                                         TypeInfo* (*resolve)(CompilerContext*, AstModule*, SymbolTable*, AstTypeRef*)) {
    const char* file = module ? module->file_path : NULL;
    AstTypeRef* elem_ref = NULL;
    AstTypeRef* cap_ref = NULL;
    for (AstTypeRef* a = type_ref->generic_args; a; a = a->next) {
        if (a->is_value_arg) {
            if (!str_eq_cstr(a->value_name, "cap")) {
                char buf[256];
                snprintf(buf, sizeof buf, "Array takes a 'cap:' value argument; got '%.*s'",
                         (int)a->value_name.len, a->value_name.data);
                diag_error(file, (int)a->line, (int)a->column, buf);
                if (module) module->had_error = true;
                return NULL;
            }
            if (cap_ref) {
                diag_error(file, (int)a->line, (int)a->column, "Array given 'cap:' more than once");
                if (module) module->had_error = true;
                return NULL;
            }
            cap_ref = a;
        } else {
            if (elem_ref) {
                diag_error(file, (int)a->line, (int)a->column,
                           "Array takes exactly one element type");
                if (module) module->had_error = true;
                return NULL;
            }
            elem_ref = a;
        }
    }
    if (!elem_ref || !cap_ref) {
        diag_error(file, (int)type_ref->line, (int)type_ref->column,
                   "Array type must be written 'Array(T, cap: N)' with an element type and a compile-time cap");
        if (module) module->had_error = true;
        return NULL;
    }

    int64_t count = 0;
    if (!sema_resolve_value_arg(module, symbols, cap_ref, &count)) return NULL;

    TypeInfo* elem = resolve(ctx, module, symbols, elem_ref);
    return type_get_array(ctx->type_registry, elem, count);
}

/* Resolve `Array(T, cap: N)` written in EXPRESSION position — the
 * zero-initialising constructor form. The parser sees an ordinary call, so
 * the element type arrives as an identifier expression rather than a type
 * ref; rebuild the type ref and resolve it through the normal path so the
 * constructor and the type annotation can never disagree.
 *
 * Returns NULL (with a diagnostic) if the call is not a well-formed Array. */
static TypeInfo* sema_array_type_from_call(CompilerContext* ctx, AstModule* module,
                                           SymbolTable* symbols, AstExpr* expr) {
    const char* file = module ? module->file_path : NULL;
    AstCallArg* elem_arg = NULL;
    AstCallArg* cap_arg = NULL;
    for (AstCallArg* a = expr->as.call.args; a; a = a->next) {
        if (a->name.len > 0) {
            if (!str_eq_cstr(a->name, "cap")) {
                char buf[256];
                snprintf(buf, sizeof buf, "Array takes a 'cap:' value argument; got '%.*s'",
                         (int)a->name.len, a->name.data);
                diag_error(file, (int)expr->line, (int)expr->column, buf);
                if (module) module->had_error = true;
                return NULL;
            }
            cap_arg = a;
        } else if (!elem_arg) {
            elem_arg = a;
        }
    }
    if (!elem_arg || !cap_arg) {
        diag_error(file, (int)expr->line, (int)expr->column,
                   "Array constructor must be written 'Array(T, cap: N)' with an element type and a compile-time cap");
        if (module) module->had_error = true;
        return NULL;
    }

    int64_t count = 0;
    if (!sema_check_value_arg(module, const_eval(symbols, cap_arg->value),
                              cap_arg->name, expr->line, expr->column, &count)) return NULL;

    TypeInfo* elem = NULL;
    if (elem_arg->value->kind == AST_EXPR_IDENT) {
        AstTypeRef* tr = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
        memset(tr, 0, sizeof(*tr));
        AstIdentifierPart* part = arena_alloc(ctx->ast_arena, sizeof(AstIdentifierPart));
        memset(part, 0, sizeof(*part));
        part->text = elem_arg->value->as.ident;
        tr->parts = part;
        tr->line = expr->line; tr->column = expr->column;
        elem = sema_resolve_type_internal(ctx, module, symbols, tr);
    } else if (elem_arg->value->kind == AST_EXPR_CALL &&
               elem_arg->value->as.call.callee->kind == AST_EXPR_IDENT &&
               str_eq_cstr(elem_arg->value->as.call.callee->as.ident, "Array")) {
        elem = sema_array_type_from_call(ctx, module, symbols, elem_arg->value);
    }
    if (!elem || elem->kind == TYPE_VOID) {
        diag_error(file, (int)expr->line, (int)expr->column,
                   "Array's first argument must name an element type");
        if (module) module->had_error = true;
        return NULL;
    }
    TypeInfo* arr = type_get_array(ctx->type_registry, elem, count);
    /* Register for typedef emission. `let a = Array(Float, cap: 4)` has no
     * type annotation anywhere, so without this the struct would be used and
     * never declared. Registration is keyed on an AstTypeRef, so synthesize
     * one carrying the resolved type. */
    AstTypeRef* reg = arena_alloc(ctx->ast_arena, sizeof(AstTypeRef));
    memset(reg, 0, sizeof(*reg));
    reg->resolved_type = arr;
    reg->line = expr->line; reg->column = expr->column;
    register_generic_type(ctx, reg);
    return arr;
}


/* ---------------------------------------------------------------------
 * The own-argument rule. docs/value-aggregates-and-ownership.md §2.2.
 *
 * Where a parameter is `own T` and T owns heap storage, the argument must
 * be an OWNING expression. Passing a borrow or a global hands the callee
 * something it will free but does not own — the shape behind log_overlay's
 * double-free and queue #222's dangling global.
 *
 * Checkable from types and expression kinds alone: no dataflow, no new IR.
 * Deliberately does NOT catch use-after-move within a function (§2.3).
 * ------------------------------------------------------------------- */

/* Does this expression produce a value the callee may take ownership of? */
static bool expr_is_owning(SymbolTable* symbols, const AstExpr* e) {
    if (!e) return true;  /* nothing to say; don't invent an error */
    switch (e->kind) {
        /* Fresh values: the callee is the only owner. */
        case AST_EXPR_CALL:
        case AST_EXPR_METHOD_CALL:
        case AST_EXPR_INTERP:
        case AST_EXPR_OBJECT:
        case AST_EXPR_COLLECTION_LITERAL:
        case AST_EXPR_LIST:
        case AST_EXPR_STRING:
            return true;
        /* Explicit transfer — the author has said this is intentional. */
        case AST_EXPR_OWN:
            return true;
        /* A local is owning unless it was bound from something that isn't
         * (§2.4, one hop). A view/mod parameter and a global are marked
         * non-owning where their symbols are created. */
        case AST_EXPR_IDENT: {
            Symbol* sym = symbol_table_lookup(symbols, e->as.ident);
            if (sym && sym->is_non_owning) return false;
            return true;
        }
        /* Reading a field out of a borrow yields storage owned by whatever
         * the borrow points at. */
        case AST_EXPR_MEMBER: {
            const AstExpr* obj = e->as.member.object;
            if (obj && obj->kind == AST_EXPR_IDENT) {
                Symbol* sym = symbol_table_lookup(symbols, obj->as.ident);
                if (sym && sym->is_non_owning) return false;
            }
            return true;
        }
        /* Boxing/unboxing is a representation change, not a transfer. */
        case AST_EXPR_BOX:
        case AST_EXPR_UNBOX:
            return expr_is_owning(symbols, e->as.unary.operand);
        default:
            return true;
    }
}

/* Would handing this expression to `own T` free storage someone else owns?
 * Only fires when T actually owns heap storage — the rule stays invisible
 * in numeric code (open question 3, answered as the narrow reading). */
static void sema_check_own_args(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                                const AstFuncDecl* fd, AstCallArg* args, bool skip_receiver) {
    if (!fd || !module) return;
    const AstParam* p = fd->params;
    AstCallArg* a = args;
    if (skip_receiver && p) p = p->next;
    while (p && a) {
        /* `type_needs_cascade_drop`, not `type_owns_heap_storage`: the
         * latter does not count a bare String (it only reports types whose
         * FIELDS own heap), and String is the single most common `own`
         * parameter. The question here is "would the callee free
         * something?", which is exactly cascade-drop.
         *
         * The param type is also consulted through the ARGUMENT's resolved
         * type, because a generic container's parameter is the
         * unsubstituted `T` — `List(String).add(value: own T)` is the
         * log_overlay shape and `T` alone says nothing about heap. The
         * argument must typecheck against the parameter, so its type is the
         * same type, just already substituted. */
        bool owns_heap = false;
        if (p->type && p->type->is_own) {
            owns_heap = type_needs_cascade_drop(ctx, module, p->type, 0);
            if (!owns_heap && a->value && a->value->resolved_type) {
                AstTypeRef argtr = {0};
                argtr.resolved_type = a->value->resolved_type;
                if (argtr.resolved_type->kind == TYPE_REF) argtr.resolved_type = argtr.resolved_type->as.ref.base;
                owns_heap = type_needs_cascade_drop(ctx, module, &argtr, 0);
            }
        }
        if (p->type && p->type->is_own && owns_heap
            && !expr_is_owning(symbols, a->value)) {
            Str pbase = get_base_type_name(p->type);
            if (a->value && a->value->resolved_type) {
                TypeInfo* at = a->value->resolved_type;
                if (at->kind == TYPE_REF) at = at->as.ref.base;
                /* Report the concrete type; "own T" would not help a reader. */
                if (pbase.len <= 1 && at->name.len > 0) pbase = at->name;
            }
            char buf[320];
            snprintf(buf, sizeof buf,
                     "argument for '%.*s' does not own its value, but the parameter is 'own %.*s'. "
                     "Write '\"{%.*s}\"' (or another expression that produces a fresh value) to give "
                     "the callee its own copy, or 'own <expr>' to transfer ownership deliberately",
                     (int)p->name.len, p->name.data,
                     (int)pbase.len, pbase.data,
                     (int)(a->value && a->value->kind == AST_EXPR_IDENT ? a->value->as.ident.len : 0),
                     (a->value && a->value->kind == AST_EXPR_IDENT ? a->value->as.ident.data : ""));
            /* The merged AstModule remembers one file path, but decls come
             * from many — use the enclosing decl's origin so the location
             * points at the file the reader actually has open. Same reason
             * the rae_ext_rae_buf_set check uses it. */
            const char* own_file = s_current_decl_origin ? s_current_decl_origin : module->file_path;
            diag_error(own_file, (int)a->value->line, (int)a->value->column, buf);
            module->had_error = true;
        }
        p = p->next; a = a->next;
    }
}

// `alias Name = Type` (#647): find the aliased target AstTypeRef for `name` in
// this module or any it imports. Aliases are transparent — `name` resolves to
// the target's TypeInfo, so it is the same type (identical layout, dot-syntax
// methods, no conversions). Returns NULL when `name` is not an alias.
static const AstTypeRef* find_alias_target(const AstModule* module, Str name, int depth) {
    if (!module || depth > 16) return NULL;
    for (const AstDecl* d = module->decls; d; d = d->next) {
        if (d->kind == AST_DECL_ALIAS && str_eq(d->as.alias_decl.name, name))
            return d->as.alias_decl.target;
    }
    for (const AstImport* imp = module->imports; imp; imp = imp->next) {
        if (imp->module) {
            const AstTypeRef* t = find_alias_target(imp->module, name, depth + 1);
            if (t) return t;
        }
    }
    return NULL;
}

static TypeInfo* sema_resolve_type_internal(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstTypeRef* type_ref) {
    if (!type_ref) return type_get_void(ctx->type_registry);
    if (type_ref->resolved_type) return type_ref->resolved_type;
    TypeInfo* base = NULL;
    if (type_ref->parts) {
        // Type alias (#647): rewrite a bare alias name to its canonical target
        // IN PLACE, before any name-based resolution — so not only sema but also
        // the mangler and C-type emission (which read `parts->text`, not
        // `resolved_type`) see the canonical type. The use's own
        // is_view/is_mod/is_opt flags on this ref are preserved. The loop
        // resolves a short alias-of-alias chain; the counter guards `alias A = A`.
        for (int alias_hops = 0; alias_hops < 16
                 && !type_ref->parts->next && !type_ref->generic_args; alias_hops++) {
            const AstTypeRef* alias_target = find_alias_target(module, type_ref->parts->text, 0);
            if (!alias_target || !alias_target->parts) break;
            type_ref->parts = clone_parts(ctx, alias_target->parts);
            type_ref->generic_args = alias_target->generic_args
                ? clone_type_ref(ctx->ast_arena, alias_target->generic_args) : NULL;
        }
        Str name = type_ref->parts->text;
        // `any` (#772) is the compile-time type wildcard. It is meaningful ONLY
        // as a field-loop binding pattern (`loop let x: mod ComponentTable(any)
        // in fields(world)`), where sema matches it structurally and never
        // resolves it as a type. Reaching real type resolution means it was
        // written somewhere it has no meaning — a parameter type, a field type,
        // a `createList(any)` — so reject it with a pointed message. Note the
        // capital-`Any` runtime box resolves normally just below.
        if (str_eq_cstr(name, "any")) {
            diag_error(module->file_path, (int)type_ref->line, (int)type_ref->column,
                       "'any' is the compile-time type wildcard; it is only valid as a "
                       "binding pattern inside a fields() loop (did you mean the runtime box 'Any'?)");
            module->had_error = true;
            return type_get_any(ctx->type_registry);
        }
        if (str_eq_cstr(name, "Int")) base = type_get_int(ctx->type_registry);
        else if (str_eq_cstr(name, "Int64")) base = type_get_int_sized(ctx->type_registry, 64, false);
        else if (str_eq_cstr(name, "Int32")) base = type_get_int_sized(ctx->type_registry, 32, false);
        else if (str_eq_cstr(name, "Int16")) base = type_get_int_sized(ctx->type_registry, 16, false);
        else if (str_eq_cstr(name, "Int8")) base = type_get_int_sized(ctx->type_registry, 8, false);
        else if (str_eq_cstr(name, "UInt64")) base = type_get_int_sized(ctx->type_registry, 64, true);
        else if (str_eq_cstr(name, "UInt32")) base = type_get_int_sized(ctx->type_registry, 32, true);
        else if (str_eq_cstr(name, "UInt16")) base = type_get_int_sized(ctx->type_registry, 16, true);
        else if (str_eq_cstr(name, "UInt8")) base = type_get_int_sized(ctx->type_registry, 8, true);
        /* `Float` and `Float32` deliberately resolve to the SAME TypeInfo:
         * Float is an alias of Float32, not a third float type. Float64 is a
         * distinct type. See docs/primitive-types.md. */
        else if (str_eq_cstr(name, "Float") || str_eq_cstr(name, "Float32")) base = type_get_float(ctx->type_registry);
        else if (str_eq_cstr(name, "Float64")) base = type_get_float64(ctx->type_registry);
        else if (str_eq_cstr(name, "Bool")) base = type_get_bool(ctx->type_registry);
        else if (str_eq_cstr(name, "String")) base = type_get_string(ctx->type_registry);
        else if (str_eq_cstr(name, "Char")) base = type_get_char(ctx->type_registry);
        else if (str_eq_cstr(name, "Any")) base = type_get_any(ctx->type_registry);
        else if (str_eq_cstr(name, "Buffer")) {
            TypeInfo* arg = type_get_void(ctx->type_registry);
            if (type_ref->generic_args) arg = sema_resolve_type_internal(ctx, module, symbols, type_ref->generic_args);
            base = type_get_buffer(ctx->type_registry, arg);
        }
        // `Ptr` is the low-level opaque pointer primitive (general FFI, #497):
        // an untyped void*, modelled as Buffer(void) so it reuses the void*
        // lowering (emits `void*`, zero value NULL) rather than resolving to a
        // phantom `rae_Ptr` struct. It's what generated C-ABI bindings use for
        // handles, callbacks and raw data pointers.
        else if (str_eq_cstr(name, "Ptr")) {
            base = type_get_buffer(ctx->type_registry, type_get_void(ctx->type_registry));
        }
        else if (str_eq_cstr(name, "Array")) {
            base = sema_resolve_array_type(ctx, module, symbols, type_ref, sema_resolve_type_internal);
            if (!base) base = type_get_void(ctx->type_registry);
            else register_generic_type(ctx, type_ref);
        }
        else if (str_eq_cstr(name, "Task")) {
            TypeInfo* arg = type_get_void(ctx->type_registry);
            if (type_ref->generic_args) arg = sema_resolve_type_internal(ctx, module, symbols, type_ref->generic_args);
            base = type_get_task(ctx->type_registry, arg);
        } else if (symbols) {
            Symbol* sym = symbol_table_lookup(symbols, name);
            if (sym && sym->decl && sym->decl->kind == AST_DECL_ENUM) {
                /* Enums are int-backed value types with no payloads. Resolve the
                 * enum name to Int so both cast directions (`intVal as Enum` and
                 * `enumVal as Int`) type-check through the numeric-cast path,
                 * enum-typed values interoperate with Int params/storage, and
                 * arithmetic on ordinals is legal. This does NOT weaken
                 * `match` exhaustiveness: that keys off the case patterns
                 * (`Enum.member`), not the subject's resolved type.
                 * (docs/match-and-sum-types.md) */
                base = type_get_int(ctx->type_registry);
            } else if (sym && sym->type) {
                if (type_ref->generic_args && sym->decl && sym->decl->kind == AST_DECL_TYPE) {
                    /* Value generic arguments are a builtin-only capability for
                     * now (Array is the sole consumer). User generics would need
                     * the value substituted into the specialized body, which is
                     * a larger feature with no caller yet — so reject it clearly
                     * rather than resolving the value as if it were a type. */
                    for (AstTypeRef* a = type_ref->generic_args; a; a = a->next) {
                        if (a->is_value_arg) {
                            char vbuf[256];
                            snprintf(vbuf, sizeof vbuf,
                                     "'%.*s' does not take value generic arguments; only built-in types do",
                                     (int)name.len, name.data);
                            diag_error(module ? module->file_path : NULL, (int)a->line, (int)a->column, vbuf);
                            if (module) module->had_error = true;
                            break;
                        }
                    }
                    TypeInfo* args[16]; size_t ac = 0; AstTypeRef* curr = type_ref->generic_args;
                    while (curr && ac < 16) {
                        if (curr->is_value_arg) { curr = curr->next; continue; }
                        args[ac++] = sema_resolve_type_internal(ctx, module, symbols, curr); curr = curr->next;
                    }
                    base = type_get_struct(ctx->type_registry, sym->decl, args, ac);
                    register_generic_type(ctx, type_ref);
                    if (!type_registry_find_specialization(ctx->type_registry, sym->decl, args, ac)) specialize_decl(ctx, module, symbols, sym->decl, args, ac, type_ref->line, type_ref->column);
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
    s_current_module = module;
    if (!ctx->type_registry) {
        ctx->type_registry = arena_alloc(ctx->ast_arena, sizeof(TypeRegistry));
        type_registry_init(ctx->type_registry, ctx->ast_arena);
    }
    if (!ctx->instantiation_stack) {
        ctx->instantiation_stack = arena_alloc(ctx->ast_arena, sizeof(InstantiationStack));
        ctx->instantiation_stack->head = NULL;
    }
    SymbolTable symbols = {0};
 size_t processed_count = 0; const AstDecl* processed[8192]; memset(processed, 0, sizeof(processed));
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
        if (name.len > 0) {
            Symbol* existing = symbol_table_lookup(&symbols, name);
            if (!existing || (existing->decl && existing->decl->kind == AST_DECL_FUNC && d->kind == AST_DECL_FUNC)) {
                symbol_table_define(&symbols, ctx->ast_arena, name, d, t, false);
            } else {
                // Already defined and not a function overload
            }
        }
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
