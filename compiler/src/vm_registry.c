#include "vm_registry.h"

#include <stdlib.h>
#include <string.h>

static char* dup_string(const char* src) {
  if (!src) return NULL;
  size_t len = strlen(src);
  char* copy = malloc(len + 1);
  if (copy) {
    memcpy(copy, src, len + 1);
  }
  return copy;
}

void vm_registry_init(VmRegistry* registry) {
  if (!registry) return;
  registry->modules = NULL;
  registry->count = 0;
  registry->capacity = 0;
}

void vm_registry_free(VmRegistry* registry) {
  if (!registry) return;
  for (size_t i = 0; i < registry->count; ++i) {
    VmModule* module = &registry->modules[i];
    chunk_free(&module->chunk);
    free(module->path);
    module->path = NULL;
  }
  free(registry->modules);
  registry->modules = NULL;
  registry->count = 0;
  registry->capacity = 0;
}

VmModule* vm_registry_find(VmRegistry* registry, const char* path) {
  if (!registry || !path) return NULL;
  for (size_t i = 0; i < registry->count; ++i) {
    VmModule* module = &registry->modules[i];
    if (module->path && strcmp(module->path, path) == 0) {
      return module;
    }
  }
  return NULL;
}

static VmModule* vm_registry_add(VmRegistry* registry) {
  if (registry->count + 1 > registry->capacity) {
    size_t old_capacity = registry->capacity;
    size_t new_capacity = old_capacity < 4 ? 4 : old_capacity * 2;
    VmModule* resized = realloc(registry->modules, new_capacity * sizeof(VmModule));
    if (!resized) {
      return NULL;
    }
    registry->modules = resized;
    registry->capacity = new_capacity;
  }
  VmModule* module = &registry->modules[registry->count++];
  memset(module, 0, sizeof(VmModule));
  chunk_init(&module->chunk);
  return module;
}

bool vm_registry_load(VmRegistry* registry, const char* path, Chunk chunk) {
  if (!registry || !path) return false;
  VmModule* existing = vm_registry_find(registry, path);
  if (existing) {
    chunk_free(&existing->chunk);
    existing->chunk = chunk;
    return true;
  }
  VmModule* module = vm_registry_add(registry);
  if (!module) {
    chunk_free(&chunk);
    return false;
  }
  module->path = dup_string(path);
  if (!module->path) {
    chunk_free(&chunk);
    return false;
  }
  module->chunk = chunk;
  return true;
}

bool vm_registry_reload(VmRegistry* registry, const char* path, Chunk chunk) {
  VmModule* module = vm_registry_find(registry, path);
  if (!module) {
    return vm_registry_load(registry, path, chunk);
  }
  chunk_free(&module->chunk);
  module->chunk = chunk;
  return true;
}
