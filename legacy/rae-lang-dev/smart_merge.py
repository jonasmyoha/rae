import sys

with open('baseline_c_backend.c', 'r') as f:
    lines = f.readlines()

out = []
out.append('#include "c_backend.h"\n')
out.append('#include <math.h>\n')

# Find CDeferEntry, CDeferStack, CFuncContext
ctx_lines = []
in_block = False
for i, line in enumerate(lines):
    if 'typedef struct {' in line and ('CDeferEntry entries[64];' in lines[min(i+1, len(lines)-1)] or 'const struct AstBlock* block;' in lines[min(i+1, len(lines)-1)] or 'const AstModule* module;' in lines[min(i+1, len(lines)-1)]):
        in_block = True
    if in_block:
        ctx_lines.append(line)
        if ('} CDeferEntry;' in line or '} CDeferStack;' in line or '} CFuncContext;' in line):
            in_block = False

seen = set()
for l in ctx_lines:
    if l not in seen:
        out.append(l)
        seen.add(l)

out.append('\n// --- NEW INFRA ---\n')
out.append('static Str g_emitted_json_decls[512]; static size_t g_emitted_json_decls_count = 0;\n')
out.append('static Str g_emitted_json_impls[512]; static size_t g_emitted_json_impls_count = 0;\n')
out.append('static const AstTypeRef* g_generic_types[512]; static size_t g_generic_type_count = 0;\n')
out.append('static const AstTypeRef* g_emitted_generic_types[512]; static size_t g_emitted_generic_type_count = 0;\n')
out.append('static const AstDecl* g_all_decls[2048]; static size_t g_all_decl_count = 0;\n\n')

# Forward Declarations
out.append('static const char* map_rae_type_to_c(Str type_name);\n')
out.append('static Str get_base_type_name(const AstTypeRef* type);\n')
out.append('static void emit_mangled_type_name(const AstTypeRef* type, FILE* out);\n')
out.append('static void register_generic_type(const AstTypeRef* type);\n')
out.append('static bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b);\n')
out.append('static void collect_type_refs_expr(const AstExpr* e);\n')
out.append('static void collect_type_refs_stmt(const AstStmt* s);\n')
out.append('static const AstDecl* find_type_decl(const AstModule* module, Str name);\n')
out.append('static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out);\n')
out.append('static bool emit_specialized_struct_def(const AstModule* module, const AstTypeRef* type, FILE* out, bool uses_raylib);\n')
out.append('static bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue);\n')
out.append('static bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out);\n')
out.append('static bool emit_block(CFuncContext* ctx, const AstBlock* block, FILE* out);\n')
out.append('static bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out);\n')
out.append('static void pop_defers(CFuncContext* ctx, int depth);\n')
out.append('static void emit_mangled_function_name(const AstFuncDecl* func, FILE* out);\n')
out.append('static const AstDecl* find_enum_decl(const AstModule* module, Str name);\n')
out.append('static const char* find_raylib_mapping(Str name);\n')
out.append('static bool has_property(const AstProperty* props, const char* name);\n')
out.append('static const AstFuncDecl* find_function_overload(const AstModule* module, CFuncContext* ctx, Str name, const Str* param_types, uint16_t param_count);\n')
out.append('static bool emit_param_list(CFuncContext* ctx, const AstParam* params, FILE* out, bool is_extern);\n')
out.append('static const char* c_return_type(CFuncContext* ctx, const AstFuncDecl* func);\n')
out.append('static bool is_pointer_type(CFuncContext* ctx, Str name);\n')
out.append('static bool is_mod_type(CFuncContext* ctx, Str name);\n')
out.append('static Str get_local_type_name(CFuncContext* ctx, Str name);\n')
out.append('static const AstTypeRef* get_local_type_ref(CFuncContext* ctx, Str name);\n')
out.append('static Str infer_expr_type(CFuncContext* ctx, const AstExpr* expr);\n')
out.append('static bool emit_json_methods(CFuncContext* ctx, FILE* out, bool uses_raylib);\n')
out.append('static bool emit_enum_defs(const AstModule* module, FILE* out, bool uses_raylib);\n')
out.append('static bool emit_struct_defs(const AstModule* module, FILE* out, bool uses_raylib);\n')
out.append('static bool emit_raylib_wrapper(const AstFuncDecl* fn, const char* c_name, FILE* out, const AstModule* module);\n')
out.append('static bool emit_function(const AstModule* module, const AstFuncDecl* func, FILE* out, const struct VmRegistry* registry, bool uses_raylib);\n')
out.append('static bool emit_single_struct_def(const AstModule* module, const AstDecl* decl, FILE* out, Str* emitted_types, size_t* emitted_count, bool uses_raylib);\n\n')

