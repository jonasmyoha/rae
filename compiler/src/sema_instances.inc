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

static void sema_literal_instances(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, TypeInfo* type);

/* A concrete generic struct (StringMap(Box)) is built with its fields' defaults:
 * a `{ }` list field (`values: List(T) = {}`) needs createList and add of its
 * type there. The number of instances it queued. */
static size_t sema_default_literal_instances(CompilerContext* ctx, AstModule* module, SymbolTable* symbols,
                                             TypeInfo* type) {
    AstDecl* template_decl = type->as.structure.decl;
    AstDecl* spec = type_registry_find_specialization(ctx->type_registry, template_decl,
                                                      type->as.structure.generic_args,
                                                      type->as.structure.generic_count);
    if (!spec || spec->kind != AST_DECL_TYPE) return 0;
    size_t before = s_instance_work_count;
    for (AstTypeField* field = spec->as.type_decl.fields; field; field = field->next) {
        if (!field->type || !field->default_value || field->default_value->kind != AST_EXPR_COLLECTION_LITERAL
            || field->default_value->as.collection.type) continue;
        sema_literal_instances(ctx, module, symbols, sema_resolve_type_internal(ctx, module, symbols, field->type));
    }
    return s_instance_work_count - before;
}

static int sema_compare_str(Str left, Str right) {
    size_t shorter = left.len < right.len ? left.len : right.len;
    int order = shorter ? memcmp(left.data, right.data, shorter) : 0;
    if (order) return order;
    if (left.len != right.len) return left.len < right.len ? -1 : 1;
    return 0;
}

/* An order of types that holds in every run: by name and kind, then by what they
 * are made of. Two types can share a name (`List(Any)` and `List(opt Any)` are both
 * named for their template), and qsort leaves equal ones in any order. */
static int sema_compare_types(const TypeInfo* left, const TypeInfo* right, int depth) {
    if (left == right) return 0;
    if (!left || !right) return left ? 1 : -1;
    int order = sema_compare_str(left->name, right->name);
    if (order) return order;
    if (left->kind != right->kind) return (int)left->kind - (int)right->kind;
    if (depth > 16) return 0;
    switch (left->kind) {
        case TYPE_STRUCT:
        case TYPE_GENERIC_INST:
            if (left->as.structure.generic_count != right->as.structure.generic_count)
                return left->as.structure.generic_count < right->as.structure.generic_count ? -1 : 1;
            for (size_t i = 0; i < left->as.structure.generic_count; i++) {
                order = sema_compare_types(left->as.structure.generic_args[i], right->as.structure.generic_args[i],
                                           depth + 1);
                if (order) return order;
            }
            return 0;
        case TYPE_REF:
            if (left->as.ref.is_mod != right->as.ref.is_mod) return left->as.ref.is_mod ? 1 : -1;
            return sema_compare_types(left->as.ref.base, right->as.ref.base, depth + 1);
        case TYPE_OPT: return sema_compare_types(left->as.opt.base, right->as.opt.base, depth + 1);
        case TYPE_BUFFER: return sema_compare_types(left->as.buffer.base, right->as.buffer.base, depth + 1);
        case TYPE_TASK: return sema_compare_types(left->as.task.base, right->as.task.base, depth + 1);
        case TYPE_ARRAY:
            if (left->as.array.count != right->as.array.count) return left->as.array.count < right->as.array.count ? -1 : 1;
            return sema_compare_types(left->as.array.base, right->as.array.base, depth + 1);
        case TYPE_INT:
            if (left->as.integer.bits != right->as.integer.bits) return left->as.integer.bits < right->as.integer.bits ? -1 : 1;
            if (left->as.integer.is_unsigned != right->as.integer.is_unsigned) return left->as.integer.is_unsigned ? 1 : -1;
            return 0;
        default: return 0;
    }
}

