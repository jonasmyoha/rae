/* sema_results.inc - a function with several labeled results, part of sema.c.
 *
 *   func divide(a: view Int, b: view Int) ret quotient: Int, remainder: Int {
 *     ret quotient: a / b, remainder: a % b
 *   }
 *   let quotient: Int = quotient, let rest: Int = remainder = divide(a: 7, b: 2)
 *
 * Every `ret` gives every result, by its label. The call is taken apart where it
 * is made, each local with its type and the label of the result it takes; it is
 * no value anywhere else. A function with one result is as it was. */

static const AstReturnItem* sema_result_named(const AstReturnItem* results, Str label) {
    for (; results; results = results->next)
        if (results->has_name && str_eq(results->name, label)) return results;
    return NULL;
}

/* "quotient, remainder" */
static void sema_result_labels(const AstReturnItem* results, char* out, size_t cap) {
    size_t pos = 0;
    out[0] = '\0';
    for (const AstReturnItem* r = results; r && pos < cap; r = r->next) {
        int wrote = snprintf(out + pos, cap - pos, "%s%.*s", r == results ? "" : ", ", (int)r->name.len, r->name.data);
        if (wrote > 0) pos += (size_t)wrote;
    }
}

static void sema_results_error(AstModule* module, size_t line, size_t column, const char* message) {
    diag_error(sema_diag_file(module), (int)line, (int)column, message);
    if (module) module->had_error = true;
}

/* The results of `decl` when it has several, checked as a declaration: each has
 * a label of its own and is a value. NULL for one result or none. */
static const AstReturnItem* sema_check_results(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                                               AstDecl* decl) {
    AstFuncDecl* fd = &decl->as.func_decl;
    if (!fd->returns || !fd->returns->next) return NULL;
    char buf[512];
    for (AstReturnItem* r = fd->returns; r; r = r->next) {
        size_t line = r->type ? r->type->line : decl->line;
        size_t column = r->type ? r->type->column : decl->column;
        if (!r->has_name) {
            snprintf(buf, sizeof buf,
                     "each of several results has a label: `ret quotient: Int, remainder: Int`; '%.*s' has one without",
                     (int)fd->name.len, fd->name.data);
            sema_results_error(module, line, column, buf);
            continue;
        }
        for (const AstReturnItem* earlier = fd->returns; earlier != r; earlier = earlier->next) {
            if (!earlier->has_name || !str_eq(earlier->name, r->name)) continue;
            snprintf(buf, sizeof buf, "two results of '%.*s' are labeled '%.*s'; each result has a label of its own",
                     (int)fd->name.len, fd->name.data, (int)r->name.len, r->name.data);
            sema_results_error(module, line, column, buf);
            break;
        }
        if (!r->type) continue;
        sema_resolve_type_internal(ctx, module, symbols, r->type);
        if (r->type->is_view || r->type->is_mod) {
            snprintf(buf, sizeof buf,
                     "result '%.*s' is a reference; each of several results is a value (a function that returns "
                     "a reference has that one result)", (int)r->name.len, r->name.data);
            sema_results_error(module, line, column, buf);
        }
    }
    if (fd->is_extern || fd->c_export_symbol) {
        snprintf(buf, sizeof buf, "'%.*s' is %s: a C function has one result", (int)fd->name.len, fd->name.data,
                 fd->is_extern ? "extern" : "a cExport function");
        sema_results_error(module, decl->line, decl->column, buf);
    }
    return fd->returns;
}

static TypeInfo* sema_generic_result(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstDecl* decl,
                                     TypeInfo* receiver, const AstCallArg* args, AstTypeRef* written, int depth);
static TypeInfo* sema_value_type(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, const AstExpr* value,
                                 int depth);

/* A call of a generic function is left to the backend (no decl_link): what `result`
 * of the generic `decl` is for this call's values, NULL when they do not say */
