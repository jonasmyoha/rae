#include "vm_registry.h"

#include <stdio.h>
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
    registry->natives = NULL;
    registry->native_count = 0;
    registry->native_capacity = 0;

    registry->globals = NULL;
    registry->global_init_bits = NULL;
    registry->global_count = 0;
    registry->global_capacity = 0;

    registry->global_mappings = NULL;
    registry->mapping_count = 0;
    registry->mapping_capacity = 0;
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
    for (size_t i = 0; i < registry->native_count; ++i) {
        free(registry->natives[i].name);
        registry->natives[i].name = NULL;
        registry->natives[i].callback = NULL;
        registry->natives[i].user_data = NULL;
    }
    free(registry->natives);
    registry->natives = NULL;
    registry->native_count = 0;
    registry->native_capacity = 0;

    // Free globals
    for (size_t i = 0; i < registry->global_count; ++i) {
        value_free(&registry->globals[i]);
    }
    free(registry->globals);
    free(registry->global_init_bits);

    // Free mappings
    for (size_t i = 0; i < registry->mapping_count; ++i) {
        str_free(registry->global_mappings[i].name);
        str_free(registry->global_mappings[i].type_name);
    }
    free(registry->global_mappings);
}

uint32_t vm_registry_ensure_global(VmRegistry* registry, Str name, Str type_name) {

  uint32_t existing = vm_registry_find_global(registry, name);

  if (existing != VM_GLOBAL_NOT_FOUND) {

    return existing;

  }

  


  

  // Create new mapping

  if (registry->mapping_count + 1 > registry->mapping_capacity) {


        size_t old_cap = registry->mapping_capacity;
        size_t new_cap = old_cap < 8 ? 8 : old_cap * 2;
        VmGlobalMapping* resized = realloc(registry->global_mappings, new_cap * sizeof(VmGlobalMapping));
        if (!resized) return VM_GLOBAL_NOT_FOUND;
        registry->global_mappings = resized;
        registry->mapping_capacity = new_cap;
    }

    uint32_t index = (uint32_t)registry->global_count;

    // Ensure storage capacity
    if (registry->global_count + 1 > registry->global_capacity) {
        size_t old_cap = registry->global_capacity;
        size_t new_cap = old_cap < 8 ? 8 : old_cap * 2;
        Value* resized_globals = realloc(registry->globals, new_cap * sizeof(Value));
        if (!resized_globals) return VM_GLOBAL_NOT_FOUND;
        registry->globals = resized_globals;

        uint8_t* resized_bits = realloc(registry->global_init_bits, new_cap * sizeof(uint8_t));
        if (!resized_bits) return VM_GLOBAL_NOT_FOUND;
        registry->global_init_bits = resized_bits;

        registry->global_capacity = new_cap;
    }

      if (str_eq_cstr(type_name, "Int")) {

          registry->globals[index] = value_int(0);

      } else if (str_eq_cstr(type_name, "Float")) {

          registry->globals[index] = value_float(0.0);

      } else if (str_eq_cstr(type_name, "Bool")) {

          registry->globals[index] = value_bool(false);

      } else if (str_eq_cstr(type_name, "String")) {

          registry->globals[index] = value_string_copy("", 0);

      } else {

          registry->globals[index] = value_none();

      }

      

      registry->global_init_bits[index] = 0;

    
    registry->global_count++;

    VmGlobalMapping* mapping = &registry->global_mappings[registry->mapping_count++];
    mapping->name = str_dup(name);
    mapping->type_name = str_dup(type_name);
    mapping->index = index;

    return index;
}

uint32_t vm_registry_find_global(const VmRegistry* registry, Str name) {
  if (!registry) return VM_GLOBAL_NOT_FOUND;
  for (size_t i = 0; i < registry->mapping_count; ++i) {
    if (str_eq(registry->global_mappings[i].name, name)) {
      return registry->global_mappings[i].index;
    }
  }
  return VM_GLOBAL_NOT_FOUND;
}
Str vm_registry_get_global_type(const VmRegistry* registry, Str name) {
    if (!registry) return (Str){0};
    for (size_t i = 0; i < registry->mapping_count; ++i) {
        if (str_eq(registry->global_mappings[i].name, name)) {
            return registry->global_mappings[i].type_name;
        }
    }
    return (Str){0};
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

static VmNativeEntry* vm_registry_native_slot(VmRegistry* registry) {
  if (registry->native_count + 1 > registry->native_capacity) {
    size_t old_capacity = registry->native_capacity;
    size_t new_capacity = old_capacity < 4 ? 4 : old_capacity * 2;
    VmNativeEntry* resized = realloc(registry->natives, new_capacity * sizeof(VmNativeEntry));
    if (!resized) {
      return NULL;
    }
    registry->natives = resized;
    registry->native_capacity = new_capacity;
  }
  VmNativeEntry* entry = &registry->natives[registry->native_count++];
  memset(entry, 0, sizeof(VmNativeEntry));
  return entry;
}

static VmNativeEntry* vm_registry_find_native_mut(VmRegistry* registry, const char* name) {
  if (!registry || !name) return NULL;
  for (size_t i = 0; i < registry->native_count; ++i) {
    if (registry->natives[i].name && strcmp(registry->natives[i].name, name) == 0) {
      return &registry->natives[i];
    }
  }
  return NULL;
}

bool vm_registry_register_native(VmRegistry* registry,
                                 const char* name,
                                 VmNativeCallback callback,
                                 void* user_data) {
  if (!registry || !name || !callback) {
    return false;
  }
  VmNativeEntry* existing = vm_registry_find_native_mut(registry, name);
  if (existing) {
    existing->callback = callback;
    existing->user_data = user_data;
    return true;
  }
  VmNativeEntry* entry = vm_registry_native_slot(registry);
  if (!entry) {
    return false;
  }
  entry->name = dup_string(name);
  if (!entry->name) {
    return false;
  }
  entry->callback = callback;
  entry->user_data = user_data;
  return true;
}

const VmNativeEntry* vm_registry_find_native(const VmRegistry* registry, const char* name) {
  if (!registry || !name) return NULL;
  for (size_t i = 0; i < registry->native_count; ++i) {
    const VmNativeEntry* entry = &registry->natives[i];
    if (entry->name && strcmp(entry->name, name) == 0) {
      return entry;
    }
  }
  return NULL;
}
