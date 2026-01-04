#ifndef VM_REGISTRY_H
#define VM_REGISTRY_H

#include <stddef.h>

#include "vm_chunk.h"

typedef struct {
  char* path;
  Chunk chunk;
} VmModule;

struct VM;

typedef bool (*VmNativeCallback)(struct VM* vm,
                                 Value* out_value,
                                 const Value* args,
                                 size_t arg_count,
                                 void* user_data);

typedef struct {
  char* name;
  VmNativeCallback callback;
  void* user_data;
} VmNativeEntry;

typedef struct {
  VmModule* modules;
  size_t count;
  size_t capacity;
  VmNativeEntry* natives;
  size_t native_count;
  size_t native_capacity;
} VmRegistry;

void vm_registry_init(VmRegistry* registry);
void vm_registry_free(VmRegistry* registry);
VmModule* vm_registry_find(VmRegistry* registry, const char* path);
bool vm_registry_load(VmRegistry* registry, const char* path, Chunk chunk);
bool vm_registry_reload(VmRegistry* registry, const char* path, Chunk chunk);
bool vm_registry_register_native(VmRegistry* registry,
                                 const char* name,
                                 VmNativeCallback callback,
                                 void* user_data);
const VmNativeEntry* vm_registry_find_native(const VmRegistry* registry, const char* name);

#endif /* VM_REGISTRY_H */
