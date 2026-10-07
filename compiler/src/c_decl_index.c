/* c_decl_index.c — a hash index over CompilerContext.all_decls.
 *
 * The program-wide declaration list is append-only (register_decl) and is
 * reset only when a new emit begins. The backend asks "which type/enum is
 * named N?" for nearly every expression it types, and answering by a scan of
 * the whole list made the emit quadratic in program size: 2.5x the code cost
 * ~9x the time, ~80% of it in find_type_decl/find_enum_decl. The index is
 * synced lazily from the list, so every lookup is O(1) and keeps the scan's
 * answer: the FIRST matching declaration in list order wins.
 *
 * It also answers "is this declaration already registered?" (register_decl's
 * dedup was a scan per insert) and "was this module collected?", which lets a
 * missed lookup skip the walk of a module's import tree that the list already
 * covers. */
#include "c_decl_index.h"

#include "c_backend_internal.h"
#include "sema.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  Str name;
  /* Index into all_decls + 1 (0 = none) of the first match of each kind */
  size_t firstTemplateType;
  size_t firstAnyType;
  size_t firstEnum;
  /* The first non-generic, non-extern unary `drop` / `copy` taking this type */
  size_t firstDrop;
  size_t firstCopy;
  /* The first module-level `let`/`var` of this name */
  size_t firstGlobal;
  /* Every function of this name, as all_decls indexes in list order */
  size_t* functions;
  size_t functionCount;
  size_t functionCap;
} DeclNameEntry;

typedef struct {
  const CompilerContext* owner;
  size_t indexedCount;
  DeclNameEntry* names;
  size_t nameCap;
  size_t nameCount;
  const void** pointers; /* registered decls and collected modules */
  size_t pointerCap;
  size_t pointerCount;
} DeclIndex;

static DeclIndex g_decl_index;

static uint64_t hash_str(Str name) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < name.len; i++) h = (h ^ (uint8_t)name.data[i]) * 1099511628211ULL;
  return h;
}

static uint64_t hash_pointer(const void* pointer) {
  uint64_t h = (uint64_t)(uintptr_t)pointer;
  h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
  return h;
}

static DeclNameEntry* name_slot(Str name, bool create) {
  if (g_decl_index.nameCap == 0) return NULL;
  size_t mask = g_decl_index.nameCap - 1;
  size_t slot = (size_t)hash_str(name) & mask;
  while (g_decl_index.names[slot].name.data) {
    if (str_eq(g_decl_index.names[slot].name, name)) return &g_decl_index.names[slot];
    slot = (slot + 1) & mask;
  }
  if (!create) return NULL;
  g_decl_index.names[slot].name = name;
  g_decl_index.nameCount++;
  return &g_decl_index.names[slot];
}

static bool pointer_insert(const void* pointer) {
  size_t mask = g_decl_index.pointerCap - 1;
  size_t slot = (size_t)hash_pointer(pointer) & mask;
  while (g_decl_index.pointers[slot]) {
    if (g_decl_index.pointers[slot] == pointer) return false;
    slot = (slot + 1) & mask;
  }
  g_decl_index.pointers[slot] = pointer;
  g_decl_index.pointerCount++;
  return true;
}

static bool pointer_contains(const void* pointer) {
  if (g_decl_index.pointerCap == 0) return false;
  size_t mask = g_decl_index.pointerCap - 1;
  size_t slot = (size_t)hash_pointer(pointer) & mask;
  while (g_decl_index.pointers[slot]) {
    if (g_decl_index.pointers[slot] == pointer) return true;
    slot = (slot + 1) & mask;
  }
  return false;
}

static void grow_names(void) {
  DeclNameEntry* old = g_decl_index.names;
  size_t oldCap = g_decl_index.nameCap;
  g_decl_index.nameCap = oldCap ? oldCap * 2 : 1024;
  g_decl_index.names = calloc(g_decl_index.nameCap, sizeof(DeclNameEntry));
  g_decl_index.nameCount = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!old[i].name.data) continue;
    *name_slot(old[i].name, true) = old[i];
  }
  free(old);
}

static void grow_pointers(void) {
  const void** old = g_decl_index.pointers;
  size_t oldCap = g_decl_index.pointerCap;
  g_decl_index.pointerCap = oldCap ? oldCap * 2 : 4096;
  g_decl_index.pointers = calloc(g_decl_index.pointerCap, sizeof(void*));
  g_decl_index.pointerCount = 0;
  for (size_t i = 0; i < oldCap; i++) if (old[i]) pointer_insert(old[i]);
  free(old);
}

