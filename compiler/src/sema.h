#ifndef RAE_SEMA_H
#define RAE_SEMA_H

#include "ast.h"
#include <stdbool.h>

// Main entry point for semantic analysis
// Returns true if analysis succeeded, false if errors were found
bool sema_analyze_module(CompilerContext* ctx, AstModule* module);

// Per-file import/open scope (docs/module-namespacing.md). Call reset once, then
// register each source file's import directives (from the module graph) before
// sema, so name resolution can honour per-file aliases and import/open.
void sema_reset_file_scopes(void);
void sema_register_file_imports(Arena* arena, const char* file, AstImport* imports);
// Register a prelude package (auto-loaded => opened for every file).
void sema_register_global_open(Arena* arena, const char* module);
/* A module every file IMPORTS without opening it (lib/Target.rae): reached
 * qualified only */
void sema_register_global_import(Arena* arena, const char* module);

// Resolves a type reference (AstTypeRef) to a canonical TypeInfo*
TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref);

// Specialization helpers used by backends
/* A written-form type ref for a resolved type (`List(Int)`, with its generic
 * arguments), for code that needs the spelling, not just the TypeInfo */
AstTypeRef* sema_type_ref_of(CompilerContext* ctx, TypeInfo* t);
AstTypeRef* substitute_type_ref(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type);

// #773 compile-time field reflection through a generic world parameter. When a
// generic function's body contains a `loop ... in fields(world)`, the field set
// depends on the concrete instantiation, so a backend emitting one concrete
// specialization must expand the loop AFTER substitution. Returns a CLONE of
// `template_body` with every fields() loop unrolled for the given concrete
// generic args, or NULL if the body has no fields() loop (emit the body as-is).
AstBlock* reflect_instantiate_body(CompilerContext* ctx, const AstModule* module,
        const AstBlock* template_body, const AstParam* params,
        const AstIdentifierPart* gparams, const AstTypeRef* concrete_args);
AstTypeRef* infer_generic_args(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef* pattern, const AstTypeRef* concrete_type);
AstTypeRef* infer_generic_args_multi(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef** patterns, const AstTypeRef** concretes, size_t pair_count);
Str get_base_type_name(const AstTypeRef* type);
// `mainLoop` placement, state and body rules (sema_main_loop.c).
void sema_check_main_loops(AstModule* module, const char* file, const AstFuncDecl* func);
Str get_decl_name(const AstDecl* d);

/* The visibility rules of a bare name (docs/module-namespacing.md), for the
 * passes outside sema.c that resolve names by them (type_names.c). */
bool sema_decl_bare_visible(const char* file, const AstDecl* d);     /* own module, opened, project, prelude */
bool sema_decl_opened_explicitly(const char* file, const AstDecl* d);
bool sema_decl_reachable(const char* file, const AstDecl* d);        /* bare-visible or imported */
bool sema_decl_in_lib(const AstDecl* d);
bool sema_decl_in_prelude(const AstDecl* d);
bool sema_file_names_module(const char* file, Str qualifier);     /* `import X` / `open X` / `as X` */
/* Does `file` reach d's module through `qualifier` (`import a/X` or `as X`)? */
bool sema_qualifier_reaches(const char* file, Str qualifier, const AstDecl* d);

/* type_names.c: two modules may declare the same type or enum name. Before
 * sema, every such declaration but one gets an internal name that carries
 * its module, and every use is pointed at the declaration the using file
 * sees (an error when two are visible bare). type_written_name gives back
 * the name as written, for anything a program reads as text. */
void sema_separate_clashing_type_names(CompilerContext* ctx, AstModule* module);
Str type_written_name(Str name);

#endif // RAE_SEMA_H
