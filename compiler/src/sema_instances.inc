/* sema_instances.inc — sema decides the instance of every call of a generic
 * function (docs/generic-instances-design.md, step 4). Included by sema.c.
 *
 * After a call is analyzed, sema_decide_instance finds its template and its type
 * arguments (written, or inferred from the values: sema_generic_types), and the
 * instance in the registry (type_registry_find_specialization, keyed by the
 * template and the interned types), making it when it is new. A new instance is
 * put on a worklist and analyzed when the function that made it is done
 * (sema_drain_instances), so making one never runs inside another's analysis;
 * its calls make the next ones, until no new one comes. The call keeps it in
 * `instance`; decl_link stays what the backend resolves by itself, until the
 * backend emits from instances (step 5). RAE_SEMA_INSTANCES=0 turns this off. */

typedef struct InstanceWork {
    AstDecl* generic_decl;
    AstDecl* spec;
    const AstTypeRef* args;
    const AstTypeRef* written;
    const char* made_in;
    size_t line;
    size_t column;
} InstanceWork;

static InstanceWork* s_instance_work;
static size_t s_instance_work_count;
static size_t s_instance_work_cap;
static bool s_defer_instances;  /* specialize_decl queues the instance it makes */

static void sema_queue_instance(AstDecl* generic_decl, AstDecl* spec, const AstTypeRef* args,
                                const AstTypeRef* written, size_t line, size_t column) {
    if (s_instance_work_count == s_instance_work_cap) {
        s_instance_work_cap = s_instance_work_cap ? s_instance_work_cap * 2 : 256;
        s_instance_work = realloc(s_instance_work, s_instance_work_cap * sizeof *s_instance_work);
    }
    s_instance_work[s_instance_work_count++] = (InstanceWork){
        .generic_decl = generic_decl, .spec = spec, .args = args, .written = written,
        .made_in = s_current_decl_origin, .line = line, .column = column,
    };
}

static void sema_drain_queue(CompilerContext* ctx, AstModule* module, SymbolTable* symbols);

/* The templates sema_check_template_names refused */
static const AstDecl** s_rejected_templates;
static size_t s_rejected_template_count;

static void sema_note_rejected_template(const AstDecl* template_decl) {
    s_rejected_templates = realloc(s_rejected_templates, (s_rejected_template_count + 1) * sizeof *s_rejected_templates);
    s_rejected_templates[s_rejected_template_count++] = template_decl;
}

static bool sema_template_rejected(const AstDecl* template_decl) {
    for (size_t i = 0; i < s_rejected_template_count; i++)
        if (s_rejected_templates[i] == template_decl) return true;
    return false;
}

/* The generic function `name` of the template `type_decl`: its first parameter
 * or its result is that type (`drop(this: List(T))`, `createList(T, cap:) ret
 * List(T)`), NULL for none */
static AstDecl* sema_container_function(SymbolTable* symbols, const char* name, const AstDecl* type_decl) {
    Str type_name = type_decl->as.type_decl.name;
    for (Symbol* curr = symbol_table_first(symbols, str_from_cstr(name)); curr; curr = curr->sameNext) {
        AstDecl* d = curr->decl;
        if (!d || d->kind != AST_DECL_FUNC || !d->as.func_decl.generic_params || d->as.func_decl.specialization_args)
            continue;
        const AstParam* first = d->as.func_decl.params;
        while (first && first->type && str_eq_cstr(get_base_type_name(first->type), "type")) first = first->next;
        const AstReturnItem* result = d->as.func_decl.returns;
        if ((first && first->type && str_eq(get_base_type_name(first->type), type_name))
            || (result && result->type && str_eq(get_base_type_name(result->type), type_name))) return d;
    }
    return NULL;
}

/* The instance of the container function `name` for the concrete `container`
 * (`createList` of List(Spot)), queued when it is new; false for none */
