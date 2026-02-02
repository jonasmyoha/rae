#ifndef VM_REGISTRY_H
#define VM_REGISTRY_H

#include <stddef.h>

#include "vm_chunk.h"
#include "str.h"

typedef struct {
  char* path;
  Chunk chunk;
} VmModule;

struct VM;

typedef struct {
  bool has_value;
  Value value;
} VmNativeResult;

typedef bool (*VmNativeCallback)(struct VM* vm,
                                 VmNativeResult* out_result,
                                 const Value* args,
                                 size_t arg_count,
                                 void* user_data);

typedef struct {
  char* name;
  VmNativeCallback callback;
  void* user_data;
} VmNativeEntry;

typedef struct {
  Str name;
  Str type_name;
  uint32_t index;
} VmGlobalMapping;

typedef struct VmRegistry {
  VmModule* modules;
  size_t count;
  size_t capacity;
  VmNativeEntry* natives;
  size_t native_count;
  size_t native_capacity;
  
  // Globals storage
  Value* globals;
  uint8_t* global_init_bits;
  size_t global_count;
  size_t global_capacity;
  
  // Stable GlobalName -> Index mapping
  VmGlobalMapping* global_mappings;
  size_t mapping_count;
  size_t mapping_capacity;
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

// Globals management
uint32_t vm_registry_ensure_global(VmRegistry* registry, Str name, Str type_name);
uint32_t vm_registry_find_global(const VmRegistry* registry, Str name);
Str vm_registry_get_global_type(const VmRegistry* registry, Str name);
#define VM_GLOBAL_NOT_FOUND ((uint32_t)-1)

#endif /* VM_REGISTRY_H */