static TypeInfo* sema_generic_result_type(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                                          const AstDecl* decl, const AstReturnItem* result, const AstExpr* call) {
    AstDecl one = *decl;
    AstReturnItem only = *result;
    only.next = NULL;
    one.as.func_decl.returns = &only;
    if (call->kind == AST_EXPR_CALL)
        return sema_generic_result(ctx, module, symbols, &one, NULL, call->as.call.args, call->as.call.generic_args, 0);
    TypeInfo* receiver = sema_value_type(ctx, module, symbols, call->as.method_call.object, 0);
    while (receiver && receiver->kind == TYPE_REF) receiver = receiver->as.ref.base;
    if (!receiver) return NULL;
    return sema_generic_result(ctx, module, symbols, &one, receiver, call->as.method_call.args,
                               call->as.method_call.generic_args, 0);
}

/* The generic function with several results that such a call means, NULL for none */
static const AstDecl* sema_generic_results_callee(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                                                  const AstExpr* call) {
    Str name = {0};
    if (call->kind == AST_EXPR_CALL && call->as.call.callee && call->as.call.callee->kind == AST_EXPR_IDENT)
        name = call->as.call.callee->as.ident;
    else if (call->kind == AST_EXPR_METHOD_CALL) name = call->as.method_call.method_name;
    if (!name.len) return NULL;
    for (Symbol* curr = symbol_table_first(symbols, name); curr; curr = curr->sameNext) {
        const AstDecl* d = curr->decl;
        if (!d || d->kind != AST_DECL_FUNC || !d->as.func_decl.generic_params || d->as.func_decl.specialization_args
            || !d->as.func_decl.returns || !d->as.func_decl.returns->next) continue;
        if (!sema_package_reaches(s_current_decl_origin, d)) continue;
        if (sema_generic_result_type(ctx, module, symbols, d, d->as.func_decl.returns, call)) return d;
    }
    return NULL;
}

/* `ret quotient: a / b, remainder: a % b` in a function with several results */
static void sema_analyze_ret_results(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt) {
    const AstReturnItem* results = s_current_results;
    char labels[256];
    char buf[640];
    sema_result_labels(results, labels, sizeof labels);
    for (AstReturnArg* arg = stmt->as.ret_stmt.values; arg; arg = arg->next) {
        /* `ret divide(a: a, b: b)`: the results of another function are not passed on */
        bool passes_on = !arg->has_label && arg->value
            && (arg->value->kind == AST_EXPR_CALL || arg->value->kind == AST_EXPR_METHOD_CALL);
        const AstExpr* saved_call = s_destructured_call;
        if (passes_on) s_destructured_call = arg->value;
        if (arg->value) sema_analyze_expr(ctx, module, symbols, arg->value, true);
        s_destructured_call = saved_call;
        size_t line = arg->value ? arg->value->line : stmt->line;
        size_t column = arg->value ? arg->value->column : stmt->column;
        const AstDecl* passed = !passes_on ? NULL
            : arg->value->decl_link && arg->value->decl_link->kind == AST_DECL_FUNC ? arg->value->decl_link
            : arg->value->decl_link ? NULL : sema_generic_results_callee(ctx, module, symbols, arg->value);
        if (passed && passed->as.func_decl.returns && passed->as.func_decl.returns->next) {
            snprintf(buf, sizeof buf,
                     "a 'ret' does not pass on the results of '%.*s': take them apart, then give each by its label "
                     "(`ret %.*s: ..., ...`)", (int)passed->as.func_decl.name.len, passed->as.func_decl.name.data,
                     (int)results->name.len, results->name.data);
            sema_results_error(module, line, column, buf);
            return;
        }
        if (!arg->has_label) {
            snprintf(buf, sizeof buf, "this function has several results (%s): 'ret' gives each by its label, "
                     "as in `ret %.*s: value, ...`", labels, (int)results->name.len, results->name.data);
            sema_results_error(module, line, column, buf);
            for (AstReturnArg* rest = arg->next; rest; rest = rest->next)
                if (rest->value) sema_analyze_expr(ctx, module, symbols, rest->value, true);
            return;
        }
        const AstReturnItem* result = sema_result_named(results, arg->label);
        if (!result) {
            snprintf(buf, sizeof buf, "this function has no result '%.*s'; its results are: %s",
                     (int)arg->label.len, arg->label.data, labels);
            sema_results_error(module, line, column, buf);
            continue;
        }
        bool twice = false;
        for (const AstReturnArg* earlier = stmt->as.ret_stmt.values; earlier != arg; earlier = earlier->next)
            if (earlier->has_label && str_eq(earlier->label, arg->label)) twice = true;
        if (twice) {
            snprintf(buf, sizeof buf, "result '%.*s' is given twice", (int)arg->label.len, arg->label.data);
            sema_results_error(module, line, column, buf);
            continue;
        }
        if (arg->value && result->type) {
            TypeInfo* wanted = sema_resolve_type_internal(ctx, module, symbols, result->type);
            if (arg->value->kind == AST_EXPR_CALL && sema_call_returns_reference(module, arg->value)) {
                sema_results_error(module, line, column,
                                   "this call returns a reference, and a result is a value; bind it first "
                                   "('let source: view T = ...'), then give 'source'");
            }
            ensure_type_match(ctx, wanted, &arg->value);
        }
    }
    /* every result, in each `ret` */
    char missing[256];
    size_t pos = 0;
    missing[0] = '\0';
    for (const AstReturnItem* r = results; r; r = r->next) {
        bool given = false;
        for (const AstReturnArg* arg = stmt->as.ret_stmt.values; arg; arg = arg->next)
            if (arg->has_label && str_eq(arg->label, r->name)) given = true;
        if (given || !r->has_name || pos >= sizeof missing) continue;
        int wrote = snprintf(missing + pos, sizeof missing - pos, "%s'%.*s'", pos ? ", " : "", (int)r->name.len, r->name.data);
        if (wrote > 0) pos += (size_t)wrote;
    }
    if (pos) {
        snprintf(buf, sizeof buf, "this 'ret' does not give %s: every 'ret' gives every result (%s)", missing, labels);
        sema_results_error(module, stmt->line, stmt->column, buf);
    }
}

