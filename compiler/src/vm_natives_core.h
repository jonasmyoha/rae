#ifndef VM_NATIVES_CORE_H
#define VM_NATIVES_CORE_H

#include <stdbool.h>
#include <stdint.h>

// Per-VM-instance state used by the `nextTick` native; lifetime managed by the
// caller (typically a stack-allocated value in `run_command`).
typedef struct {
  int64_t next;
} TickCounter;

struct VmRegistry;

// Register the core native bindings (time, sleep, math, string, IO, sys, json,
// buffer ops, raylib, tinyexpr) on the given registry. Pass NULL for
// tick_counter to skip the `nextTick` binding.
bool register_default_natives(struct VmRegistry* registry, TickCounter* tick_counter);

#endif /* VM_NATIVES_CORE_H */
