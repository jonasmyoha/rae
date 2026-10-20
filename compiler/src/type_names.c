/* Type and enum names belong to their module (docs/module-namespacing.md).
 *
 * Two modules may both declare `type Point`. A type is reached bare in its own
 * module and in the modules the file opens (the project, the prelude), so each
 * module uses its own, and a file that sees both bare must not name it bare:
 * the use is ambiguous, not the declarations.
 *
 * Sema, the registry and the C backend find a type by its name, so before sema
 * runs, every declaration of a clashing name but one gets an INTERNAL name
 * that carries its module (`Point__in_points_Right`), and every use of the
 * written name is pointed at the declaration the using file sees: its own
 * file's, else the one visible bare (an explicit `open` picks between two
 * project modules), else the one it imports. The declaration that keeps the
 * written name is a lib one when there is one, so a type the compiler knows by
 * name (`List`, `ComponentTable`) keeps it. An internal name cannot be written
 * in source (a type name has no `_`), and type_written_name gives the written
 * name back for reflection, JSON and messages.
 *
 * It also resolves `Module.Type` (a type reached through an imported module):
 * the reference is pointed at that module's declaration and its qualifier
 * cleared, so sema and the backend see a plain type name. Only clashing names
 * are renamed: a program with no clash and no qualified type is unchanged.
 */
#include <stdio.h>
#include <string.h>

#include "sema.h"
#include "diag.h"

#define INTERNAL_MARK "__in_"

Str type_written_name(Str name) {
    size_t mark = sizeof(INTERNAL_MARK) - 1;
    for (size_t i = 1; i + mark <= name.len; i++) {
        if (memcmp(name.data + i, INTERNAL_MARK, mark) == 0) return (Str){ name.data, i };
    }
    return name;
}

typedef struct {
    Str written;           /* the name as declared */
    bool clashing;         /* declared in two modules or more */
    AstDecl* decls[16];
    Str names[16];         /* the name each declaration carries from here on */
    size_t count;
    size_t kept;           /* the declaration that keeps the written name */
} Clash;

typedef struct {
    CompilerContext* ctx;
    AstModule* module;
    Clash* clashes;
    size_t clash_count;
    const char* file;      /* the file of the declaration being walked */
    const AstDecl* self;   /* that declaration */
} Pass;

static Str get_decl_type_name(const AstDecl* d) {
    if (d->kind == AST_DECL_TYPE) return d->as.type_decl.name;
    if (d->kind == AST_DECL_ENUM) return d->as.enum_decl.name;
    return (Str){0};
}

/* Every declared type name; clash_for only the clashing ones */
static Clash* entry_for(Pass* p, Str name) {
    for (size_t i = 0; i < p->clash_count; i++) if (str_eq(p->clashes[i].written, name)) return &p->clashes[i];
    return NULL;
}

static Clash* clash_for(Pass* p, Str name) {
    Clash* c = entry_for(p, name);
    return c && c->clashing ? c : NULL;
}

/* `Q.T`: the declaration of T that the file reaches through module Q. Sets
 * *name to that declaration's name; false when Q has no such type. */
static bool resolve_qualified(Pass* p, Str qualifier, Str* name) {
    Clash* c = entry_for(p, *name);
    if (!c) return false;
    for (size_t i = 0; i < c->count; i++) {
        if (sema_qualifier_reaches(p->file, qualifier, c->decls[i])) { *name = c->names[i]; return true; }
    }
    return false;
}

static Str internal_name(Pass* p, Str written, const AstDecl* d) {
    const char* module = d->module_name ? d->module_name : (d->origin_file ? d->origin_file : "unknown");
    size_t module_len = strlen(module);
    size_t len = written.len + (sizeof(INTERNAL_MARK) - 1) + module_len;
    char* buf = arena_alloc(p->ctx->ast_arena, len + 1);
    memcpy(buf, written.data, written.len);
    memcpy(buf + written.len, INTERNAL_MARK, sizeof(INTERNAL_MARK) - 1);
    char* out = buf + written.len + sizeof(INTERNAL_MARK) - 1;
    for (size_t i = 0; i < module_len; i++) {
        char c = module[i];
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        out[i] = keep ? c : '_';
    }
    buf[len] = '\0';
    return (Str){ buf, len };
}

static const char* module_label(const AstDecl* d) {
    return sema_decl_in_lib(d) ? (d->module_name ? d->module_name : d->origin_file)
                               : (d->module_name ? d->module_name : "this project");
}