static bool sema_container_instance(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, const char* name,
                                    TypeInfo* container) {
    while (container && container->kind == TYPE_REF) container = container->as.ref.base;
    if (!container || (container->kind != TYPE_STRUCT && container->kind != TYPE_GENERIC_INST)) return false;
    const AstDecl* type_decl = container->as.structure.decl;
    if (!type_decl || type_decl->kind != AST_DECL_TYPE || container->as.structure.generic_count == 0) return false;
    AstDecl* function = sema_container_function(symbols, name, type_decl);
    if (!function || function->as.func_decl.is_extern) return false;
    size_t want = 0;
    for (const AstIdentifierPart* gp = function->as.func_decl.generic_params; gp; gp = gp->next) want++;
    if (want != container->as.structure.generic_count) return false;
    if (type_registry_find_specialization(ctx->type_registry, function, container->as.structure.generic_args, want))
        return false;
    bool saved = s_defer_instances;
    s_defer_instances = true;
    specialize_decl(ctx, module, symbols, function, container->as.structure.generic_args, want, 0, 0);
    s_defer_instances = saved;
    return true;
}

/* A value of a container type is dropped where its owner ends, a call nobody
 * writes: each concrete instantiation of a template with a generic `drop`
 * (List(Note), ComponentTable(Position), ...) gets that drop's instance.
 * The number of instances it queued. */
static size_t sema_queue_drops(CompilerContext* ctx, AstModule* module, SymbolTable* symbols) {
    TypeRegistry* registry = ctx->type_registry;
    size_t found_count = 0, found_cap = 64;
    TypeInfo** found = malloc(found_cap * sizeof *found);
    for (size_t b = 0; b < registry->capacity; b++) {
        for (TypeInfo* t = registry->buckets[b]; t; t = t->next_interned) {
            if ((t->kind != TYPE_STRUCT && t->kind != TYPE_GENERIC_INST) || t->as.structure.generic_count == 0) continue;
            const AstDecl* decl = t->as.structure.decl;
            if (!decl || decl->kind != AST_DECL_TYPE || !decl->as.type_decl.generic_params) continue;
            if (!sema_type_is_concrete(t, 0)) continue;
            if (found_count == found_cap) found = realloc(found, (found_cap *= 2) * sizeof *found);
            found[found_count++] = t;
        }
    }
    size_t queued = 0;
    for (size_t i = 0; i < found_count; i++)
        if (sema_container_instance(ctx, module, symbols, "drop", found[i])) queued++;
    free(found);
    return queued;
}

/* Analyze the queued instances, and those their analysis queues, in order */
static void sema_drain_instances(CompilerContext* ctx, AstModule* module, SymbolTable* symbols) {
    static bool draining;
    if (draining) return;
    draining = true;
    do {
        sema_drain_queue(ctx, module, symbols);
    } while (sema_infer_all_calls() && sema_queue_drops(ctx, module, symbols) > 0);
    draining = false;
}

static void sema_drain_queue(CompilerContext* ctx, AstModule* module, SymbolTable* symbols) {
    /* an instance of a template whose names were already refused would only
     * repeat that error (an unknown type in its signature) */
    for (size_t i = 0; i < s_instance_work_count; i++) {
        InstanceWork work = s_instance_work[i];
        if (sema_template_rejected(work.generic_decl)) continue;
        const char* saved_origin = s_current_decl_origin;
        s_current_decl_origin = work.made_in;
        sema_analyze_instance(ctx, module, symbols, work.generic_decl, work.spec, work.args, work.written,
                              work.line, work.column);
        work.spec->as.func_decl.is_analyzed_instance = true;
        s_current_decl_origin = saved_origin;
    }
    s_instance_work_count = 0;
}

/* A method's receiver is a value of its first parameter's type: `bytes.set(...)`
 * of a List(UInt8) is List's set, not a map's that would take V from the value */
static bool sema_receiver_fits(const AstDecl* d, TypeInfo* receiver) {
    const AstFuncDecl* fd = &d->as.func_decl;
    const AstParam* first = fd->params;
    if (!first || !first->type) return false;
    Str base = get_base_type_name(first->type);
    for (const AstIdentifierPart* gp = fd->generic_params; gp; gp = gp->next)
        if (str_eq(gp->text, base)) return true;  /* a bare T takes anything */
    Str name = receiver->name;
    if ((receiver->kind == TYPE_STRUCT || receiver->kind == TYPE_GENERIC_INST) && receiver->as.structure.decl
        && receiver->as.structure.decl->kind == AST_DECL_TYPE)
        name = receiver->as.structure.decl->as.type_decl.name;
    else if (receiver->kind == TYPE_ARRAY) name = str_from_cstr("Array");
    else if (receiver->kind == TYPE_BUFFER) name = str_from_cstr("Buffer");
    else if (receiver->kind == TYPE_STRING) name = str_from_cstr("String");
    return str_eq(base, name);
}