/* A call of a function with several results that is not the call of a
 * destructuring statement: reported, true. */
static bool sema_several_results_as_value(AstModule* module, const AstExpr* call, const AstDecl* callee) {
    const AstFuncDecl* fd = &callee->as.func_decl;
    if (!fd->returns || !fd->returns->next || call == s_destructured_call) return false;
    if (call == s_spawned_call) {
        char spawned[400];
        snprintf(spawned, sizeof spawned,
                 "'%.*s' has several results and a Task has one: spawn a function that calls it and returns "
                 "what is wanted of them", (int)fd->name.len, fd->name.data);
        sema_results_error(module, call->line, call->column, spawned);
        return true;
    }
    char labels[256];
    char buf[640];
    sema_result_labels(fd->returns, labels, sizeof labels);
    snprintf(buf, sizeof buf,
             "'%.*s' has several results (%s), so its call is no value: take them apart, as in "
             "`let %.*s: T = %.*s, ... = %.*s(...)`",
             (int)fd->name.len, fd->name.data, labels, (int)fd->returns->name.len, fd->returns->name.data,
             (int)fd->returns->name.len, fd->returns->name.data, (int)fd->name.len, fd->name.data);
    sema_results_error(module, call->line, call->column, buf);
    return true;
}

/* `let quotient: Int = quotient, var rest: Int = remainder = divide(a: 7, b: 2)` */
static void sema_analyze_destructure(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstStmt* stmt) {
    AstExpr* call = stmt->as.destruct_stmt.call;
    char buf[640];
    const AstExpr* saved = s_destructured_call;
    s_destructured_call = call;
    if (call) sema_analyze_expr(ctx, module, symbols, call, true);
    s_destructured_call = saved;
    const AstDecl* callee = call && call->decl_link && call->decl_link->kind == AST_DECL_FUNC ? call->decl_link : NULL;
    /* a generic function's call is the backend's to bind: its results by its values */
    bool generic = false;
    if (!callee && call) {
        callee = sema_generic_results_callee(ctx, module, symbols, call);
        generic = callee != NULL;
    }
    const AstReturnItem* results = callee ? callee->as.func_decl.returns : NULL;
    if (callee && (!results || !results->next)) {
        snprintf(buf, sizeof buf, "'%.*s' has %s",
                 (int)callee->as.func_decl.name.len, callee->as.func_decl.name.data,
                 results ? "one result, not several to take apart; bind its call with a plain `let`"
                         : "no result to take apart");
        sema_results_error(module, stmt->line, stmt->column, buf);
        results = NULL;
    } else if (!callee && module && !module->had_error) {
        sema_results_error(module, stmt->line, stmt->column,
                           "cannot tell which function with several results this call is");
    }
    char labels[256];
    sema_result_labels(results, labels, sizeof labels);
    for (AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next) {
        TypeInfo* declared = b->type ? sema_resolve_type_internal(ctx, module, symbols, b->type) : NULL;
        if (b->type && (b->type->is_view || b->type->is_mod)) {
            snprintf(buf, sizeof buf, "'%.*s' is written as a reference; a result is a value, and the local owns it",
                     (int)b->local_name.len, b->local_name.data);
            sema_results_error(module, b->line, b->column, buf);
        }
        const AstReturnItem* result = results ? sema_result_named(results, b->return_label) : NULL;
        if (results && !result) {
            snprintf(buf, sizeof buf, "'%.*s' has no result '%.*s'; its results are: %s",
                     (int)callee->as.func_decl.name.len, callee->as.func_decl.name.data,
                     (int)b->return_label.len, b->return_label.data, labels);
            sema_results_error(module, b->line, b->column, buf);
        }
        for (const AstDestructureBinding* earlier = stmt->as.destruct_stmt.bindings; result && earlier != b;
             earlier = earlier->next) {
            if (!str_eq(earlier->return_label, b->return_label)) continue;
            snprintf(buf, sizeof buf, "result '%.*s' is taken twice; each result goes to one local",
                     (int)b->return_label.len, b->return_label.data);
            sema_results_error(module, b->line, b->column, buf);
            break;
        }
        if (result && result->type && declared && !b->type->is_view && !b->type->is_mod) {
            TypeInfo* given = generic ? sema_generic_result_type(ctx, module, symbols, callee, result, call)
                                      : sema_resolve_type_internal(ctx, module, symbols, result->type);
            if (given && given != declared) {
                char want[160], have[160];
                size_t want_pos = 0, have_pos = 0;
                sema_type_text(b->type, want, sizeof want, &want_pos);
                sema_type_text(generic ? sema_type_ref_of(ctx, given) : result->type, have, sizeof have, &have_pos);
                snprintf(buf, sizeof buf, "result '%.*s' is '%s'; '%.*s' is written '%s'",
                         (int)b->return_label.len, b->return_label.data, have,
                         (int)b->local_name.len, b->local_name.data, want);
                sema_results_error(module, b->line, b->column, buf);
            }
        }
    }
    /* every result goes to a local */
    for (const AstReturnItem* r = results; r; r = r->next) {
        bool taken = false;
        for (const AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next)
            if (r->has_name && str_eq(b->return_label, r->name)) taken = true;
        if (taken || !r->has_name) continue;
        snprintf(buf, sizeof buf, "result '%.*s' of '%.*s' is not taken; every result goes to a local (%s)",
                 (int)r->name.len, r->name.data, (int)callee->as.func_decl.name.len, callee->as.func_decl.name.data,
                 labels);
        sema_results_error(module, stmt->line, stmt->column, buf);
    }
    /* The locals, after the call: its arguments do not see them */
    for (AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next) {
        AstStmt as_let = { .kind = AST_STMT_LET, .line = b->line, .column = b->column };
        as_let.as.let_stmt.name = b->local_name;
        sema_check_rebinding(module, symbols, &as_let);
        TypeInfo* declared = b->type ? sema_resolve_type_internal(ctx, module, symbols, b->type) : NULL;
        Symbol* local = symbol_table_define(symbols, ctx->ast_arena, b->local_name, NULL, declared, !b->is_var);
        if (!local) continue;
        local->is_local_storage = true;
        local->bind_kind = b->is_var ? BIND_MUTABLE : BIND_LET;
        local->decl_line = (int)b->line;
        local->decl_column = (int)b->column;
    }
}