# Infra Implementation
out.append('static void register_decl(const AstDecl* decl) { if (!decl) return; for (size_t i = 0; i < g_all_decl_count; i++) if (g_all_decls[i] == decl) return; if (g_all_decl_count < 2048) { g_all_decls[g_all_decl_count] = decl; g_all_decl_count++; } }\n')
out.append('static void collect_decls_from_module(const AstModule* module) { if (!module) return; static const AstModule* visited[128]; static size_t visited_count = 0; for (size_t i = 0; i < visited_count; i++) if (visited[i] == module) return; if (visited_count < 128) { visited[visited_count] = module; visited_count++; } for (const AstDecl* d = module->decls; d; d = d->next) register_decl(d); for (const AstImport* imp = module->imports; imp; imp = imp->next) collect_decls_from_module(imp->module); visited_count--; }\n')
out.append('static bool type_refs_equal(const AstTypeRef* a, const AstTypeRef* b) { if (a == b) return true; if (!a || !b) return false; if (a->parts && b->parts) { if (!str_eq(a->parts->text, b->parts->text)) return false; } else if (a->parts != b->parts) return false; if (a->is_opt != b->is_opt || a->is_view != b->is_view || a->is_mod != b->is_mod) return false; const AstTypeRef* aa = a->generic_args; const AstTypeRef* ab = b->generic_args; while (aa && ab) { if (!type_refs_equal(aa, ab)) return false; aa = aa->next; ab = ab->next; } return aa == NULL && ab == NULL; }\n')
out.append('static void register_generic_type(const AstTypeRef* type) { if (!type || !type->generic_args) return; for (size_t i = 0; i < g_generic_type_count; i++) if (type_refs_equal(g_generic_types[i], type)) return; if (g_generic_type_count < 512) { g_generic_types[g_generic_type_count] = type; g_generic_type_count++; } }\n')
out.append('static void collect_type_refs_expr(const AstExpr* e) { if(!e) return; if (e->kind == 13 && e->as.object_literal.type) register_generic_type(e->as.object_literal.type); if (e->kind == 11) collect_type_refs_expr(e->as.call.callee); }\n')
out.append('static void collect_type_refs_stmt(const AstStmt* s) { if(!s) return; if (s->kind == 2 && s->as.let_stmt.type) register_generic_type(s->as.let_stmt.type); if (s->kind == 1) collect_type_refs_expr(s->as.expr_stmt); }\n')
out.append('static void collect_type_refs_module(const AstModule* m) { (void)m; for (size_t i = 0; i < g_all_decl_count; i++) { const AstDecl* d = g_all_decls[i]; if (d->kind == 1) { if (d->as.func_decl.returns && d->as.func_decl.returns->type) register_generic_type(d->as.func_decl.returns->type); if (d->as.func_decl.body) { for (const AstStmt* s = d->as.func_decl.body->first; s; s = s->next) collect_type_refs_stmt(s); } } else if (d->kind == 2) { for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) if (f->type) register_generic_type(f->type); } } }\n')
out.append('static void emit_mangled_type_name(const AstTypeRef* type, FILE* out) { if (!type || !type->parts) { fprintf(out, "int64_t"); return; } Str base = type->parts->text; const char* prim = map_rae_type_to_c(base); if (prim && !type->generic_args) { fprintf(out, "%s", prim); return; } fprintf(out, "rae_%.*s", (int)base.len, base.data); if (type->generic_args) { fprintf(out, "_"); for (const AstTypeRef* a = type->generic_args; a; a = a->next) { Str ab = get_base_type_name(a); fprintf(out, "%.*s_", (int)ab.len, ab.data); } } }\n')
out.append('static bool emit_specialized_struct_def(const AstModule* module, const AstTypeRef* type, FILE* out, bool uses_raylib) { (void)uses_raylib; if (!type || !type->generic_args) return true; for (size_t i = 0; i < g_emitted_generic_type_count; i++) if (type_refs_equal(g_emitted_generic_types[i], type)) return true; if (g_emitted_generic_type_count < 512) { g_emitted_generic_types[g_emitted_generic_type_count] = type; g_emitted_generic_type_count++; } Str base = type->parts->text; const AstDecl* d = find_type_decl(module, base); if (!d || d->kind != 2) return true; fprintf(out, "typedef struct { "); const AstTypeField* f = d->as.type_decl.fields; while (f) { bool sub = 0; if (f->type && f->type->parts && !f->type->generic_args) { const AstTypeRef* arg = type->generic_args; for (const AstIdentifierPart* gp = d->as.type_decl.generic_params; gp && arg; gp = gp->next, arg = arg->next) { if (str_eq(gp->text, f->type->parts->text)) { emit_type_ref_as_c_type(NULL, arg, out); sub = 1; break; } } } if (!sub) emit_type_ref_as_c_type(NULL, f->type, out); fprintf(out, " %.*s; ", (int)f->name.len, f->name.data); f = f->next; } fprintf(out, "} "); emit_mangled_type_name(type, out); fprintf(out, ";\\n\\n"); return 1; }\n\n')