/* The same for a plain call whose first parameter is an Array: the Array methods
 * are one template per capacity (array_methods.c), `copyAt(this, index:)` */
static bool sema_first_value_fits(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, const AstDecl* d,
                                  const AstCallArg* args) {
    const AstParam* first = d->as.func_decl.params;
    if (!first || !first->type || !str_eq_cstr(get_base_type_name(first->type), "Array")) return true;
    while (args && args->name.len == 0 && sema_arg_is_type_name(symbols, args->value)) args = args->next;
    TypeInfo* value = args && args->value ? sema_value_type(ctx, module, symbols, args->value, 0) : NULL;
    while (value && value->kind == TYPE_REF) value = value->as.ref.base;
    return !value || sema_array_receiver_matches(first->type, value);
}

/* The Array methods are declarations of the module but not in its symbols
 * (array_methods.c): `copyAt(this, index:)` in one of them, by its first value */
static AstDecl* sema_array_template(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, Str name,
                                    const AstCallArg* args, AstTypeRef* written, AstTypeRef** types_out) {
    const AstCallArg* first = args;
    while (first && first->name.len == 0 && sema_arg_is_type_name(symbols, first->value)) first = first->next;
    TypeInfo* value = first && first->value ? sema_value_type(ctx, module, symbols, first->value, 0) : NULL;
    while (value && value->kind == TYPE_REF) value = value->as.ref.base;
    if (!value || value->kind != TYPE_ARRAY || !module) return NULL;
    for (AstDecl* d = module->decls; d; d = d->next) {
        if (d->kind != AST_DECL_FUNC || !d->as.func_decl.generic_params || d->as.func_decl.specialization_args
            || !str_eq(d->as.func_decl.name, name)) continue;
        const AstParam* param = d->as.func_decl.params;
        if (!param || !param->type || !str_eq_cstr(get_base_type_name(param->type), "Array")
            || !sema_array_receiver_matches(param->type, value)) continue;
        *types_out = sema_generic_types(ctx, module, symbols, d, NULL, args, written, 0);
        if (*types_out) return d;
    }
    return NULL;
}

/* The template a call means and its type arguments, NULL when they are not known
 * or more than one template fits */
static AstDecl* sema_call_template(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, const AstExpr* call,
                                   AstTypeRef** types_out) {
    Str name = {0};
    TypeInfo* receiver = NULL;
    const AstCallArg* args = NULL;
    AstTypeRef* written = NULL;
    if (call->kind == AST_EXPR_CALL) {
        if (!call->as.call.callee || call->as.call.callee->kind != AST_EXPR_IDENT) return NULL;
        name = call->as.call.callee->as.ident;
        args = call->as.call.args;
        written = call->as.call.generic_args;
    } else {
        name = call->as.method_call.method_name;
        args = call->as.method_call.args;
        written = call->as.method_call.generic_args;
        receiver = sema_value_type(ctx, module, symbols, call->as.method_call.object, 0);
        while (receiver && receiver->kind == TYPE_REF) receiver = receiver->as.ref.base;
        if (!receiver) return NULL;
    }
    AstDecl* linked = call->decl_link;
    if (linked && linked->kind == AST_DECL_FUNC && linked->as.func_decl.generic_params
        && !linked->as.func_decl.specialization_args) {
        *types_out = sema_generic_types(ctx, module, symbols, linked, receiver, args, written, 0);
        return *types_out ? linked : NULL;
    }
    if (linked) return NULL;
    AstDecl* found = NULL;
    AstTypeRef* found_types = NULL;
    for (Symbol* curr = symbol_table_first(symbols, name); curr; curr = curr->sameNext) {
        AstDecl* d = curr->decl;
        if (!d || d->kind != AST_DECL_FUNC || !d->as.func_decl.generic_params || d->as.func_decl.specialization_args)
            continue;
        if (d == found || !sema_package_reaches(s_current_decl_origin, d)) continue;
        if (!receiver && !sema_decl_opened(s_current_decl_origin, d)) continue;
        if (receiver && !sema_receiver_fits(d, receiver)) continue;
        if (!receiver && !sema_first_value_fits(ctx, module, symbols, d, args)) continue;
        AstTypeRef* types = sema_generic_types(ctx, module, symbols, d, receiver, args, written, 0);
        if (!types) continue;
        if (found) return NULL;  /* two fit: not this function's to decide */
        found = d;
        found_types = types;
    }
    if (!found && !receiver) found = sema_array_template(ctx, module, symbols, name, args, written, &found_types);
    *types_out = found_types;
    return found;
}