void decl_index_reset(const CompilerContext* ctx) {
  for (size_t i = 0; i < g_decl_index.nameCap; i++) free(g_decl_index.names[i].functions);
  free(g_decl_index.names);
  free(g_decl_index.pointers);
  memset(&g_decl_index, 0, sizeof g_decl_index);
  g_decl_index.owner = ctx;
  grow_names();
  grow_pointers();
}

static void add_pointer(const void* pointer) {
  if ((g_decl_index.pointerCount + 1) * 2 > g_decl_index.pointerCap) grow_pointers();
  pointer_insert(pointer);
}

static void index_unary(const AstDecl* decl, size_t position) {
  const AstFuncDecl* func = &decl->as.func_decl;
  bool isDrop = str_eq_cstr(func->name, "drop");
  if (!isDrop && !str_eq_cstr(func->name, "copy")) return;
  if (func->generic_params || func->is_extern) return;
  const AstParam* first = func->params;
  if (!first || !first->type || first->next) return;
  Str base = get_base_type_name(first->type);
  if (base.len == 0) return;
  if ((g_decl_index.nameCount + 1) * 2 > g_decl_index.nameCap) grow_names();
  DeclNameEntry* entry = name_slot(base, true);
  size_t* slot = isDrop ? &entry->firstDrop : &entry->firstCopy;
  if (!*slot) *slot = position + 1;
}

static void index_function(const AstDecl* decl, size_t position) {
  Str name = decl->as.func_decl.name;
  if (!name.data) return;
  if ((g_decl_index.nameCount + 1) * 2 > g_decl_index.nameCap) grow_names();
  DeclNameEntry* entry = name_slot(name, true);
  if (entry->functionCount == entry->functionCap) {
    entry->functionCap = entry->functionCap ? entry->functionCap * 2 : 4;
    entry->functions = realloc(entry->functions, entry->functionCap * sizeof(size_t));
  }
  entry->functions[entry->functionCount++] = position;
}

static void index_decl(const AstDecl* decl, size_t position) {
  add_pointer(decl);
  if (decl->kind == AST_DECL_FUNC) {
    index_function(decl, position);
    index_unary(decl, position);
    return;
  }
  if (decl->kind == AST_DECL_GLOBAL_LET) {
    Str global = decl->as.let_decl.name;
    if (!global.data) return;
    if ((g_decl_index.nameCount + 1) * 2 > g_decl_index.nameCap) grow_names();
    DeclNameEntry* entry = name_slot(global, true);
    if (!entry->firstGlobal) entry->firstGlobal = position + 1;
    return;
  }
  Str name;
  if (decl->kind == AST_DECL_TYPE) name = decl->as.type_decl.name;
  else if (decl->kind == AST_DECL_ENUM) name = decl->as.enum_decl.name;
  else return;
  if (!name.data) return;
  if ((g_decl_index.nameCount + 1) * 2 > g_decl_index.nameCap) grow_names();
  DeclNameEntry* entry = name_slot(name, true);
  if (decl->kind == AST_DECL_ENUM) {
    if (!entry->firstEnum) entry->firstEnum = position + 1;
    return;
  }
  if (!entry->firstAnyType) entry->firstAnyType = position + 1;
  if (!decl->as.type_decl.specialization_args && !entry->firstTemplateType)
    entry->firstTemplateType = position + 1;
}

/* Brings the index up to date with the list: appended decls are added, and a
 * list that shrank (a new emit) is re-indexed from scratch. */
static void decl_index_sync(const CompilerContext* ctx) {
  if (g_decl_index.owner != ctx || ctx->all_decl_count < g_decl_index.indexedCount)
    decl_index_reset(ctx);
  while (g_decl_index.indexedCount < ctx->all_decl_count) {
    index_decl(ctx->all_decls[g_decl_index.indexedCount], g_decl_index.indexedCount);
    g_decl_index.indexedCount++;
  }
}

bool decl_index_contains(const CompilerContext* ctx, const AstDecl* decl) {
  decl_index_sync(ctx);
  return pointer_contains(decl);
}

/* A module whose decls (and, recursively, imports') were collected into the
 * list: a lookup that missed the list cannot find anything by walking it. */
void decl_index_note_module(const CompilerContext* ctx, const AstModule* module) {
  decl_index_sync(ctx);
  add_pointer(module);
}

bool decl_index_module_collected(const CompilerContext* ctx, const AstModule* module) {
  if (ctx->all_decl_count >= ctx->all_decl_cap) return false; /* the list overflowed */
  decl_index_sync(ctx);
  return pointer_contains(module);
}

