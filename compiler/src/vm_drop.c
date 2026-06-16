/* Live VM cascade-drop synthesis — see vm_drop.h.
 *
 * One shared C callback (`drop_native_cb`) handles every registered
 * helper; per-(type × variant) behaviour comes from a
 * `RaeVmDropDescriptor*` passed through `user_data`.
 *
 * Synthesises helpers for two families of user struct types:
 *
 *   1. Non-generic user structs (`type Outer { ... }`). Naming:
 *      `rae_vm_drop_struct_<TypeName>[_alias]`.
 *
 *   2. Concrete specializations of generic user structs
 *      (`Wrapper(String)`, `Pair(String, List(String))`, …).
 *      Discovered via the C backend's existing
 *      `ctx->generic_types[]` list, which `vm_compile_module`
 *      populates by invoking `discover_specializations_module`.
 *      Naming: `rae_vm_drop_struct_<MangledType>[_alias]` where
 *      `<MangledType>` is the mangler output with the leading
 *      `rae_` prefix stripped (so the non-generic case stays
 *      ergonomic and the spec case is unambiguous).
 *
 * The descriptor's field walk substitutes generic params with the
 * concrete spec args so nested cascades resolve correctly. Nested
 * user-struct fields (whether non-generic or themselves
 * specialized) get linked via descriptor pointers; both layers must
 * be registered for the link to resolve.
 */
#include "vm_drop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "c_backend.h"           /* register_decl / collect_decls_from_module */
#include "c_backend_internal.h"  /* has_property (AST utility) */
#include "mangler.h"
#include "ownership.h"
#include "sema.h"
#include "str.h"
#include "vm_registry.h"
#include "vm_value.h"

/* What to do with a single field during the cascade walk. */
typedef enum {
  DROP_F_SKIP,
  DROP_F_FREE,
  DROP_F_NESTED,
} DropFieldKind;

struct RaeVmDropDescriptor;

typedef struct {
  DropFieldKind kind;
  const struct RaeVmDropDescriptor* nested;
} DropFieldSpec;

typedef struct RaeVmDropDescriptor {
  char* name;
  char* type_name;
  bool is_alias;
  size_t field_count;
  DropFieldSpec* fields;
  size_t invocations;
} RaeVmDropDescriptor;

static bool drop_native_cb(struct VM* vm,
                           VmNativeResult* out_result,
                           const Value* args,
                           size_t arg_count,
                           void* user_data) {
  (void)vm;
  if (out_result) out_result->has_value = false;
  if (!user_data) return true;
  RaeVmDropDescriptor* d = (RaeVmDropDescriptor*)user_data;
  d->invocations++;
  if (arg_count == 0) return true;
  Value* target = NULL;
  const Value* first = &args[0];
  if (first->type == VAL_REF && first->as.ref_value.target) {
    target = first->as.ref_value.target;
  } else if (first->type == VAL_OBJECT) {
    target = (Value*)first;
  }
  if (!target || target->type != VAL_OBJECT) return true;
  Object* obj = &target->as.object_value;
  size_t n = obj->field_count < d->field_count ? obj->field_count : d->field_count;
  for (size_t i = 0; i < n; ++i) {
    const DropFieldSpec* spec = &d->fields[i];
    switch (spec->kind) {
      case DROP_F_SKIP:
        break;
      case DROP_F_FREE:
        value_free(&obj->fields[i]);
        break;
      case DROP_F_NESTED: {
        if (spec->nested && obj->fields[i].type == VAL_OBJECT) {
          Value ref = {
            .type = VAL_REF,
            .as.ref_value = { .target = &obj->fields[i], .kind = REF_MOD }
          };
          (void)drop_native_cb(vm, NULL, &ref,
                               1, (void*)spec->nested);
        }
        break;
      }
    }
  }
  return true;
}

size_t vm_drop_descriptor_invocations(const void* user_data) {
  if (!user_data) return 0;
  const RaeVmDropDescriptor* d = (const RaeVmDropDescriptor*)user_data;
  return d->invocations;
}