static void sema_decide_instance(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* call) {
    if (!sema_infer_all_calls() || call->instance || s_in_spawn_wrapper || call == s_spawned_call) return;
    AstDecl* linked = call->decl_link;
    if (linked && linked->kind == AST_DECL_FUNC && linked->as.func_decl.specialization_args) {
        call->instance = linked;
        return;
    }
    AstTypeRef* types = NULL;
    AstDecl* generic_decl = sema_call_template(ctx, module, symbols, call, &types);
    if (getenv("RAE_DEBUG_INSTANCES") && !generic_decl && call->kind == AST_EXPR_CALL && call->as.call.callee
        && call->as.call.callee->kind == AST_EXPR_IDENT)
        fprintf(stderr, "[undecided] %.*s at %zu:%zu linked %p\n", (int)call->as.call.callee->as.ident.len,
                call->as.call.callee->as.ident.data, call->line, call->column, (void*)call->decl_link);
    if (!generic_decl || generic_decl->as.func_decl.is_extern) return;
    TypeInfo* resolved[16];
    size_t count = 0;
    for (AstTypeRef* tr = types; tr; tr = tr->next) {
        if (count == 16) return;
        AstTypeRef one = *tr;
        one.next = NULL;
        TypeInfo* t = sema_resolve_type_internal(ctx, module, symbols, &one);
        if (!sema_type_is_concrete(t, 0)) return;
        resolved[count++] = t;
        if (getenv("RAE_DEBUG_INSTANCES"))
            fprintf(stderr, "[decided] %.*s at %zu: %.*s kind %d\n", (int)generic_decl->as.func_decl.name.len,
                    generic_decl->as.func_decl.name.data, call->line, (int)t->name.len, t->name.data, (int)t->kind);
    }
    AstDecl* spec = type_registry_find_specialization(ctx->type_registry, generic_decl, resolved, count);
    if (!spec) {
        bool saved = s_defer_instances;
        s_defer_instances = true;
        spec = specialize_decl(ctx, module, symbols, generic_decl, resolved, count, call->line, call->column);
        s_defer_instances = saved;
    }
    call->instance = spec;
    /* the argument rules a resolved call gets (`own` to a view parameter, a
     * local moved into `own T`), by the instance's parameters */
    if (spec && !call->decl_link && spec->kind == AST_DECL_FUNC) {
        AstCallArg* values = call->kind == AST_EXPR_CALL ? call->as.call.args : call->as.method_call.args;
        while (values && values->name.len == 0 && sema_arg_is_type_name(symbols, values->value)) values = values->next;
        sema_check_own_args(ctx, module, symbols, &spec->as.func_decl, values, call->kind == AST_EXPR_METHOD_CALL);
        const AstParam* receiver = spec->as.func_decl.params;
        char message[400];
        if (call->kind == AST_EXPR_METHOD_CALL && receiver
            && sema_own_argument_error(&spec->as.func_decl, receiver, call->as.method_call.object, message,
                                       sizeof message)) {
            diag_error(sema_diag_file(module), (int)call->as.method_call.object->line,
                       (int)call->as.method_call.object->column, message);
            module->had_error = true;
        }
    }
}

/* A `{ ... }` list literal is built with createList and add, calls nobody
 * writes; `type` is its own, or where an untyped one goes */
static void sema_literal_instances(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, TypeInfo* type) {
    if (!sema_infer_all_calls() || !module || !symbols || !sema_type_is_concrete(type, 0)) return;
    while (type && type->kind == TYPE_REF) type = type->as.ref.base;
    if (!type || (type->kind != TYPE_STRUCT && type->kind != TYPE_GENERIC_INST) || !type->as.structure.decl) return;
    if (!str_eq_cstr(type->as.structure.decl->as.type_decl.name, "List")) return;
    sema_container_instance(ctx, module, symbols, "createList", type);
    sema_container_instance(ctx, module, symbols, "add", type);
}