/* The declaration of `c` that a use in p->file means; reports a use that sees
 * two bare. */
static size_t pick(Pass* p, Clash* c, size_t line, size_t column) {
    for (size_t i = 0; i < c->count; i++)
        if (p->file && c->decls[i]->origin_file && strcmp(p->file, c->decls[i]->origin_file) == 0) return i;
    size_t visible[16]; size_t visible_count = 0;
    for (size_t i = 0; i < c->count; i++)
        if (sema_decl_bare_visible(p->file, c->decls[i])) visible[visible_count++] = i;
    /* An explicit `open` picks one of two project modules (as for consts) */
    size_t opened_project = c->count, opened_project_count = 0, project_count = 0;
    for (size_t k = 0; k < visible_count; k++) {
        const AstDecl* d = c->decls[visible[k]];
        if (sema_decl_in_lib(d)) continue;
        project_count++;
        if (sema_decl_opened_explicitly(p->file, d)) { opened_project = visible[k]; opened_project_count++; }
    }
    if (project_count >= 2 && opened_project_count == 1) {
        size_t kept = 0;
        for (size_t k = 0; k < visible_count; k++) {
            const AstDecl* d = c->decls[visible[k]];
            if (!sema_decl_in_lib(d) && visible[k] != opened_project) continue;
            visible[kept++] = visible[k];
        }
        visible_count = kept;
    }
    if (visible_count == 1) return visible[0];
    if (visible_count >= 2) {
        char buf[600];
        snprintf(buf, sizeof buf,
                 "type '%.*s' is ambiguous: it is declared in both '%s' and '%s', which are both open here; "
                 "open only one of them",
                 (int)c->written.len, c->written.data,
                 module_label(c->decls[visible[0]]), module_label(c->decls[visible[1]]));
        diag_error(p->file, (int)line, (int)column, buf);
        p->module->had_error = true;
        return visible[0];
    }
    size_t reachable = c->count; size_t reachable_count = 0;
    for (size_t i = 0; i < c->count; i++)
        if (sema_decl_reachable(p->file, c->decls[i])) { reachable = i; reachable_count++; }
    if (reachable_count == 1) return reachable;
    return c->kept;  /* seen from nowhere: the written name, as before */
}

static void rename_use(Pass* p, Str* name, size_t line, size_t column) {
    if (!name || !name->data) return;
    Clash* c = clash_for(p, *name);
    if (!c) return;
    *name = c->names[pick(p, c, line, column)];
}

static void walk_type(Pass* p, AstTypeRef* t) {
    for (; t; t = t->next) {
        if (t->qualifier.len && t->parts) {
            Str written = t->parts->text;
            if (!resolve_qualified(p, t->qualifier, &t->parts->text)) {
                char buf[400];
                snprintf(buf, sizeof buf,
                         "'%.*s.%.*s': no type '%.*s' in a module this file imports or opens as '%.*s'",
                         (int)t->qualifier.len, t->qualifier.data, (int)written.len, written.data,
                         (int)written.len, written.data, (int)t->qualifier.len, t->qualifier.data);
                diag_error(p->file, (int)t->line, (int)t->column, buf);
                p->module->had_error = true;
            }
            t->qualifier = (Str){0};
        } else if (t->parts && !t->parts->next) {
            rename_use(p, &t->parts->text, t->line, t->column);
        }
        walk_type(p, t->generic_args);
    }
}

static void walk_expr(Pass* p, AstExpr* e);
static void walk_block(Pass* p, AstBlock* b);
static void walk_stmt(Pass* p, AstStmt* s);

static void walk_args(Pass* p, AstCallArg* a) {
    for (; a; a = a->next) walk_expr(p, a->value);
}

/* `X.member`: X is a module qualifier when the file imports or opens a module
 * of that name; then it is not a type use. */
static void walk_qualifier(Pass* p, AstExpr* object) {
    if (object && object->kind == AST_EXPR_IDENT && sema_file_names_module(p->file, object->as.ident)) return;
    walk_expr(p, object);
}