/* types_match also equates String with its C spellings */
static const char* const kStringSpellings[] = { "String", "const char*", "rae_String", "const_char_p" };

static size_t first_position(Str name, int kind) {
  DeclNameEntry* entry = name_slot(name, false);
  if (!entry) return 0;
  if (kind == 0) return entry->firstTemplateType;
  if (kind == 1) return entry->firstAnyType;
  return entry->firstEnum;
}

/* kind: 0 = type that is not a specialization clone, 1 = any type, 2 = enum */
static const AstDecl* lookup(const CompilerContext* ctx, Str name, int kind) {
  decl_index_sync(ctx);
  size_t best = first_position(name, kind);
  bool isString = str_eq_cstr(name, "String");
  bool isStringSpelling = false;
  for (size_t i = 1; i < 4; i++) if (str_eq_cstr(name, kStringSpellings[i])) isStringSpelling = true;
  if (isString || isStringSpelling) {
    for (size_t i = 0; i < 4; i++) {
      if (isString == (i == 0)) continue; /* String ~ C spellings, not C ~ C */
      size_t position = first_position(str_from_cstr(kStringSpellings[i]), kind);
      if (position && (!best || position < best)) best = position;
    }
  }
  return best ? ctx->all_decls[best - 1] : NULL;
}

const AstDecl* decl_index_find_type(const CompilerContext* ctx, Str name, bool templateOnly) {
  return lookup(ctx, name, templateOnly ? 0 : 1);
}

const AstDecl* decl_index_find_enum(const CompilerContext* ctx, Str name) {
  return lookup(ctx, name, 2);
}

const AstDecl* decl_index_find_global(const CompilerContext* ctx, Str name) {
  decl_index_sync(ctx);
  DeclNameEntry* entry = name_slot(name, false);
  return entry && entry->firstGlobal ? ctx->all_decls[entry->firstGlobal - 1] : NULL;
}

/* The all_decls indexes of the functions named `name`, in list order. The
 * array is valid until the next decl is registered. */
const size_t* decl_index_functions(const CompilerContext* ctx, Str name, size_t* count) {
  decl_index_sync(ctx);
  DeclNameEntry* entry = name_slot(name, false);
  *count = entry ? entry->functionCount : 0;
  return entry ? entry->functions : NULL;
}

/* find_named_unary_for: the user `drop` (isDrop) or `copy` of a type */
const AstFuncDecl* decl_index_find_unary(const CompilerContext* ctx, bool isDrop, Str base) {
  decl_index_sync(ctx);
  DeclNameEntry* entry = name_slot(base, false);
  if (!entry) return NULL;
  size_t position = isDrop ? entry->firstDrop : entry->firstCopy;
  return position ? &ctx->all_decls[position - 1]->as.func_decl : NULL;
}

/* The import-graph walk of find_type_decl / find_enum_decl (the lookups that
 * search only what a module can see): one generation per walk, and a module
 * is searched again only when reached at a shallower depth (the walks are
 * depth-limited, and a shallower visit reaches more). */
typedef struct {
  const AstModule** modules;
  unsigned* generations;
  int* depths;
  size_t cap;
  size_t count;
  unsigned generation;
} ModuleVisits;

static ModuleVisits g_module_visits;

void decl_visit_begin(void) {
  g_module_visits.generation++;
}

static void grow_visits(void) {
  const AstModule** oldModules = g_module_visits.modules;
  unsigned* oldGenerations = g_module_visits.generations;
  int* oldDepths = g_module_visits.depths;
  size_t oldCap = g_module_visits.cap;
  g_module_visits.cap = oldCap ? oldCap * 2 : 1024;
  g_module_visits.modules = calloc(g_module_visits.cap, sizeof(AstModule*));
  g_module_visits.generations = calloc(g_module_visits.cap, sizeof(unsigned));
  g_module_visits.depths = calloc(g_module_visits.cap, sizeof(int));
  g_module_visits.count = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (!oldModules[i]) continue;
    size_t mask = g_module_visits.cap - 1;
    size_t slot = (size_t)hash_pointer(oldModules[i]) & mask;
    while (g_module_visits.modules[slot]) slot = (slot + 1) & mask;
    g_module_visits.modules[slot] = oldModules[i];
    g_module_visits.generations[slot] = oldGenerations[i];
    g_module_visits.depths[slot] = oldDepths[i];
    g_module_visits.count++;
  }
  free(oldModules);
  free(oldGenerations);
  free(oldDepths);
}

