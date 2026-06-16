/* Live VM cascade-drop synthesis — see vm_drop.h.
 *
 * One shared C callback (`drop_native_cb`) handles every registered
 * helper; per-(type × variant) behaviour comes from a
 * `RaeVmDropDescriptor*` passed through `user_data`. Mirrors the C
 * backend's `rae_drop_struct_<T>` / `..._alias` selection logic
 * exactly: same predicate, same skip rules, same target naming
 * convention (with a `vm_` infix).
 *
 * This is infrastructure only. Nothing in the VM emitter calls
 * these natives yet; tests invoke them through the registry directly.
 */
#include "vm_drop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "c_backend.h"           /* register_decl / collect_decls_from_module */
#include "c_backend_internal.h"  /* has_property (AST utility) */
#include "ownership.h"
#include "sema.h"
#include "str.h"
#include "vm_registry.h"
#include "vm_value.h"

/* What to do with a single field during the cascade walk. */
typedef enum {
  /* Primitive, view/mod, c_struct, or — for ALIAS — a String / a
   * cascade-droppable container. Leave the field's heap (if any)
   * untouched. */
  DROP_F_SKIP,
  /* value_free() the field. Handles VAL_STRING, VAL_KEY, VAL_OBJECT,
   * VAL_ARRAY, and VAL_BUFFER uniformly. */
  DROP_F_FREE,
  /* Nested user struct — recurse via `nested`. */
  DROP_F_NESTED,
} DropFieldKind;

struct RaeVmDropDescriptor;

typedef struct {
  DropFieldKind kind;
  const struct RaeVmDropDescriptor* nested; /* for DROP_F_NESTED */
} DropFieldSpec;