static void walk_expr(Pass* p, AstExpr* e) {
    if (!e) return;
    switch (e->kind) {
        case AST_EXPR_IDENT:
            rename_use(p, &e->as.ident, e->line, e->column);
            break;
        case AST_EXPR_BINARY:
            walk_expr(p, e->as.binary.lhs);
            walk_expr(p, e->as.binary.rhs);
            break;
        case AST_EXPR_UNARY:
        case AST_EXPR_BOX:
        case AST_EXPR_UNBOX:
        case AST_EXPR_OWN:
            walk_expr(p, e->as.unary.operand);
            break;
        case AST_EXPR_CAST:
            walk_expr(p, e->as.cast.operand);
            walk_type(p, e->as.cast.target);
            break;
        case AST_EXPR_CALL:
            if (e->as.call.callee && e->as.call.callee->kind == AST_EXPR_MEMBER)
                walk_expr(p, e->as.call.callee);
            walk_args(p, e->as.call.args);
            walk_type(p, e->as.call.generic_args);
            break;
        case AST_EXPR_MEMBER: {
            /* `Q.T` in an expression (`Device.Pointer.fine`): the type of
             * that name in module Q */
            const AstExpr* object = e->as.member.object;
            Str member = e->as.member.member;
            if (object && object->kind == AST_EXPR_IDENT && entry_for(p, member)
                && sema_file_names_module(p->file, object->as.ident)
                && resolve_qualified(p, object->as.ident, &member)) {
                /* the expression becomes the type's plain name */
                e->kind = AST_EXPR_IDENT;
                e->as.ident = member;
                break;
            }
            walk_qualifier(p, e->as.member.object);
            break;
        }
        case AST_EXPR_OBJECT:
            walk_type(p, e->as.object_literal.type);
            for (AstObjectField* f = e->as.object_literal.fields; f; f = f->next) walk_expr(p, f->value);
            break;
        case AST_EXPR_MATCH:
            walk_expr(p, e->as.match_expr.subject);
            for (AstMatchArm* arm = e->as.match_expr.arms; arm; arm = arm->next) {
                walk_expr(p, arm->pattern);
                walk_expr(p, arm->value);
            }
            break;
        case AST_EXPR_LIST:
            for (AstExprList* it = e->as.list; it; it = it->next) walk_expr(p, it->value);
            break;
        case AST_EXPR_INDEX:
            walk_expr(p, e->as.index.target);
            walk_expr(p, e->as.index.index);
            break;
        case AST_EXPR_METHOD_CALL:
            walk_qualifier(p, e->as.method_call.object);
            walk_args(p, e->as.method_call.args);
            walk_type(p, e->as.method_call.generic_args);
            break;
        case AST_EXPR_COLLECTION_LITERAL:
            walk_type(p, e->as.collection.type);
            for (AstCollectionElement* el = e->as.collection.elements; el; el = el->next) walk_expr(p, el->value);
            break;
        case AST_EXPR_INTERP:
            for (AstInterpPart* part = e->as.interp.parts; part; part = part->next) walk_expr(p, part->value);
            break;
        default:
            break;
    }
}

static void walk_stmt(Pass* p, AstStmt* s) {
    if (!s) return;
    switch (s->kind) {
        case AST_STMT_LET:
            walk_type(p, s->as.let_stmt.type);
            walk_expr(p, s->as.let_stmt.value);
            break;
        case AST_STMT_DESTRUCT:
            walk_expr(p, s->as.destruct_stmt.call);
            break;
        case AST_STMT_EXPR:
            walk_expr(p, s->as.expr_stmt);
            break;
        case AST_STMT_RET:
            for (AstReturnArg* a = s->as.ret_stmt.values; a; a = a->next) walk_expr(p, a->value);
            break;
        case AST_STMT_IF:
            walk_stmt(p, s->as.if_stmt.binding);
            walk_expr(p, s->as.if_stmt.condition);
            walk_block(p, s->as.if_stmt.then_block);
            walk_block(p, s->as.if_stmt.else_block);
            break;
        case AST_STMT_LOOP:
            walk_stmt(p, s->as.loop_stmt.init);
            walk_expr(p, s->as.loop_stmt.condition);
            walk_expr(p, s->as.loop_stmt.increment);
            for (AstQueryLoopBinding* b = s->as.loop_stmt.query_bindings; b; b = b->next) walk_type(p, b->type);
            walk_expr(p, s->as.loop_stmt.query_iterable);
            walk_block(p, s->as.loop_stmt.body);
            break;
        case AST_STMT_MATCH:
            walk_stmt(p, s->as.match_stmt.binding);
            walk_expr(p, s->as.match_stmt.subject);
            for (AstMatchCase* c = s->as.match_stmt.cases; c; c = c->next) {
                walk_expr(p, c->pattern);
                for (AstCasePattern* cp = c->or_patterns; cp; cp = cp->next) walk_expr(p, cp->expr);
                walk_block(p, c->block);
            }
            break;
        case AST_STMT_ASSIGN:
            walk_expr(p, s->as.assign_stmt.target);
            walk_expr(p, s->as.assign_stmt.value);
            break;
        case AST_STMT_DEFER:
            walk_block(p, s->as.defer_stmt.block);
            break;
        case AST_STMT_UNSAFE:
            walk_block(p, s->as.unsafe_stmt.block);
            break;
        default:
            break;
    }
}