bool decl_visit_first(const AstModule* module, int depth) {
  if ((g_module_visits.count + 1) * 2 > g_module_visits.cap) grow_visits();
  size_t mask = g_module_visits.cap - 1;
  size_t slot = (size_t)hash_pointer(module) & mask;
  while (g_module_visits.modules[slot] && g_module_visits.modules[slot] != module)
    slot = (slot + 1) & mask;
  if (!g_module_visits.modules[slot]) {
    g_module_visits.modules[slot] = module;
    g_module_visits.count++;
  } else if (g_module_visits.generations[slot] == g_module_visits.generation
             && g_module_visits.depths[slot] <= depth) {
    return false;
  }
  g_module_visits.generations[slot] = g_module_visits.generation;
  g_module_visits.depths[slot] = depth;
  return true;
}

/* One module's OWN decls by name, for the lookups that walk a module's import
 * graph (find_type_decl(NULL, module, ...) — what a module can see). Scanning
 * each module's list per call made every ownership question (does this type
 * cascade a drop? need a deep copy?) cost a pass over the whole program.
 * Specialization clones are PREPENDED to a module's list, so the index keeps
 * the head it saw and takes in what was added in front of it; the first decl
 * in list order wins, as in the scan. */
typedef struct {
  Str name;
  const AstDecl* templateType;
  const AstDecl* anyType;
  const AstDecl* enumDecl;
  const AstDecl* alias;
  /* The functions of this name in REVERSE list order: decls are entered
   * last-to-first and clones prepended later are entered after, so an append
   * keeps the order and a reader's snapshot (the first `count`) stays valid */
  const AstDecl** functions;
  size_t functionCount;
  size_t functionCap;
} ModuleNameEntry;

typedef struct {
  const AstModule* module;
  const AstDecl* head;
  ModuleNameEntry* names;
  size_t cap;
  size_t count;
} ModuleIndex;

typedef struct {
  ModuleIndex* indices;
  size_t cap;
  size_t count;
} ModuleIndexTable;

static ModuleIndexTable g_module_indices;

static ModuleNameEntry* module_name_slot(ModuleIndex* index, Str name, bool create) {
  size_t mask = index->cap - 1;
  size_t slot = (size_t)hash_str(name) & mask;
  while (index->names[slot].name.data) {
    if (str_eq(index->names[slot].name, name)) return &index->names[slot];
    slot = (slot + 1) & mask;
  }
  if (!create) return NULL;
  index->names[slot].name = name;
  index->count++;
  return &index->names[slot];
}

static void module_index_grow(ModuleIndex* index) {
  ModuleNameEntry* old = index->names;
  size_t oldCap = index->cap;
  index->cap = oldCap ? oldCap * 2 : 64;
  index->names = calloc(index->cap, sizeof(ModuleNameEntry));
  index->count = 0;
  for (size_t i = 0; i < oldCap; i++) {
    if (old[i].name.data) *module_name_slot(index, old[i].name, true) = old[i];
  }
  free(old);
}

/* Entered last-to-first, so an earlier decl in the list overwrites a later one */
static void module_index_enter(ModuleIndex* index, const AstDecl* decl) {
  Str name;
  if (decl->kind == AST_DECL_FUNC) {
    name = decl->as.func_decl.name;
    if (!name.data) return;
    if ((index->count + 1) * 2 > index->cap) module_index_grow(index);
    ModuleNameEntry* entry = module_name_slot(index, name, true);
    if (entry->functionCount == entry->functionCap) {
      entry->functionCap = entry->functionCap ? entry->functionCap * 2 : 4;
      entry->functions = realloc(entry->functions, entry->functionCap * sizeof(AstDecl*));
    }
    entry->functions[entry->functionCount++] = decl;
    return;
  }
  if (decl->kind == AST_DECL_TYPE) name = decl->as.type_decl.name;
  else if (decl->kind == AST_DECL_ENUM) name = decl->as.enum_decl.name;
  else if (decl->kind == AST_DECL_ALIAS) name = decl->as.alias_decl.name;
  else return;
  if (!name.data) return;
  if ((index->count + 1) * 2 > index->cap) module_index_grow(index);
  ModuleNameEntry* entry = module_name_slot(index, name, true);
  if (decl->kind == AST_DECL_ENUM) { entry->enumDecl = decl; return; }
  if (decl->kind == AST_DECL_ALIAS) { entry->alias = decl; return; }
  entry->anyType = decl;
  if (!decl->as.type_decl.specialization_args) entry->templateType = decl;
}