typedef struct RaeVmDropDescriptor {
  char* name;                /* "rae_vm_drop_struct_<...>" / "..._alias" */
  char* type_name;           /* matches Object.type_name set by emitters */
  bool is_alias;
  size_t field_count;
  DropFieldSpec* fields;
  /* Observable counters — let tests confirm the helper ran. The
   * test counter is incremented every invocation regardless of
   * whether the value was a VAL_OBJECT. */
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
  /* Caller may pass either a `mod` reference (the natural call
   * shape from bytecode) or the object directly (the natural shape
   * for a direct-invocation unit test). Both work. */
  Value* target = NULL;
  const Value* first = &args[0];
  if (first->type == VAL_REF && first->as.ref_value.target) {
    target = first->as.ref_value.target;
  } else if (first->type == VAL_OBJECT) {
    /* Discarding const is safe: the contract of drop is destructive
     * mutation of args[0]. The test feeds a stack-owned Value. */
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

/* Lookup a per-type descriptor pair (FULL, ALIAS) by struct decl. */
typedef struct {
  const AstDecl* decl;
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

static RaeVmDropDescriptor* alloc_descriptor(Str type_name,
                                             size_t field_count,
                                             bool is_alias) {
  RaeVmDropDescriptor* d = (RaeVmDropDescriptor*)calloc(1, sizeof(*d));
  if (!d) return NULL;
  /* Use the bare type-decl name (no `rae_` mangler prefix). The
   * C backend names its drop helpers `rae_drop_struct_rae_<T>`
   * because the mangler adds a `rae_` prefix to avoid C symbol
   * clashes; the VM registry has no such risk, so the simpler
   * name `rae_vm_drop_struct_<T>` is preferred — matches what a
   * user typing an `extern func` would naturally write. */
  char buf[512];
  snprintf(buf, sizeof(buf), "rae_vm_drop_struct_%.*s%s",
           (int)type_name.len, type_name.data,
           is_alias ? "_alias" : "");
  d->name = dup_cstr(buf);
  /* Object.type_name is a NUL-terminated cstr — match that. */
  char* tn = (char*)malloc(type_name.len + 1);
  if (tn) {
    memcpy(tn, type_name.data, type_name.len);
    tn[type_name.len] = '\0';
  }
  d->type_name = tn;
  d->is_alias = is_alias;
  d->field_count = field_count;
  d->fields = field_count
    ? (DropFieldSpec*)calloc(field_count, sizeof(DropFieldSpec))
    : NULL;
  if (field_count && !d->fields) {
    free(d->name);
    free(d->type_name);
    free(d);
    return NULL;
  }
  return d;
}

static DescriptorSlot* find_slot(DescriptorSlot* slots, size_t count,
                                 const AstDecl* decl) {
  for (size_t i = 0; i < count; ++i) {
    if (slots[i].decl == decl) return &slots[i];
  }
  return NULL;
}

/* Resolve one field's drop kind under the requested variant. Mirrors
 * the C backend's rae_drop_struct_<T>(_alias) field-by-field switch
 * in c_backend.c. */
static void resolve_field(CompilerContext* ctx,
                          const AstModule* module,
                          const AstTypeField* f,
                          bool is_alias,
                          DescriptorSlot* slots,
                          size_t slot_count,
                          DropFieldSpec* out) {
  out->kind = DROP_F_SKIP;
  out->nested = NULL;
  AstTypeRef* concrete = f->type;
  if (!type_needs_cascade_drop(ctx, module, concrete, 0)) return;
  Str fbase = get_base_type_name(concrete);
  if (str_eq_cstr(fbase, "String")) {
    if (is_alias) return; /* alias skips String — would double-free */
    out->kind = DROP_F_FREE;
    return;
  }
  /* List / StringMap / IntMap: drop the container. ALIAS skips when
   * the element type itself cascades (mirrors C ALIAS logic). */
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
  /* Nested non-generic user struct: recurse via the matching variant. */
  const AstDecl* nested_decl = find_type_decl(NULL, module, fbase);
  if (!nested_decl || nested_decl->kind != AST_DECL_TYPE) return;
  if (nested_decl->as.type_decl.generic_params) return;
  if (has_property(nested_decl->as.type_decl.properties, "c_struct")) return;
  DescriptorSlot* nested_slot = find_slot(slots, slot_count, nested_decl);
  if (!nested_slot) return;
  out->kind = DROP_F_NESTED;
  out->nested = is_alias ? nested_slot->aliasv : nested_slot->full;
}

bool vm_drop_register_for_module(CompilerContext* ctx,
                                 const AstModule* module,
                                 VmRegistry* registry) {
  if (!ctx || !module || !registry) return false;

  /* Make sure ctx->all_decls is populated for nested-type lookups
   * (collect_decls_from_module is idempotent — skips already-walked
   * modules). */
  collect_decls_from_module(ctx, module);

  /* Pass 1: enumerate candidate structs and allocate empty descriptors
   * so nested-field resolution in Pass 2 can point at them. */
  enum { MAX_DROP_TYPES = 512 };
  DescriptorSlot slots[MAX_DROP_TYPES];
  size_t slot_count = 0;
  for (size_t i = 0;
       i < ctx->all_decl_count && slot_count < MAX_DROP_TYPES;
       i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_TYPE) continue;
    if (d->as.type_decl.generic_params) continue;
    if (has_property(d->as.type_decl.properties, "c_struct")) continue;
    /* Build a type ref so the predicate sees the bare base name. */
    AstIdentifierPart part = {0};
    part.text = d->as.type_decl.name;
    AstTypeRef tr = {0};
    tr.parts = &part;
    if (!type_needs_cascade_drop(ctx, module, &tr, 0)) continue;
    size_t field_count = 0;
    for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
      field_count++;
    }
    RaeVmDropDescriptor* full =
        alloc_descriptor(d->as.type_decl.name, field_count, false);
    RaeVmDropDescriptor* aliasv =
        alloc_descriptor(d->as.type_decl.name, field_count, true);
    if (!full || !aliasv) {
      free(full); free(aliasv);
      continue;
    }
    slots[slot_count].decl = d;
    slots[slot_count].full = full;
    slots[slot_count].aliasv = aliasv;
    slot_count++;
  }

  /* Pass 2: resolve per-field kinds. */
  for (size_t i = 0; i < slot_count; i++) {
    DescriptorSlot* s = &slots[i];
    size_t j = 0;
    for (const AstTypeField* f = s->decl->as.type_decl.fields;
         f;
         f = f->next, ++j) {
      resolve_field(ctx, module, f, /*is_alias=*/false,
                    slots, slot_count, &s->full->fields[j]);
      resolve_field(ctx, module, f, /*is_alias=*/true,
                    slots, slot_count, &s->aliasv->fields[j]);
    }
  }

  /* Pass 3: register both natives per type with the VM registry. */
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