static void walk_block(Pass* p, AstBlock* b) {
    if (!b) return;
    for (AstStmt* s = b->first; s; s = s->next) walk_stmt(p, s);
}

static void walk_decl(Pass* p, AstDecl* d) {
    p->file = d->origin_file;
    p->self = d;
    switch (d->kind) {
        case AST_DECL_TYPE:
            for (AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
                walk_type(p, f->type);
                walk_expr(p, f->default_value);
            }
            break;
        case AST_DECL_FUNC:
            for (AstParam* param = d->as.func_decl.params; param; param = param->next) walk_type(p, param->type);
            for (AstReturnItem* r = d->as.func_decl.returns; r; r = r->next) walk_type(p, r->type);
            walk_block(p, d->as.func_decl.body);
            break;
        case AST_DECL_GLOBAL_LET:
            walk_type(p, d->as.let_decl.type);
            walk_expr(p, d->as.let_decl.value);
            break;
        case AST_DECL_ALIAS:
            walk_type(p, d->as.alias_decl.target);
            break;
        default:
            break;
    }
}

void sema_separate_clashing_type_names(CompilerContext* ctx, AstModule* module) {
    Pass p = { .ctx = ctx, .module = module };
    size_t cap = 0;
    for (AstDecl* d = module->decls; d; d = d->next) {
        Str name = get_decl_type_name(d);
        if (!name.len || !d->origin_file) continue;
        if (d->kind == AST_DECL_TYPE && d->as.type_decl.specialization_args) continue;
        Clash* c = entry_for(&p, name);
        if (!c) {
            if (p.clash_count == cap) {
                size_t grown = cap ? cap * 2 : 64;
                Clash* more = arena_alloc(ctx->ast_arena, grown * sizeof(Clash));
                if (cap) memcpy(more, p.clashes, cap * sizeof(Clash));
                p.clashes = more; cap = grown;
            }
            c = &p.clashes[p.clash_count++];
            memset(c, 0, sizeof *c);
            c->written = name;
        }
        /* Two declarations in ONE file stay as they are: sema reports that
         * module's duplicate */
        bool seen = false;
        for (size_t i = 0; i < c->count; i++)
            if (c->decls[i] == d || strcmp(c->decls[i]->origin_file, d->origin_file) == 0) seen = true;
        if (!seen && c->count < 16) c->decls[c->count++] = d;
    }
    for (size_t i = 0; i < p.clash_count; i++) {
        Clash* c = &p.clashes[i];
        c->clashing = c->count >= 2;
        if (!c->clashing) { c->names[0] = c->written; continue; }
        c->kept = 0;
        for (size_t k = 0; k < c->count; k++) if (sema_decl_in_lib(c->decls[k])) { c->kept = k; break; }
        for (size_t k = 0; k < c->count; k++) if (sema_decl_in_prelude(c->decls[k])) { c->kept = k; break; }
        for (size_t k = 0; k < c->count; k++)
            c->names[k] = k == c->kept ? c->written : internal_name(&p, c->written, c->decls[k]);
    }
    /* Every use first (it resolves by the WRITTEN names), then the declarations */
    for (AstDecl* d = module->decls; d; d = d->next) walk_decl(&p, d);
    for (size_t i = 0; i < p.clash_count; i++) {
        Clash* c = &p.clashes[i];
        if (!c->clashing) continue;
        for (size_t k = 0; k < c->count; k++) {
            AstDecl* d = c->decls[k];
            if (d->kind == AST_DECL_TYPE) d->as.type_decl.name = c->names[k];
            else d->as.enum_decl.name = c->names[k];
        }
    }
}