/* Enters the decls from the list head up to (not including) `stop`, last first */
static bool module_index_enter_until(ModuleIndex* index, const AstDecl* stop) {
  size_t count = 0;
  const AstDecl* decl = index->module->decls;
  for (; decl && decl != stop; decl = decl->next) count++;
  if (decl != stop) return false; /* the old head is gone: the list was replaced */
  const AstDecl** added = malloc((count ? count : 1) * sizeof(AstDecl*));
  size_t i = 0;
  for (decl = index->module->decls; decl != stop; decl = decl->next) added[i++] = decl;
  while (i > 0) module_index_enter(index, added[--i]);
  free(added);
  return true;
}

static ModuleIndex* module_index_for(const AstModule* module) {
  if ((g_module_indices.count + 1) * 2 > g_module_indices.cap) {
    ModuleIndex* old = g_module_indices.indices;
    size_t oldCap = g_module_indices.cap;
    g_module_indices.cap = oldCap ? oldCap * 2 : 512;
    g_module_indices.indices = calloc(g_module_indices.cap, sizeof(ModuleIndex));
    for (size_t i = 0; i < oldCap; i++) {
      if (!old[i].module) continue;
      size_t slot = (size_t)hash_pointer(old[i].module) & (g_module_indices.cap - 1);
      while (g_module_indices.indices[slot].module) slot = (slot + 1) & (g_module_indices.cap - 1);
      g_module_indices.indices[slot] = old[i];
    }
    free(old);
  }
  size_t mask = g_module_indices.cap - 1;
  size_t slot = (size_t)hash_pointer(module) & mask;
  while (g_module_indices.indices[slot].module && g_module_indices.indices[slot].module != module)
    slot = (slot + 1) & mask;
  ModuleIndex* index = &g_module_indices.indices[slot];
  if (!index->module) {
    index->module = module;
    g_module_indices.count++;
    module_index_grow(index);
    module_index_enter_until(index, NULL);
    index->head = module->decls;
    return index;
  }
  if (index->head != module->decls) {
    if (!module_index_enter_until(index, index->head)) {
      for (size_t i = 0; i < index->cap; i++) free(index->names[i].functions);
      free(index->names);
      index->names = NULL; index->cap = 0; index->count = 0;
      module_index_grow(index);
      module_index_enter_until(index, NULL);
    }
    index->head = module->decls;
  }
  return index;
}

static ModuleNameEntry* module_entry(const AstModule* module, Str name) {
  ModuleIndex* index = module_index_for(module);
  ModuleNameEntry* entry = module_name_slot(index, name, false);
  if (entry) return entry;
  /* types_match: String and its C spellings name the same type */
  if (str_eq_cstr(name, "String")) {
    for (size_t i = 1; i < 4 && !entry; i++)
      entry = module_name_slot(index, str_from_cstr(kStringSpellings[i]), false);
  } else {
    for (size_t i = 1; i < 4; i++)
      if (str_eq_cstr(name, kStringSpellings[i]))
        return module_name_slot(index, str_from_cstr("String"), false);
  }
  return entry;
}

const AstDecl* module_index_find_type(const AstModule* module, Str name) {
  ModuleNameEntry* entry = module_entry(module, name);
  if (!entry) return NULL;
  return entry->templateType ? entry->templateType : entry->anyType;
}

const AstDecl* module_index_find_enum(const AstModule* module, Str name) {
  ModuleNameEntry* entry = module_entry(module, name);
  return entry ? entry->enumDecl : NULL;
}

/* The functions named `name` of one module, read in list order as
 *   n = module_index_function_count(module, name);
 *   for (k = 0; k < n; k++) decl = module_index_function_at(module, name, n, k);
 * A clone prepended while the loop runs is not visited, as when the loop
 * walked module->decls. */
size_t module_index_function_count(const AstModule* module, Str name) {
  ModuleNameEntry* entry = module_name_slot(module_index_for(module), name, false);
  return entry ? entry->functionCount : 0;
}

const AstDecl* module_index_function_at(const AstModule* module, Str name, size_t count, size_t k) {
  ModuleNameEntry* entry = module_name_slot(module_index_for(module), name, false);
  return entry->functions[count - 1 - k];
}

/* An alias is matched by its exact name (no String spellings) */
const AstTypeRef* module_index_find_alias(const AstModule* module, Str name) {
  ModuleNameEntry* entry = module_name_slot(module_index_for(module), name, false);
  return entry && entry->alias ? entry->alias->as.alias_decl.target : NULL;
}

