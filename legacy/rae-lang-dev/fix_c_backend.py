import sys

with open('baseline_c_backend.c', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    # 1. Update c_backend_emit_module entry point
    if 'bool c_backend_emit_module(const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {' in line:
        new_lines.append('static Str g_emitted_json_decls[512]; static size_t g_emitted_json_decls_count = 0;\n')
        new_lines.append('static Str g_emitted_json_impls[512]; static size_t g_emitted_json_impls_count = 0;\n')
        new_lines.append('static const AstTypeRef* g_generic_types[512]; static size_t g_generic_type_count = 0;\n')
        new_lines.append('static const AstTypeRef* g_emitted_generic_types[512]; static size_t g_emitted_generic_type_count = 0;\n')
        new_lines.append('static const AstDecl* g_all_decls[2048]; static size_t g_all_decl_count = 0;\n\n')
        
        new_lines.append('static void register_decl(const AstDecl* decl) {\n')
        new_lines.append('    if (!decl) return;\n')
        new_lines.append('    for (size_t i = 0; i < g_all_decl_count; i++) if (g_all_decls[i] == decl) return;\n')
        new_lines.append('    if (g_all_decl_count < 2048) g_all_decls[g_all_decl_count++] = decl;\n')
        new_lines.append('}\n\n')
        
        new_lines.append('static void collect_decls_from_module(const AstModule* module) {\n')
        new_lines.append('    if (!module) return;\n')
        new_lines.append('    static const AstModule* visited[128]; static size_t visited_count = 0;\n')
        new_lines.append('    for (size_t i = 0; i < visited_count; i++) if (visited[i] == module) return;\n')
        new_lines.append('    if (visited_count < 128) visited[visited_count++] = module;\n')
        new_lines.append('    for (const AstDecl* d = module->decls; d; d = d->next) register_decl(d);\n')
        new_lines.append('    for (const AstImport* imp = module->imports; imp; imp = imp->next) collect_decls_from_module(imp->module);\n')
        new_lines.append('    visited_count--;\n')
        new_lines.append('}\n\n')
        
        new_lines.append('static bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b) {\n')
        new_lines.append('    if (a == b) return true; if (!a || !b) return false;\n')
        new_lines.append('    if (a->parts && b->parts) { if (!str_eq(a->parts->text, b->parts->text)) return false; } else if (a->parts != b->parts) return false;\n')
        new_lines.append('    if (a->is_opt != b->is_opt || a->is_view != b->is_view || a->is_mod != b->is_mod) return false;\n')
        new_lines.append('    const AstTypeRef* aa = a->generic_args; const AstTypeRef* ab = b->generic_args;\n')
        new_lines.append('    while (aa && ab) { if (!type_refs_equal(aa, ab)) return false; aa = aa->next; ab = ab->next; }\n')
        new_lines.append('    return aa == NULL && ab == NULL;\n')
        new_lines.append('}\n\n')
        
        new_lines.append('static void register_generic_type(const AstTypeRef* type) {\n')
        new_lines.append('    if (!type || !type->generic_args) return;\n')
        new_lines.append('    for (size_t i = 0; i < g_generic_type_count; i++) if (type_refs_equal(g_generic_types[i], type)) return;\n')
        new_lines.append('    if (g_generic_type_count < 512) g_generic_types[g_generic_type_count++] = type;\n')
        new_lines.append('}\n\n')
        
        new_lines.append('static void collect_type_refs_expr(const AstExpr* e);\n')
        new_lines.append('static void collect_type_refs_stmt(const AstStmt* s);\n')
        
        new_lines.append('static void collect_type_refs_expr(const AstExpr* e) {\n')
        new_lines.append('    if(!e) return;\n')
        new_lines.append('    switch(e->kind) {\n')
        new_lines.append('        case AST_EXPR_CALL: collect_type_refs_expr(e->as.call.callee); for (const AstCallArg* a = e->as.call.args; a; a = a->next) collect_type_refs_expr(a->value); break;\n')
        new_lines.append('        case AST_EXPR_OBJECT: if (e->as.object_literal.type) register_generic_type(e->as.object_literal.type); for (const AstObjectField* f = e->as.object_literal.fields; f; f = f->next) collect_type_refs_expr(f->value); break;\n')
        new_lines.append('        case AST_EXPR_BINARY: collect_type_refs_expr(e->as.binary.lhs); collect_type_refs_expr(e->as.binary.rhs); break;\n')
        new_lines.append('        case AST_EXPR_UNARY: collect_type_refs_expr(e->as.unary.operand); break;\n')
        new_lines.append('        case AST_EXPR_MEMBER: collect_type_refs_expr(e->as.member.object); break;\n')
        new_lines.append('        case AST_EXPR_INDEX: collect_type_refs_expr(e->as.index.target); collect_type_refs_expr(e->as.index.index); break;\n')
        new_lines.append('        case AST_EXPR_METHOD_CALL: collect_type_refs_expr(e->as.method_call.object); for (const AstCallArg* a = e->as.method_call.args; a; a = a->next) collect_type_refs_expr(a->value); break;\n')
        new_lines.append('        default: break;\n')
        new_lines.append('    }\n')
        new_lines.append('}\n\n')
        
        new_lines.append('static void collect_type_refs_stmt(const AstStmt* s) {\n')
        new_lines.append('    if(!s) return;\n')
        new_lines.append('    switch(s->kind) {\n')
        new_lines.append('        case AST_STMT_LET: if (s->as.let_stmt.type) register_generic_type(s->as.let_stmt.type); collect_type_refs_expr(s->as.let_stmt.value); break;\n')
        new_lines.append('        case AST_STMT_EXPR: collect_type_refs_expr(s->as.expr_stmt); break;\n')
        new_lines.append('        case AST_STMT_RET: if(s->as.ret_stmt.values) for (const AstReturnArg* a = s->as.ret_stmt.values; a; a = a->next) collect_type_refs_expr(a->value); break;\n')
        new_lines.append('        case AST_STMT_IF: collect_type_refs_expr(s->as.if_stmt.condition); if (s->as.if_stmt.then_block) for (const AstStmt* st = s->as.if_stmt.then_block->first; st; st = st->next) collect_type_refs_stmt(st); if (s->as.if_stmt.else_block) for (const AstStmt* st = s->as.if_stmt.else_block->first; st; st = st->next) collect_type_refs_stmt(st); break;\n')
        new_lines.append('        case AST_STMT_LOOP: if (s->as.loop_stmt.body) for (const AstStmt* st = s->as.loop_stmt.body->first; st; st = st->next) collect_type_refs_stmt(st); break;\n')
        new_lines.append('        case AST_STMT_ASSIGN: collect_type_refs_expr(s->as.assign_stmt.target); collect_type_refs_expr(s->as.assign_stmt.value); break;\n')
        new_lines.append('        default: break;\n')
        new_lines.append('    }\n')
        new_lines.append('}\n\n')
        
        new_lines.append('static void collect_type_refs_module(const AstModule* m) {\n')
        new_lines.append('    for (size_t i = 0; i < g_all_decl_count; i++) {\n')
        new_lines.append('        const AstDecl* d = g_all_decls[i];\n')
        new_lines.append('        if (d->kind == AST_DECL_FUNC) {\n')
        new_lines.append('            if (d->as.func_decl.returns && d->as.func_decl.returns->type) register_generic_type(d->as.func_decl.returns->type);\n')
        new_lines.append('            for (const AstParam* p = d->as.func_decl.params; p; p = p->next) if (p->type) register_generic_type(p->type);\n')
        new_lines.append('            if (d->as.func_decl.body) for (const AstStmt* s = d->as.func_decl.body->first; s; s = s->next) collect_type_refs_stmt(s);\n')
        new_lines.append('        } else if (d->kind == AST_DECL_TYPE) {\n')
        new_lines.append('            for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) if (f->type) register_generic_type(f->type);\n')
        new_lines.append('        }\n')
        new_lines.append('    }\n')
        new_lines.append('}\n\n')
        
        new_lines.append('static void emit_mangled_type_name(const AstTypeRef* type, FILE* out) {\n')
        new_lines.append('    if (!type || !type->parts) { fprintf(out, "int64_t"); return; }\n')
        new_lines.append('    Str base = type->parts->text; const char* prim = map_rae_type_to_c(base);\n')
        new_lines.append('    if (prim && !type->generic_args) { fprintf(out, "%s", prim); return; }\n')
        new_lines.append('    fprintf(out, "%.*s", (int)base.len, base.data);\n')
        new_lines.append('    if (type->generic_args) {\n')
        new_lines.append('        fprintf(out, "_");\n')
        new_lines.append('        for (const AstTypeRef* a = type->generic_args; a; a = a->next) {\n')
        new_lines.append('            Str ab = get_base_type_name(a); fprintf(out, "%.*s_", (int)ab.len, ab.data);\n')
        new_lines.append('        }\n')
        new_lines.append('    }\n')
        new_lines.append('}\n\n')
        
        new_lines.append('bool c_backend_emit_module(const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {\n')
        new_lines.append('  if (!module || !out_path) return false;\n')
        new_lines.append('  g_emitted_json_decls_count = 0; g_emitted_json_impls_count = 0;\n')
        new_lines.append('  g_generic_type_count = 0; g_emitted_generic_type_count = 0; g_all_decl_count = 0;\n')
        new_lines.append('  collect_decls_from_module(module); collect_type_refs_module(module);\n')
        # skip next few original lines until the fopen
        continue 
    
    # 2. Update emit_json_methods signature
    if 'static bool emit_json_methods(const AstModule* module, FILE* out, bool uses_raylib) {' in line:
        new_lines.append('static bool emit_json_methods(CFuncContext* ctx, FILE* out, bool uses_raylib) {\n')
        new_lines.append('  (void)ctx; (void)uses_raylib;\n')
        continue

    # 3. Handle mangled type names in emit_type_ref_as_c_type
    if 'const char* c_type_str = map_rae_type_to_c(current->text);' in line:
        new_lines.append('  if (type->generic_args) { emit_mangled_type_name(type, out); return true; }\n')
        new_lines.append(line)
        continue

    new_lines.append(line)

with open('rae/compiler/src/c_backend.c', 'w') as f:
    f.writelines(new_lines)
