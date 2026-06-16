/* Live VM cascade-drop helpers.
 *
 * Stage 1 step 2 of structural drop (docs/structural-drop-stage1-plan.md).
 * Synthesises FULL and ALIAS drop natives for every cascade-droppable
 * non-generic user struct in a module, registered with the VM native
 * registry under the names:
 *
 *   rae_vm_drop_struct_<MangledType>          — FULL
 *   rae_vm_drop_struct_<MangledType>_alias    — ALIAS
 *
 * Naming mirrors the C backend's `rae_drop_struct_<T>` / `..._alias`
 * helpers, with the `vm_` infix so the two sets never collide if both
 * backends ever co-exist in the same address space (compiler test
 * harness, hybrid runs).
 *
 * This commit registers the helpers and lets a test invoke them
 * directly. It does NOT yet emit any opcode that calls them at scope
 * exit, early return, or reassignment — that lands separately.
 *
 * Design (B1 per the spike): one shared C callback, N registrations.
 * Each registration's user_data is a per-(type × variant) descriptor
 * that drives the cascade. Heap allocation: descriptors are leaked
 * for the lifetime of the registry (O(N_types × 2) tiny structs;
 * VmRegistry's lifetime is the whole compiler process, and hot-reload
 * preserves natives by design).
 */
#ifndef RAE_VM_DROP_H
#define RAE_VM_DROP_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"

struct VmRegistry;

/* Walk every cascade-droppable non-generic user struct reachable from
 * `module` and register FULL + ALIAS drop natives for each.
 *
 * Returns true on success, false on allocation / registration failure.
 * It is safe to call multiple times: re-registering the same name on
 * the registry overwrites the previous entry (mirrors how natives
 * survive hot-reload). */
bool vm_drop_register_for_module(CompilerContext* ctx,
                                 const AstModule* module,
                                 struct VmRegistry* registry);

/* Read the invocation counter from a descriptor (registry user_data
 * pointer). Caller MUST be confident the user_data was attached by
 * `vm_drop_register_for_module` — pass the user_data of an entry
 * whose name starts with `rae_vm_drop_struct_`. Returns 0 for a
 * NULL pointer. */
size_t vm_drop_descriptor_invocations(const void* user_data);

#endif /* RAE_VM_DROP_H */