/* CompilerContext.generic_types (append-only), for register_generic_type's
 * "is this type already registered?" — a scan of the list per registration.
 * type_refs_equal holds when two refs share a resolved type OR match by
 * written name, so a ref is filed under both, and the caller's own test runs
 * on every candidate: a bucket collision only adds a candidate, never a
 * wrong answer. Sema registers refs before resolving them, and resolving an
 * alias rewrites the ref's name in place, so a ref filed unresolved waits on a
 * pending list and is filed again (resolved type and current name) once it
 * has a resolved type. */
typedef struct {
  const CompilerContext* owner;
  size_t indexedCount;
  size_t bucketCount;
  size_t* nameHeads;     /* index + 1 into generic_types, 0 = empty */
  size_t* resolvedHeads;
  size_t* nameNext;
  size_t* resolvedNext;
  /* A second name link, for a pending ref filed again under its new name */
  size_t* renamedNext;
  size_t* renamedHeads;
  size_t nextCap;
  size_t* pending;
  size_t pendingCount;
} GenericIndex;

static GenericIndex g_generic_index;

static size_t generic_name_bucket(const AstTypeRef* type) {
  uint64_t h;
  if (!type->parts) h = 1;
  else if ((uintptr_t)type->parts < 0x1000) h = 2;
  else h = hash_str(type->parts->text);
  return (size_t)h & (g_generic_index.bucketCount - 1);
}

static size_t generic_resolved_bucket(const TypeInfo* resolved) {
  return (size_t)hash_pointer(resolved) & (g_generic_index.bucketCount - 1);
}

static void generic_index_file_resolved(const CompilerContext* ctx, size_t i) {
  size_t resolved = generic_resolved_bucket(ctx->generic_types[i]->resolved_type);
  g_generic_index.resolvedNext[i] = g_generic_index.resolvedHeads[resolved];
  g_generic_index.resolvedHeads[resolved] = i + 1;
}

static void generic_index_file(const CompilerContext* ctx, size_t i) {
  const AstTypeRef* type = ctx->generic_types[i];
  size_t name = generic_name_bucket(type);
  g_generic_index.nameNext[i] = g_generic_index.nameHeads[name];
  g_generic_index.nameHeads[name] = i + 1;
  if (type->resolved_type) generic_index_file_resolved(ctx, i);
  else g_generic_index.pending[g_generic_index.pendingCount++] = i;
}

/* Files the pending refs that have been resolved since */
static void generic_index_settle(const CompilerContext* ctx) {
  size_t kept = 0;
  for (size_t k = 0; k < g_generic_index.pendingCount; k++) {
    size_t i = g_generic_index.pending[k];
    const AstTypeRef* type = ctx->generic_types[i];
    if (!type->resolved_type) { g_generic_index.pending[kept++] = i; continue; }
    generic_index_file_resolved(ctx, i);
    size_t name = generic_name_bucket(type);
    g_generic_index.renamedNext[i] = g_generic_index.renamedHeads[name];
    g_generic_index.renamedHeads[name] = i + 1;
  }
  g_generic_index.pendingCount = kept;
}

static void generic_index_sync(const CompilerContext* ctx) {
  size_t count = ctx->generic_type_count;
  bool rebuild = g_generic_index.owner != ctx || count < g_generic_index.indexedCount
      || count * 2 > g_generic_index.bucketCount || count > g_generic_index.nextCap;
  if (rebuild) {
    free(g_generic_index.nameHeads); free(g_generic_index.resolvedHeads);
    free(g_generic_index.nameNext); free(g_generic_index.resolvedNext);
    free(g_generic_index.renamedHeads); free(g_generic_index.renamedNext);
    free(g_generic_index.pending);
    memset(&g_generic_index, 0, sizeof g_generic_index);
    g_generic_index.owner = ctx;
    g_generic_index.bucketCount = 1024;
    while (g_generic_index.bucketCount < count * 4) g_generic_index.bucketCount <<= 1;
    g_generic_index.nextCap = g_generic_index.bucketCount;
    g_generic_index.nameHeads = calloc(g_generic_index.bucketCount, sizeof(size_t));
    g_generic_index.resolvedHeads = calloc(g_generic_index.bucketCount, sizeof(size_t));
    g_generic_index.nameNext = calloc(g_generic_index.nextCap, sizeof(size_t));
    g_generic_index.resolvedNext = calloc(g_generic_index.nextCap, sizeof(size_t));
    g_generic_index.renamedHeads = calloc(g_generic_index.bucketCount, sizeof(size_t));
    g_generic_index.renamedNext = calloc(g_generic_index.nextCap, sizeof(size_t));
    g_generic_index.pending = calloc(g_generic_index.nextCap, sizeof(size_t));
  }
  while (g_generic_index.indexedCount < count) {
    generic_index_file(ctx, g_generic_index.indexedCount);
    g_generic_index.indexedCount++;
  }
}

