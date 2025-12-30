#ifndef VM_REGISTRY_H
#define VM_REGISTRY_H

#include <stddef.h>

#include "vm_chunk.h"

typedef struct {
  char* path;
  Chunk chunk;
} VmModule;

typedef struct {
  VmModule* modules;
  size_t count;
  size_t capacity;
} VmRegistry;

void vm_registry_init(VmRegistry* registry);
void vm_registry_free(VmRegistry* registry);
VmModule* vm_registry_find(VmRegistry* registry, const char* path);
bool vm_registry_load(VmRegistry* registry, const char* path, Chunk chunk);
bool vm_registry_reload(VmRegistry* registry, const char* path, Chunk chunk);

#endif /* VM_REGISTRY_H */