# Main loop
for line in lines:
    if "#include" in line or any(cl in line for cl in ctx_lines): continue
    
    if 'static bool emit_type_ref_as_c_type(CFuncContext* ctx, const AstTypeRef* type, FILE* out) {' in line:
        out.append(line)
        out.append('  if (type && type->generic_args) { emit_mangled_type_name(type, out); return true; }\n')
        continue

    if 'static bool emit_json_methods(const AstModule* module, FILE* out, bool uses_raylib) {' in line:
        out.append('static bool emit_json_methods(CFuncContext* ctx, FILE* out, bool uses_raylib) {\n')
        out.append('  (void)ctx; (void)uses_raylib;\n')
        out.append('  for (size_t i = 0; i < g_all_decl_count; i++) {\n')
        out.append('    const AstDecl* decl = g_all_decls[i];\n')
        out.append('    if (decl->kind == 2) {\n') # TYPE
        out.append('      const AstTypeDecl* td = &decl->as.type_decl;\n')
        out.append('      if (td->generic_params) continue;\n')
        out.append('      if (has_property(td->properties, "c_struct")) continue;\n')
        out.append('      if (is_raylib_builtin_type(td->name) || is_primitive_type(td->name)) continue;\n')
        out.append('      fprintf(out, "RAE_UNUSED static const char* rae_toJson_%.*s_(%.*s* this) { (void)this; return \\"{}\\"; }\\n", (int)td->name.len, td->name.data, (int)td->name.len, td->name.data);\n')
        out.append('    }\n')
        out.append('  }\n')
        out.append('  for (size_t i = 0; i < g_emitted_generic_type_count; i++) {\n')
        out.append('    fprintf(out, "static const char* rae_toJson_"); emit_mangled_type_name(g_emitted_generic_types[i], out); fprintf(out, "_(void* this) { (void)this; return \\"{}\\"; }\\n");\n')
        out.append('  }\n')
        out.append('  return true;\n')
        out.append('}\n')
        continue
    
    if 'for (const AstDecl* decl = module->decls; decl; decl = decl->next) {' in line:
         out.append('  for (size_t i = 0; i < g_all_decl_count; i++) {\n')
         out.append('    const AstDecl* decl = g_all_decls[i];\n')
         continue

    if 'if (find_type_decl(module, base)) {' in line:
        out.append('        } else if (find_type_decl(ctx->module, base)) {\n')
        continue
    if 'if (find_enum_decl(module, base)) {' in line:
        out.append('        } else if (find_enum_decl(ctx->module, base)) {\n')
        continue

    if 'bool c_backend_emit_module(const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib) {' in line:
        out.append(line)
        out.append('  g_emitted_json_decls_count = 0; g_emitted_json_impls_count = 0;\n')
        out.append('  g_generic_type_count = 0; g_emitted_generic_type_count = 0; g_all_decl_count = 0;\n')
        out.append('  collect_decls_from_module(module); collect_type_refs_module(module);\n')
        continue

    if 'static bool emit_enum_defs(const AstModule* module, FILE* out, bool uses_raylib) {' in line:
        out.append(line)
        out.append('  (void)module;\n')
        continue
    if 'static bool emit_struct_defs(const AstModule* module, FILE* out, bool uses_raylib) {' in line:
        out.append(line)
        out.append('  (void)module;\n')
        out.append('  for (size_t i = 0; i < g_generic_type_count; i++) emit_specialized_struct_def(module, g_generic_types[i], out, uses_raylib);\n')
        continue

    if 'if (!emit_json_methods(module, out, uses_raylib)) {' in line:
        out.append('  if (!emit_json_methods(&ctx, out, uses_raylib)) {\n')
        continue

    out.append(line)

with open('rae/compiler/src/c_backend.c', 'w') as f:
    f.writelines(out)