/* A descriptor slot covers one (struct decl, optional spec
 * args) instantiation pair. `spec` is NULL for non-generic
 * decls; non-NULL when this entry represents `Wrapper(String)`
 * etc. `mangled` is the helper short-name (no `rae_vm_drop_struct_`
 * prefix, no `_alias` suffix). */
typedef struct {
  const AstDecl* decl;
  const AstTypeRef* spec;       /* NULL for non-generic */
  const char* mangled;          /* short-name without prefix/suffix */
  RaeVmDropDescriptor* full;
  RaeVmDropDescriptor* aliasv;
} DescriptorSlot;

static char* dup_cstr(const char* s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char* p = (char*)malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

/* The mangler always prefixes user types with `rae_` because the C
 * backend needs to avoid C symbol clashes. The VM registry has no
 * such risk, so strip one leading `rae_` to keep the helper name
 * ergonomic — matches what 498/499/500/501/502 already expect for
 * non-generic types, and keeps the spec form readable. */
static const char* strip_rae_prefix(const char* mangled) {
  if (!mangled) return "";
  if (strncmp(mangled, "rae_", 4) == 0) return mangled + 4;
  return mangled;
}

static RaeVmDropDescriptor* alloc_descriptor(const char* short_name,
                                             const char* type_name,
                                             size_t field_count,
                                             bool is_alias) {
  RaeVmDropDescriptor* d = (RaeVmDropDescriptor*)calloc(1, sizeof(*d));
  if (!d) return NULL;
  char buf[1024];
  snprintf(buf, sizeof(buf), "rae_vm_drop_struct_%s%s",
           short_name, is_alias ? "_alias" : "");
  d->name = dup_cstr(buf);
  d->type_name = dup_cstr(type_name);
  d->is_alias = is_alias;
  d->field_count = field_count;
  d->fields = field_count
    ? (DropFieldSpec*)calloc(field_count, sizeof(DropFieldSpec))
    : NULL;
  if ((field_count && !d->fields) || !d->name || !d->type_name) {
    free(d->name);
    free(d->type_name);
    free(d->fields);
    free(d);
    return NULL;
  }
  return d;
}

/* Lookup helpers — by decl (non-generic) or by mangled short-name
 * (covers specializations and non-generic, since the C backend's
 * mangler is the unique key in both cases). */
static DescriptorSlot* find_slot_by_short_name(DescriptorSlot* slots,
                                               size_t count,
                                               const char* short_name) {
  for (size_t i = 0; i < count; ++i) {
    if (slots[i].mangled && strcmp(slots[i].mangled, short_name) == 0) {
      return &slots[i];
    }
  }
  return NULL;
}

/* Resolve one field's drop kind under the requested variant. When
 * `generic_params` + `concrete_args` are non-NULL the field type is
 * substituted before classification — that is how we handle
 * specs like `Wrapper(String) { value: T }` (T → String). */
static void resolve_field(CompilerContext* ctx,
                          const AstModule* module,
                          const AstTypeField* f,
                          bool is_alias,
                          const AstIdentifierPart* generic_params,
                          const AstTypeRef* concrete_args,
                          DescriptorSlot* slots,
                          size_t slot_count,
                          DropFieldSpec* out) {
  out->kind = DROP_F_SKIP;
  out->nested = NULL;
  AstTypeRef* concrete = f->type;
  if (generic_params && concrete_args) {
    concrete = substitute_type_ref(ctx, generic_params, concrete_args, f->type);
  }
  if (!concrete) return;
  if (!type_needs_cascade_drop(ctx, module, concrete, 0)) return;
  Str fbase = get_base_type_name(concrete);
  if (str_eq_cstr(fbase, "String")) {
    if (is_alias) return;
    out->kind = DROP_F_FREE;
    return;
  }
  if (is_drop_target_type(concrete)) {
    if (is_alias) {
      const AstTypeRef* elem = concrete->generic_args;
      if (elem && type_needs_cascade_drop(ctx, module, (AstTypeRef*)elem, 0)) {
        return;
      }
    }
    out->kind = DROP_F_FREE;
    return;
  }
  /* Nested user struct: could be non-generic or another spec.
   * Look up by mangled name so both shapes resolve. */
  const char* nested_mangled =
      rae_mangle_type_specialized(ctx, NULL, NULL, concrete);
  if (!nested_mangled) return;
  const char* nested_short = strip_rae_prefix(nested_mangled);
  DescriptorSlot* nested_slot =
      find_slot_by_short_name(slots, slot_count, nested_short);
  if (!nested_slot) return;
  out->kind = DROP_F_NESTED;
  out->nested = is_alias ? nested_slot->aliasv : nested_slot->full;
}

bool vm_drop_register_for_module(CompilerContext* ctx,
                                 const AstModule* module,
                                 VmRegistry* registry) {
  if (!ctx || !module || !registry) return false;

  collect_decls_from_module(ctx, module);

  enum { MAX_DROP_TYPES = 1024 };
  DescriptorSlot slots[MAX_DROP_TYPES];
  size_t slot_count = 0;

  /* Pass 1a — non-generic user structs. Same selection as before:
   * cascade-drop true, not c_struct, not generic. */
  for (size_t i = 0;
       i < ctx->all_decl_count && slot_count < MAX_DROP_TYPES;
       i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_TYPE) continue;
    if (d->as.type_decl.generic_params) continue;
    if (has_property(d->as.type_decl.properties, "c_struct")) continue;
    AstIdentifierPart part = {0};
    part.text = d->as.type_decl.name;
    AstTypeRef tr = {0};
    tr.parts = &part;
    if (!type_needs_cascade_drop(ctx, module, &tr, 0)) continue;
    size_t field_count = 0;
    for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
      field_count++;
    }
    const char* mangled = rae_mangle_type_specialized(ctx, NULL, NULL, &tr);
    const char* short_name = strip_rae_prefix(mangled);
    /* Dedup defensively — Pass 1b might collide if a non-generic
     * type somehow shows up in generic_types[]. */
    if (find_slot_by_short_name(slots, slot_count, short_name)) continue;
    char tn_buf[256];
    snprintf(tn_buf, sizeof(tn_buf), "%.*s",
             (int)d->as.type_decl.name.len, d->as.type_decl.name.data);
    RaeVmDropDescriptor* full =
        alloc_descriptor(short_name, tn_buf, field_count, false);
    RaeVmDropDescriptor* aliasv =
        alloc_descriptor(short_name, tn_buf, field_count, true);
    if (!full || !aliasv) {
      free(full); free(aliasv);
      continue;
    }
    slots[slot_count].decl = d;
    slots[slot_count].spec = NULL;
    slots[slot_count].mangled = short_name;
    slots[slot_count].full = full;
    slots[slot_count].aliasv = aliasv;
    slot_count++;
  }

  /* Pass 1b — concrete generic struct specializations. The C
   * backend populates `ctx->generic_types[]` via
   * `discover_specializations_module` (called from
   * `vm_compile_module` before us). Each entry is an `AstTypeRef`
   * with `generic_args`. We accept it iff:
   *   * it resolves to a user struct decl (not List/Map/Buffer —
   *     those are leaves handled by OP_DROP_LOCAL)
   *   * the underlying decl is generic (otherwise Pass 1a covered it)
   *   * the substituted-fields walk cascade-drops
   * Trivial specializations (e.g. `Wrapper(Int)` whose substituted
   * fields are all primitive) get no helper — type_needs_cascade_drop
   * with substitution returns false. */
  for (size_t i = 0;
       i < ctx->generic_type_count && slot_count < MAX_DROP_TYPES;
       i++) {
    const AstTypeRef* gt = ctx->generic_types[i];
    if (!gt || gt->is_view || gt->is_mod) continue;
    Str gb = get_base_type_name(gt);
    /* Skip the leaf containers — handled by OP_DROP_LOCAL. */
    if (str_eq_cstr(gb, "List")
        || str_eq_cstr(gb, "StringMap")
        || str_eq_cstr(gb, "IntMap")
        || str_eq_cstr(gb, "Buffer")
        || str_eq_cstr(gb, "Box")
        || str_eq_cstr(gb, "Opt")) continue;
    if (!gt->generic_args) continue;
    /* Find the template decl. Prefer non-spec generic decl. */
    const AstDecl* tdecl = NULL;
    for (size_t k = 0; k < ctx->all_decl_count; k++) {
      const AstDecl* d = ctx->all_decls[k];
      if (d->kind != AST_DECL_TYPE) continue;
      if (d->as.type_decl.specialization_args) continue;
      if (!str_eq(d->as.type_decl.name, gb)) continue;
      tdecl = d;
      break;
    }
    if (!tdecl
        || !tdecl->as.type_decl.generic_params
        || has_property(tdecl->as.type_decl.properties, "c_struct")) continue;
    /* Walk the substituted fields manually — the backend-neutral
     * predicate doesn't substitute generic params, so calling it on
     * a template's fields would always say "no cascade". Iterate
     * fields, substitute T → concrete, ask the predicate on the
     * substituted type, and accept the spec if ANY field cascades. */
    {
      bool any_cascades = false;
      const AstIdentifierPart* gp = tdecl->as.type_decl.generic_params;
      const AstTypeRef* ga = gt->generic_args;
      for (const AstTypeField* f = tdecl->as.type_decl.fields; f; f = f->next) {
        AstTypeRef* sub = substitute_type_ref(ctx, gp, ga, f->type);
        if (sub && type_needs_cascade_drop(ctx, module, sub, 0)) {
          any_cascades = true;
          break;
        }
      }
      if (!any_cascades) continue;
    }
    const char* mangled =
        rae_mangle_type_specialized(ctx, NULL, NULL, (AstTypeRef*)gt);
    if (!mangled) continue;
    const char* short_name = strip_rae_prefix(mangled);
    /* Dedup — `register_generic_type` already dedups by
     * `type_refs_equal`, but two different AstTypeRef objects may
     * mangle to the same name. */
    if (find_slot_by_short_name(slots, slot_count, short_name)) continue;
    size_t field_count = 0;
    for (const AstTypeField* f = tdecl->as.type_decl.fields; f; f = f->next) {
      field_count++;
    }
    /* type_name for runtime-side debugging — the bare spec name
     * (matches Object.type_name set by struct-construct emission). */
    char tn_buf[256];
    snprintf(tn_buf, sizeof(tn_buf), "%.*s",
             (int)tdecl->as.type_decl.name.len, tdecl->as.type_decl.name.data);
    RaeVmDropDescriptor* full =
        alloc_descriptor(short_name, tn_buf, field_count, false);
    RaeVmDropDescriptor* aliasv =
        alloc_descriptor(short_name, tn_buf, field_count, true);
    if (!full || !aliasv) {
      free(full); free(aliasv);
      continue;
    }
    slots[slot_count].decl = tdecl;
    slots[slot_count].spec = gt;
    slots[slot_count].mangled = short_name;
    slots[slot_count].full = full;
    slots[slot_count].aliasv = aliasv;
    slot_count++;
  }

  /* Pass 2 — resolve per-field kinds. Substitution is applied
   * when this slot represents a specialization. */
  for (size_t i = 0; i < slot_count; i++) {
    DescriptorSlot* s = &slots[i];
    const AstIdentifierPart* gp = NULL;
    const AstTypeRef* args = NULL;
    if (s->spec) {
      gp = s->decl->as.type_decl.generic_params;
      args = s->spec->generic_args;
    }
    size_t j = 0;
    for (const AstTypeField* f = s->decl->as.type_decl.fields;
         f;
         f = f->next, ++j) {
      resolve_field(ctx, module, f, /*is_alias=*/false,
                    gp, args, slots, slot_count, &s->full->fields[j]);
      resolve_field(ctx, module, f, /*is_alias=*/true,
                    gp, args, slots, slot_count, &s->aliasv->fields[j]);
    }
  }

  /* Pass 3 — register natives. */
  for (size_t i = 0; i < slot_count; i++) {
    DescriptorSlot* s = &slots[i];
    if (!vm_registry_register_native(registry, s->full->name,
                                     drop_native_cb, s->full)) {
      return false;
    }
    if (!vm_registry_register_native(registry, s->aliasv->name,
                                     drop_native_cb, s->aliasv)) {
      return false;
    }
  }

  return true;
}