bool generic_index_any(const CompilerContext* ctx, const AstTypeRef* type,
                       GenericTypeMatch matches, void* data) {
  generic_index_sync(ctx);
  generic_index_settle(ctx);
  size_t name = generic_name_bucket(type);
  for (size_t at = g_generic_index.nameHeads[name]; at; at = g_generic_index.nameNext[at - 1]) {
    if (matches(ctx->generic_types[at - 1], type, data)) return true;
  }
  for (size_t at = g_generic_index.renamedHeads[name]; at; at = g_generic_index.renamedNext[at - 1]) {
    if (matches(ctx->generic_types[at - 1], type, data)) return true;
  }
  if (!type->resolved_type) return false;
  for (size_t at = g_generic_index.resolvedHeads[generic_resolved_bucket(type->resolved_type)]; at;
       at = g_generic_index.resolvedNext[at - 1]) {
    if (matches(ctx->generic_types[at - 1], type, data)) return true;
  }
  return false;
}

/* sema_is_module_name: the names that qualify as a module from a given module
 * (its decls' module names, file components, folder namespaces, and its
 * imports' module names). Asked for nearly every member expression, it
 * re-walked those decl lists and rebuilt each decl's names every time. The
 * set is built once per module and rebuilt only when a decl list it was built
 * from has changed (a new head, or a different import list). */
typedef struct {
  const AstModule* module;
  uint64_t signature;
  char** names;
  size_t cap;
  size_t count;
} ModuleNameSet;

typedef struct {
  ModuleNameSet* sets;
  size_t cap;
  size_t count;
} ModuleNameSets;

static ModuleNameSets g_module_name_sets;
static ModuleNameSet* g_collecting_set;

static uint64_t module_names_signature(const AstModule* module) {
  uint64_t h = hash_pointer(module->decls) ^ 0x9e3779b97f4a7c15ULL;
  for (const AstImport* imp = module->imports; imp; imp = imp->next) {
    h = (h * 1099511628211ULL) ^ hash_pointer(imp->module);
    if (imp->module) h = (h * 1099511628211ULL) ^ hash_pointer(imp->module->decls);
  }
  return h;
}

static bool name_set_has(const ModuleNameSet* set, const char* name, size_t len) {
  if (set->cap == 0) return false;
  size_t mask = set->cap - 1;
  size_t slot = (size_t)hash_str((Str){ .data = name, .len = len }) & mask;
  while (set->names[slot]) {
    if (strlen(set->names[slot]) == len && memcmp(set->names[slot], name, len) == 0) return true;
    slot = (slot + 1) & mask;
  }
  return false;
}

static void name_set_insert(ModuleNameSet* set, char* owned) {
  size_t len = strlen(owned);
  size_t mask = set->cap - 1;
  size_t slot = (size_t)hash_str((Str){ .data = owned, .len = len }) & mask;
  while (set->names[slot]) slot = (slot + 1) & mask;
  set->names[slot] = owned;
  set->count++;
}

static void module_name_add(const char* name, size_t len) {
  ModuleNameSet* set = g_collecting_set;
  if (name_set_has(set, name, len)) return;
  if ((set->count + 1) * 2 > set->cap) {
    char** old = set->names;
    size_t oldCap = set->cap;
    set->cap = oldCap ? oldCap * 2 : 16;
    set->names = calloc(set->cap, sizeof(char*));
    set->count = 0;
    for (size_t i = 0; i < oldCap; i++) if (old[i]) name_set_insert(set, old[i]);
    free(old);
  }
  char* owned = malloc(len + 1);
  memcpy(owned, name, len);
  owned[len] = '\0';
  name_set_insert(set, owned);
}

static ModuleNameSet* module_name_set_for(const AstModule* module) {
  if ((g_module_name_sets.count + 1) * 2 > g_module_name_sets.cap) {
    ModuleNameSet* old = g_module_name_sets.sets;
    size_t oldCap = g_module_name_sets.cap;
    g_module_name_sets.cap = oldCap ? oldCap * 2 : 256;
    g_module_name_sets.sets = calloc(g_module_name_sets.cap, sizeof(ModuleNameSet));
    for (size_t i = 0; i < oldCap; i++) {
      if (!old[i].module) continue;
      size_t slot = (size_t)hash_pointer(old[i].module) & (g_module_name_sets.cap - 1);
      while (g_module_name_sets.sets[slot].module) slot = (slot + 1) & (g_module_name_sets.cap - 1);
      g_module_name_sets.sets[slot] = old[i];
    }
    free(old);
  }
  size_t mask = g_module_name_sets.cap - 1;
  size_t slot = (size_t)hash_pointer(module) & mask;
  while (g_module_name_sets.sets[slot].module && g_module_name_sets.sets[slot].module != module)
    slot = (slot + 1) & mask;
  ModuleNameSet* set = &g_module_name_sets.sets[slot];
  if (!set->module) {
    set->module = module;
    set->signature = ~module_names_signature(module); /* never matches: built below */
    g_module_name_sets.count++;
  }
  return set;
}

