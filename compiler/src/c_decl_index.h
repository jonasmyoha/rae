/* c_decl_index.h — name indexes over declarations (c_decl_index.c), so the
 * lookups the compiler runs per expression are O(1) instead of a scan of the
 * program or of a module's import graph. Every lookup keeps the answer of the
 * scan it replaced: the first declaration in list order wins. */
#ifndef RAE_C_DECL_INDEX_H
#define RAE_C_DECL_INDEX_H

#include <stdbool.h>

#include "ast.h"

/* CompilerContext.all_decls, program-wide */
bool decl_index_contains(const CompilerContext* ctx, const AstDecl* decl);
void decl_index_note_module(const CompilerContext* ctx, const AstModule* module);
bool decl_index_module_collected(const CompilerContext* ctx, const AstModule* module);
const AstDecl* decl_index_find_type(const CompilerContext* ctx, Str name, bool templateOnly);
const AstDecl* decl_index_find_enum(const CompilerContext* ctx, Str name);
/* The all_decls indexes of the functions named `name`, in list order (valid
 * until the next decl is registered) */
const size_t* decl_index_functions(const CompilerContext* ctx, Str name, size_t* count);
/* The user `drop` (isDrop) or `copy` taking a type */
const AstFuncDecl* decl_index_find_unary(const CompilerContext* ctx, bool isDrop, Str base);

/* CompilerContext.generic_types: is any registered type `matches` with
 * `type`? Only the refs that could be type_refs_equal to it are offered. */
typedef bool (*GenericTypeMatch)(const AstTypeRef* other, const AstTypeRef* type, void* data);
bool generic_index_any(const CompilerContext* ctx, const AstTypeRef* type,
                       GenericTypeMatch matches, void* data);

/* A visited mark per import-graph walk: begin a walk, then ask per module at
 * its import depth. A module is walked again only when reached shallower than
 * before, so a depth-limited walk reaches exactly what it did without marks. */
void decl_visit_begin(void);
bool decl_visit_first(const AstModule* module, int depth);

/* One module's own decls by name */
const AstDecl* module_index_find_type(const AstModule* module, Str name);
const AstDecl* module_index_find_enum(const AstModule* module, Str name);
const AstTypeRef* module_index_find_alias(const AstModule* module, Str name);

/* A per-module set of names, built by `collect` (which calls `add` per name)
 * and rebuilt when the module's or an import's decl list changes */
typedef void (*ModuleNameAdd)(const char* name, size_t len);
typedef void (*ModuleNameCollector)(const AstModule* module, ModuleNameAdd add);
bool module_names_contain(const AstModule* module, Str name, ModuleNameCollector collect);

/* A name -> pointer map; a NULL value is the same as no entry */
typedef struct NameMap NameMap;
NameMap* name_map_create(void);
void* name_map_get(const NameMap* map, Str name);
void name_map_set(NameMap* map, Str name, void* value);

#endif