static int sema_compare_type_names(const void* a, const void* b) {
    return sema_compare_types(*(const TypeInfo* const*)a, *(const TypeInfo* const*)b, 0);
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
    /* the registry's buckets follow addresses: in name order, so the C is the same every run */
    qsort(found, found_count, sizeof *found, sema_compare_type_names);
    size_t queued = 0;
    for (size_t i = 0; i < found_count; i++) {
        if (sema_container_instance(ctx, module, symbols, "drop", found[i])) queued++;
        queued += sema_default_literal_instances(ctx, module, symbols, found[i]);
    }
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

/* Each named value argument is one of the candidate's parameters: overloads
 * that differ only in their names (`pair(first:, second:)`, `pair(left:, right:)`) */
static bool sema_argument_names_fit(const AstDecl* d, SymbolTable* symbols, const AstCallArg* args) {
    const AstFuncDecl* fd = &d->as.func_decl;
    for (const AstCallArg* a = args; a; a = a->next) {
        if (!a->name.len || sema_is_type_argument(fd, symbols, a)) continue;
        bool found = false;
        for (const AstParam* p = fd->params; p && !found; p = p->next) found = str_eq(p->name, a->name);
        if (!found) return false;
    }
    return true;
}

/* The template a call means and its type arguments, NULL when they are not known
 * or more than one template fits */
static AstDecl* sema_call_template(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, const AstExpr* call,
                                   AstTypeRef** types_out, bool* qualified_out) {
    Str name = {0};
    TypeInfo* receiver = NULL;
    const AstCallArg* args = NULL;
    AstTypeRef* written = NULL;
    bool qualified = false;
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
        /* `Query.query2(...)`: a module's name, no value; a plain call of its function */
        qualified = !receiver && call->as.method_call.object->kind == AST_EXPR_IDENT
            && !symbol_table_lookup(symbols, call->as.method_call.object->as.ident);
        if (!receiver && !qualified) return NULL;
        /* `Float.createList(cap: 4)`: the type is the first type argument */
        if (qualified && !written && sema_arg_is_type_name(symbols, call->as.method_call.object))
            written = sema_type_ref_of_expr(ctx, call->as.method_call.object);
    }
    *qualified_out = qualified;
    AstDecl* linked = call->decl_link;
    if (linked && linked->kind == AST_DECL_FUNC && linked->as.func_decl.generic_params
        && !linked->as.func_decl.specialization_args) {
        *types_out = sema_generic_types(ctx, module, symbols, linked, receiver, args, written, 0);
        return *types_out ? linked : NULL;
    }
    if (linked) return NULL;
    AstDecl* found = NULL;
    AstTypeRef* found_types = NULL;
    bool ambiguous = false;
    int found_rank = -1;
    for (Symbol* curr = symbol_table_first(symbols, name); curr; curr = curr->sameNext) {
        AstDecl* d = curr->decl;
        if (!d || d->kind != AST_DECL_FUNC || !d->as.func_decl.generic_params || d->as.func_decl.specialization_args)
            continue;
        if (d == found || !sema_package_reaches(s_current_decl_origin, d)) continue;
        if (!receiver && !qualified && !sema_decl_opened(s_current_decl_origin, d)) continue;
        if (receiver && !sema_receiver_fits(d, receiver)) continue;
        if (!receiver && !sema_first_value_fits(ctx, module, symbols, d, args)) continue;
        if (!sema_argument_names_fit(d, symbols, args)) continue;
        AstTypeRef* types = sema_generic_types(ctx, module, symbols, d, receiver, args, written, 0);
        if (!types) continue;
        /* several fit: a method call means one with `this` (sema_yields_to_method),
         * then the program's own wins (resolve_function_overload) */
        const AstParam* first = d->as.func_decl.params;
        int rank = (receiver && first && str_eq_cstr(first->name, "this") ? 2 : 0) + (sema_decl_is_own(d) ? 1 : 0);
        if (found && rank == found_rank) { ambiguous = true; continue; }
        if (found && rank < found_rank) continue;
        found = d;
        found_types = types;
        found_rank = rank;
        ambiguous = false;
    }
    if (ambiguous) return NULL;  /* not this function's to decide */
    if (!found && !receiver) found = sema_array_template(ctx, module, symbols, name, args, written, &found_types);
    *types_out = found_types;
    return found;
}

/* The call's type is its instance's result, where analysis left it without one */
static void sema_type_from_instance(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* call,
                                    AstDecl* spec) {
    if (!spec || spec->kind != AST_DECL_FUNC || sema_type_is_concrete(call->resolved_type, 0)) return;
    if (spec->as.func_decl.is_extern || !spec->as.func_decl.body) return;
    const AstReturnItem* result = spec->as.func_decl.returns;
    if (!result || !result->type || result->next) return;  /* none, or several: no value */
    TypeInfo* type = sema_resolve_type_internal(ctx, module, symbols, result->type);
    if (sema_type_is_concrete(type, 0)) call->resolved_type = type;
}

static void sema_decide_instance(CompilerContext* ctx, AstModule* module, SymbolTable* symbols, AstExpr* call) {
    if (!sema_infer_all_calls() || call->instance) return;
    AstDecl* linked = call->decl_link;
    if (linked && linked->kind == AST_DECL_FUNC && linked->as.func_decl.specialization_args) {
        call->instance = linked;
        return;
    }
    AstTypeRef* types = NULL;
    bool qualified = false;
    AstDecl* generic_decl = sema_call_template(ctx, module, symbols, call, &types, &qualified);
    if (getenv("RAE_DEBUG_INSTANCES") && !generic_decl && !call->decl_link && call->kind == AST_EXPR_CALL
        && call->as.call.callee
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
    sema_type_from_instance(ctx, module, symbols, call, spec);
    /* the argument rules a resolved call gets (`own` to a view parameter, a
     * local moved into `own T`), by the instance's parameters */
    if (spec && !call->decl_link && spec->kind == AST_DECL_FUNC) {
        AstCallArg* values = call->kind == AST_EXPR_CALL ? call->as.call.args : call->as.method_call.args;
        while (values && sema_is_type_argument(&generic_decl->as.func_decl, symbols, values)) values = values->next;
        bool has_receiver = call->kind == AST_EXPR_METHOD_CALL && !qualified;
        sema_check_own_args(ctx, module, symbols, &spec->as.func_decl, values, has_receiver);
        const AstParam* receiver = spec->as.func_decl.params;
        char message[400];
        if (has_receiver && receiver
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