bool module_names_contain(const AstModule* module, Str name, ModuleNameCollector collect) {
  ModuleNameSet* set = module_name_set_for(module);
  uint64_t signature = module_names_signature(module);
  if (set->signature != signature) {
    for (size_t i = 0; i < set->cap; i++) free(set->names[i]);
    free(set->names);
    set->names = NULL; set->cap = 0; set->count = 0;
    g_collecting_set = set;
    collect(module, module_name_add);
    g_collecting_set = NULL;
    set->signature = signature;
  }
  return name_set_has(set, name.data, name.len);
}

/* A plain name -> pointer map (sema's symbol table index). A NULL value is the
 * same as no entry. */
struct NameMap {
  Str* names;
  void** values;
  size_t cap;
  size_t count;
};

NameMap* name_map_create(void) {
  NameMap* map = calloc(1, sizeof(NameMap));
  map->cap = 256;
  map->names = calloc(map->cap, sizeof(Str));
  map->values = calloc(map->cap, sizeof(void*));
  return map;
}

static size_t name_map_slot(const NameMap* map, Str name) {
  size_t mask = map->cap - 1;
  size_t slot = (size_t)hash_str(name) & mask;
  while (map->names[slot].data && !str_eq(map->names[slot], name)) slot = (slot + 1) & mask;
  return slot;
}

void* name_map_get(const NameMap* map, Str name) {
  if (!map || !name.data) return NULL;
  size_t slot = name_map_slot(map, name);
  return map->names[slot].data ? map->values[slot] : NULL;
}

void name_map_set(NameMap* map, Str name, void* value) {
  if (!name.data) return;
  if ((map->count + 1) * 2 > map->cap) {
    Str* oldNames = map->names;
    void** oldValues = map->values;
    size_t oldCap = map->cap;
    map->cap *= 2;
    map->names = calloc(map->cap, sizeof(Str));
    map->values = calloc(map->cap, sizeof(void*));
    for (size_t i = 0; i < oldCap; i++) {
      if (!oldNames[i].data) continue;
      size_t slot = name_map_slot(map, oldNames[i]);
      map->names[slot] = oldNames[i];
      map->values[slot] = oldValues[i];
    }
    free(oldNames);
    free(oldValues);
  }
  size_t slot = name_map_slot(map, name);
  if (!map->names[slot].data) { map->names[slot] = name; map->count++; }
  map->values[slot] = value;
}

/* A set of pointers (sema's "already analyzed" decls) */
struct PointerSet {
  const void** items;
  size_t cap;
  size_t count;
};

PointerSet* pointer_set_create(void) {
  PointerSet* set = calloc(1, sizeof(PointerSet));
  set->cap = 1024;
  set->items = calloc(set->cap, sizeof(void*));
  return set;
}

void pointer_set_free(PointerSet* set) {
  if (!set) return;
  free(set->items);
  free(set);
}

bool pointer_set_contains(const PointerSet* set, const void* pointer) {
  size_t mask = set->cap - 1;
  size_t slot = (size_t)hash_pointer(pointer) & mask;
  while (set->items[slot]) {
    if (set->items[slot] == pointer) return true;
    slot = (slot + 1) & mask;
  }
  return false;
}

void pointer_set_add(PointerSet* set, const void* pointer) {
  if ((set->count + 1) * 2 > set->cap) {
    const void** old = set->items;
    size_t oldCap = set->cap;
    set->cap *= 2;
    set->items = calloc(set->cap, sizeof(void*));
    set->count = 0;
    for (size_t i = 0; i < oldCap; i++) if (old[i]) pointer_set_add(set, old[i]);
    free(old);
  }
  size_t mask = set->cap - 1;
  size_t slot = (size_t)hash_pointer(pointer) & mask;
  while (set->items[slot]) {
    if (set->items[slot] == pointer) return;
    slot = (slot + 1) & mask;
  }
  set->items[slot] = pointer;
  set->count++;
}
