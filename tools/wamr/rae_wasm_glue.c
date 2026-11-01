/* lib/wasm's glue over WAMR (compiler/runtime/runtime_wasm_embed.h). WAMR calls are one each; the
 * worker thread and the host-call mailbox are what a callback-free Rae needs
 * to answer imports (see runtime_wasm_embed.h). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "wasm_export.h"
#include "runtime_wasm_embed.h"

#define RAE_WASM_MAX_VALUES 32
#define RAE_WASM_ERROR_BYTES 256

static pthread_mutex_t rae_wasm_global = PTHREAD_MUTEX_INITIALIZER;
static int rae_wasm_ready = 0;
static __thread char rae_wasm_error[RAE_WASM_ERROR_BYTES];

static void rae_wasm_set_error(const char* text) {
  snprintf(rae_wasm_error, sizeof rae_wasm_error, "%s", text ? text : "");
}

int64_t rae_wasm_init(void) {
  pthread_mutex_lock(&rae_wasm_global);
  if (!rae_wasm_ready) {
    RuntimeInitArgs args;
    memset(&args, 0, sizeof args);
    args.mem_alloc_type = Alloc_With_System_Allocator;
    rae_wasm_ready = wasm_runtime_full_init(&args) ? 1 : 0;
  }
  pthread_mutex_unlock(&rae_wasm_global);
  return rae_wasm_ready;
}

int64_t rae_wasm_last_error_length(void) { return (int64_t)strlen(rae_wasm_error); }
void rae_wasm_last_error_copy(uint8_t* into) { memcpy(into, rae_wasm_error, strlen(rae_wasm_error)); }

/* ----- modules ----- */

typedef struct RaeWasmModule {
  wasm_module_t module;
  /* the bytes as given, and the copy the current load uses (WAMR's loader
   * may write into its buffer, which must live until the module is unloaded) */
  uint8_t* bytes;
  uint8_t* loaded;
  uint32_t size;
  /* the trampoline's registry id of each function import granted; -1 not */
  int64_t* granted;
  int32_t import_count;
} RaeWasmModule;

static wasm_module_t rae_wasm_load_bytes(RaeWasmModule* m) {
  char error[RAE_WASM_ERROR_BYTES] = "";
  free(m->loaded);
  m->loaded = (uint8_t*)malloc(m->size > 0 ? m->size : 1);
  if (!m->loaded) { rae_wasm_set_error("out of memory"); return NULL; }
  memcpy(m->loaded, m->bytes, m->size);
  wasm_module_t module = wasm_runtime_load(m->loaded, m->size, error, sizeof error);
  if (!module) rae_wasm_set_error(error);
  return module;
}

int64_t rae_wasm_load(uint8_t* bytes, int64_t offset, int64_t count) {
  if (!rae_wasm_init()) { rae_wasm_set_error("the WASM runtime could not start"); return 0; }
  RaeWasmModule* m = (RaeWasmModule*)calloc(1, sizeof *m);
  if (!m) return 0;
  m->size = (uint32_t)count;
  m->bytes = (uint8_t*)malloc(count > 0 ? (size_t)count : 1);
  if (!m->bytes) { free(m); return 0; }
  memcpy(m->bytes, bytes + offset, (size_t)count);
  m->module = rae_wasm_load_bytes(m);
  if (!m->module) { free(m->loaded); free(m->bytes); free(m); return 0; }
  m->import_count = wasm_runtime_get_import_count(m->module);
  m->granted = (int64_t*)malloc(sizeof(int64_t) * (size_t)(m->import_count > 0 ? m->import_count : 1));
  for (int32_t i = 0; i < m->import_count; i++) m->granted[i] = -1;
  return (int64_t)(intptr_t)m;
}

void rae_wasm_unload(int64_t handle) {
  RaeWasmModule* m = (RaeWasmModule*)(intptr_t)handle;
  if (!m) return;
  if (m->module) wasm_runtime_unload(m->module);
  free(m->granted);
  free(m->loaded);
  free(m->bytes);
  free(m);
}

static int rae_wasm_import(int64_t handle, int64_t index, wasm_import_t* import) {
  RaeWasmModule* m = (RaeWasmModule*)(intptr_t)handle;
  if (!m || index < 0 || index >= m->import_count) return 0;
  wasm_runtime_get_import_type(m->module, (int32_t)index, import);
  return 1;
}

int64_t rae_wasm_import_count(int64_t handle) {
  RaeWasmModule* m = (RaeWasmModule*)(intptr_t)handle;
  return m ? m->import_count : 0;
}

int64_t rae_wasm_import_kind(int64_t handle, int64_t index) {
  wasm_import_t import;
  if (!rae_wasm_import(handle, index, &import)) return -1;
  switch (import.kind) {
    case WASM_IMPORT_EXPORT_KIND_FUNC: return 0;
    case WASM_IMPORT_EXPORT_KIND_TABLE: return 1;
    case WASM_IMPORT_EXPORT_KIND_MEMORY: return 2;
    case WASM_IMPORT_EXPORT_KIND_GLOBAL: return 3;
    default: return -1;
  }
}

int64_t rae_wasm_import_module_length(int64_t handle, int64_t index) {
  wasm_import_t import;
  return rae_wasm_import(handle, index, &import) ? (int64_t)strlen(import.module_name) : 0;
}
void rae_wasm_import_module_copy(int64_t handle, int64_t index, uint8_t* into) {
  wasm_import_t import;
  if (rae_wasm_import(handle, index, &import)) memcpy(into, import.module_name, strlen(import.module_name));
}
int64_t rae_wasm_import_name_length(int64_t handle, int64_t index) {
  wasm_import_t import;
  return rae_wasm_import(handle, index, &import) ? (int64_t)strlen(import.name) : 0;
}
void rae_wasm_import_name_copy(int64_t handle, int64_t index, uint8_t* into) {
  wasm_import_t import;
  if (rae_wasm_import(handle, index, &import)) memcpy(into, import.name, strlen(import.name));
}

static int64_t rae_wasm_kind_of(wasm_valkind_t kind) {
  switch (kind) {
    case WASM_I32: return 0;
    case WASM_I64: return 1;
    case WASM_F32: return 2;
    case WASM_F64: return 3;
    default: return -1;
  }
}

int64_t rae_wasm_import_param_count(int64_t handle, int64_t index) {
  wasm_import_t import;
  if (!rae_wasm_import(handle, index, &import) || import.kind != WASM_IMPORT_EXPORT_KIND_FUNC) return 0;
  return wasm_func_type_get_param_count(import.u.func_type);
}
int64_t rae_wasm_import_param_kind(int64_t handle, int64_t index, int64_t param) {
  wasm_import_t import;
  if (!rae_wasm_import(handle, index, &import) || import.kind != WASM_IMPORT_EXPORT_KIND_FUNC) return -1;
  return rae_wasm_kind_of(wasm_func_type_get_param_valkind(import.u.func_type, (uint32_t)param));
}
int64_t rae_wasm_import_result_count(int64_t handle, int64_t index) {
  wasm_import_t import;
  if (!rae_wasm_import(handle, index, &import) || import.kind != WASM_IMPORT_EXPORT_KIND_FUNC) return 0;
  return wasm_func_type_get_result_count(import.u.func_type);
}
int64_t rae_wasm_import_result_kind(int64_t handle, int64_t index, int64_t result) {
  wasm_import_t import;
  if (!rae_wasm_import(handle, index, &import) || import.kind != WASM_IMPORT_EXPORT_KIND_FUNC) return -1;
  return rae_wasm_kind_of(wasm_func_type_get_result_valkind(import.u.func_type, (uint32_t)result));
}

/* ----- the trampoline's registry: (module name, field name) -> id ----- */

typedef struct RaeWasmName {
  char* module;
  char* name;
} RaeWasmName;

static RaeWasmName* rae_wasm_names = NULL;
static int64_t rae_wasm_name_count = 0;

static void rae_wasm_trampoline(wasm_exec_env_t exec_env, uint64_t* args);

/* The registry id of (module, name), registering the trampoline under it
 * the first time (WAMR keeps the NativeSymbol, so it is never freed) */
static int64_t rae_wasm_register(const char* module, const char* name) {
  pthread_mutex_lock(&rae_wasm_global);
  for (int64_t i = 0; i < rae_wasm_name_count; i++) {
    if (strcmp(rae_wasm_names[i].module, module) == 0 && strcmp(rae_wasm_names[i].name, name) == 0) {
      pthread_mutex_unlock(&rae_wasm_global);
      return i;
    }
  }
  RaeWasmName* grown = (RaeWasmName*)realloc(rae_wasm_names, sizeof(RaeWasmName) * (size_t)(rae_wasm_name_count + 1));
  if (!grown) { pthread_mutex_unlock(&rae_wasm_global); return -1; }
  rae_wasm_names = grown;
  int64_t id = rae_wasm_name_count++;
  rae_wasm_names[id].module = strdup(module);
  rae_wasm_names[id].name = strdup(name);
  NativeSymbol* symbol = (NativeSymbol*)calloc(1, sizeof(NativeSymbol));
  symbol->symbol = rae_wasm_names[id].name;
  symbol->func_ptr = (void*)rae_wasm_trampoline;
  symbol->signature = NULL;
  symbol->attachment = (void*)(intptr_t)(id + 1);
  wasm_runtime_register_natives_raw(rae_wasm_names[id].module, symbol, 1);
  pthread_mutex_unlock(&rae_wasm_global);
  return id;
}

int64_t rae_wasm_grant(int64_t handle, int64_t index) {
  RaeWasmModule* m = (RaeWasmModule*)(intptr_t)handle;
  wasm_import_t import;
  if (!m || !rae_wasm_import(handle, index, &import) || import.kind != WASM_IMPORT_EXPORT_KIND_FUNC) return -1;
  m->granted[index] = rae_wasm_register(import.module_name, import.name);
  return m->granted[index] >= 0 ? 0 : -1;
}

int64_t rae_wasm_link(int64_t handle) {
  RaeWasmModule* m = (RaeWasmModule*)(intptr_t)handle;
  if (!m) return -1;
  wasm_runtime_unload(m->module);
  m->module = NULL;
  m->module = rae_wasm_load_bytes(m);
  return m->module ? 0 : -1;
}

/* ----- instances ----- */

enum { RAE_WASM_IDLE, RAE_WASM_CALL, RAE_WASM_RUNNING, RAE_WASM_HOST, RAE_WASM_FINISHED, RAE_WASM_TRAPPED, RAE_WASM_QUIT };

typedef struct RaeWasmInstance {
  RaeWasmModule* module;
  wasm_module_inst_t inst;
  wasm_exec_env_t exec_env;
  uint32_t stack_bytes;
  pthread_t worker;
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int state;
  int started;
  /* the call */
  char function[256];
  wasm_val_t args[RAE_WASM_MAX_VALUES];
  uint32_t arg_count;
  wasm_val_t results[RAE_WASM_MAX_VALUES];
  uint32_t result_count;
  char error[RAE_WASM_ERROR_BYTES];
  /* the waiting host call */
  int64_t host_import;
  wasm_val_t host_args[RAE_WASM_MAX_VALUES];
  uint32_t host_arg_count;
  wasm_val_t host_results[RAE_WASM_MAX_VALUES];
  uint32_t host_result_count;
  int host_trap;
  char host_message[RAE_WASM_ERROR_BYTES];
} RaeWasmInstance;

static wasm_valkind_t rae_wasm_valkind(int64_t kind) {
  switch (kind) {
    case 1: return WASM_I64;
    case 2: return WASM_F32;
    case 3: return WASM_F64;
    default: return WASM_I32;
  }
}

static void rae_wasm_run_call(RaeWasmInstance* c) {
  wasm_function_inst_t function = wasm_runtime_lookup_function(c->inst, c->function);
  int ok = function && wasm_runtime_call_wasm_a(c->exec_env, function, c->result_count, c->results,
                                                c->arg_count, c->args);
  pthread_mutex_lock(&c->lock);
  if (ok) {
    c->state = RAE_WASM_FINISHED;
  } else {
    const char* exception = wasm_runtime_get_exception(c->inst);
    snprintf(c->error, sizeof c->error, "%s", exception ? exception : "the call failed");
    wasm_runtime_clear_exception(c->inst);
    c->state = RAE_WASM_TRAPPED;
  }
  pthread_cond_broadcast(&c->changed);
  pthread_mutex_unlock(&c->lock);
}

static void* rae_wasm_worker(void* argument) {
  RaeWasmInstance* c = (RaeWasmInstance*)argument;
  wasm_runtime_init_thread_env();
  c->exec_env = wasm_runtime_create_exec_env(c->inst, c->stack_bytes);
  pthread_mutex_lock(&c->lock);
  c->started = c->exec_env ? 1 : -1;
  pthread_cond_broadcast(&c->changed);
  for (;;) {
    while (c->state != RAE_WASM_CALL && c->state != RAE_WASM_QUIT) pthread_cond_wait(&c->changed, &c->lock);
    if (c->state == RAE_WASM_QUIT) break;
    c->state = RAE_WASM_RUNNING;
    pthread_mutex_unlock(&c->lock);
    rae_wasm_run_call(c);
    pthread_mutex_lock(&c->lock);
  }
  pthread_mutex_unlock(&c->lock);
  if (c->exec_env) wasm_runtime_destroy_exec_env(c->exec_env);
  wasm_runtime_destroy_thread_env();
  return NULL;
}

/* A granted import called: post the host call and wait for Rae's answer */
static void rae_wasm_trampoline(wasm_exec_env_t exec_env, uint64_t* args) {
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
  RaeWasmInstance* c = (RaeWasmInstance*)wasm_runtime_get_custom_data(inst);
  int64_t id = (int64_t)(intptr_t)wasm_runtime_get_function_attachment(exec_env) - 1;
  int64_t import = -1;
  for (int32_t i = 0; c && i < c->module->import_count; i++) {
    if (c->module->granted[i] == id) { import = i; break; }
  }
  if (!c || import < 0) {
    wasm_runtime_set_exception(inst, "an import that was not granted was called");
    return;
  }
  uint32_t params = (uint32_t)rae_wasm_import_param_count((int64_t)(intptr_t)c->module, import);
  uint32_t results = (uint32_t)rae_wasm_import_result_count((int64_t)(intptr_t)c->module, import);
  pthread_mutex_lock(&c->lock);
  c->host_import = import;
  c->host_arg_count = params;
  for (uint32_t i = 0; i < params && i < RAE_WASM_MAX_VALUES; i++) {
    wasm_valkind_t kind = rae_wasm_valkind(rae_wasm_import_param_kind((int64_t)(intptr_t)c->module, import, i));
    c->host_args[i].kind = kind;
    memcpy(&c->host_args[i].of, &args[i], sizeof(uint64_t));
  }
  c->host_result_count = results;
  for (uint32_t i = 0; i < results && i < RAE_WASM_MAX_VALUES; i++) {
    c->host_results[i].kind = rae_wasm_valkind(rae_wasm_import_result_kind((int64_t)(intptr_t)c->module, import, i));
    c->host_results[i].of.i64 = 0;
  }
  c->host_trap = 0;
  c->state = RAE_WASM_HOST;
  pthread_cond_broadcast(&c->changed);
  while (c->state == RAE_WASM_HOST) pthread_cond_wait(&c->changed, &c->lock);
  int trap = c->host_trap;
  if (!trap && results > 0) memcpy(&args[0], &c->host_results[0].of, sizeof(uint64_t));
  pthread_mutex_unlock(&c->lock);
  if (trap) wasm_runtime_set_exception(inst, c->host_message);
}

int64_t rae_wasm_instantiate(int64_t handle, int64_t stackBytes, int64_t heapBytes, int64_t maxMemoryPages) {
  RaeWasmModule* m = (RaeWasmModule*)(intptr_t)handle;
  if (!m) return 0;
  RaeWasmInstance* c = (RaeWasmInstance*)calloc(1, sizeof *c);
  if (!c) return 0;
  c->module = m;
  c->stack_bytes = (uint32_t)stackBytes;
  char error[RAE_WASM_ERROR_BYTES] = "";
  InstantiationArgs args;
  memset(&args, 0, sizeof args);
  args.default_stack_size = (uint32_t)stackBytes;
  args.host_managed_heap_size = (uint32_t)heapBytes;
  args.max_memory_pages = (uint32_t)maxMemoryPages;
  c->inst = wasm_runtime_instantiate_ex(m->module, &args, error, sizeof error);
  if (!c->inst) { rae_wasm_set_error(error); free(c); return 0; }
  wasm_runtime_set_custom_data(c->inst, c);
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->changed, NULL);
  c->state = RAE_WASM_IDLE;
  pthread_mutex_lock(&c->lock);
  if (pthread_create(&c->worker, NULL, rae_wasm_worker, c) != 0) {
    pthread_mutex_unlock(&c->lock);
    wasm_runtime_deinstantiate(c->inst);
    rae_wasm_set_error("no thread for the instance");
    free(c);
    return 0;
  }
  while (c->started == 0) pthread_cond_wait(&c->changed, &c->lock);
  int started = c->started;
  pthread_mutex_unlock(&c->lock);
  if (started < 0) {
    rae_wasm_set_error("the instance's execution environment could not be made");
    rae_wasm_destroy((int64_t)(intptr_t)c);
    return 0;
  }
  return (int64_t)(intptr_t)c;
}

void rae_wasm_destroy(int64_t handle) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c) return;
  pthread_mutex_lock(&c->lock);
  c->state = RAE_WASM_QUIT;
  pthread_cond_broadcast(&c->changed);
  pthread_mutex_unlock(&c->lock);
  pthread_join(c->worker, NULL);
  wasm_runtime_deinstantiate(c->inst);
  pthread_mutex_destroy(&c->lock);
  pthread_cond_destroy(&c->changed);
  free(c);
}

void rae_wasm_arg(int64_t handle, int64_t index, int64_t kind, int64_t integer, double real) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || index < 0 || index >= RAE_WASM_MAX_VALUES) return;
  wasm_val_t* value = &c->args[index];
  value->kind = rae_wasm_valkind(kind);
  switch (value->kind) {
    case WASM_I32: value->of.i32 = (int32_t)integer; break;
    case WASM_I64: value->of.i64 = integer; break;
    case WASM_F32: value->of.f32 = (float)real; break;
    default: value->of.f64 = real; break;
  }
}

int64_t rae_wasm_call_start(int64_t handle, uint8_t* nameZ, int64_t count) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c) return -1;
  pthread_mutex_lock(&c->lock);
  if (c->state == RAE_WASM_CALL || c->state == RAE_WASM_RUNNING || c->state == RAE_WASM_HOST) {
    pthread_mutex_unlock(&c->lock);
    return -3;
  }
  wasm_function_inst_t function = wasm_runtime_lookup_function(c->inst, (const char*)nameZ);
  if (!function) { pthread_mutex_unlock(&c->lock); return -1; }
  uint32_t params = wasm_func_get_param_count(function, c->inst);
  uint32_t results = wasm_func_get_result_count(function, c->inst);
  if ((int64_t)params != count || params > RAE_WASM_MAX_VALUES || results > RAE_WASM_MAX_VALUES) {
    pthread_mutex_unlock(&c->lock);
    return -2;
  }
  wasm_valkind_t kinds[RAE_WASM_MAX_VALUES];
  wasm_func_get_param_types(function, c->inst, kinds);
  for (uint32_t i = 0; i < params; i++) {
    if (kinds[i] != c->args[i].kind) {
      /* the caller's number in the kind the function takes */
      wasm_val_t value = c->args[i];
      double real = value.kind == WASM_F32 ? value.of.f32 : value.kind == WASM_F64 ? value.of.f64 : 0;
      int64_t integer = value.kind == WASM_I32 ? value.of.i32 : value.kind == WASM_I64 ? value.of.i64 : (int64_t)real;
      if (value.kind == WASM_I32 || value.kind == WASM_I64) real = (double)integer;
      c->args[i].kind = kinds[i];
      if (kinds[i] == WASM_I32) c->args[i].of.i32 = (int32_t)integer;
      else if (kinds[i] == WASM_I64) c->args[i].of.i64 = integer;
      else if (kinds[i] == WASM_F32) c->args[i].of.f32 = (float)real;
      else c->args[i].of.f64 = real;
    }
  }
  wasm_valkind_t result_kinds[RAE_WASM_MAX_VALUES];
  wasm_func_get_result_types(function, c->inst, result_kinds);
  for (uint32_t i = 0; i < results; i++) c->results[i].kind = result_kinds[i];
  snprintf(c->function, sizeof c->function, "%s", (const char*)nameZ);
  c->arg_count = params;
  c->result_count = results;
  c->error[0] = '\0';
  c->state = RAE_WASM_CALL;
  pthread_cond_broadcast(&c->changed);
  pthread_mutex_unlock(&c->lock);
  return 0;
}

int64_t rae_wasm_call_wait(int64_t handle, int64_t timeoutMs) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c) return 3;
  struct timeval now;
  gettimeofday(&now, NULL);
  int64_t nanoseconds = (int64_t)now.tv_usec * 1000 + (timeoutMs % 1000) * 1000000;
  struct timespec deadline;
  deadline.tv_sec = now.tv_sec + (time_t)(timeoutMs / 1000) + (time_t)(nanoseconds / 1000000000);
  deadline.tv_nsec = (long)(nanoseconds % 1000000000);
  pthread_mutex_lock(&c->lock);
  while (c->state == RAE_WASM_CALL || c->state == RAE_WASM_RUNNING) {
    if (timeoutMs <= 0 || pthread_cond_timedwait(&c->changed, &c->lock, &deadline) != 0) break;
  }
  int state = c->state;
  pthread_mutex_unlock(&c->lock);
  if (state == RAE_WASM_HOST) return 1;
  if (state == RAE_WASM_FINISHED) return 2;
  if (state == RAE_WASM_TRAPPED) return 3;
  return 0;
}

int64_t rae_wasm_result_count(int64_t handle) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  return c ? c->result_count : 0;
}
int64_t rae_wasm_result_kind(int64_t handle, int64_t index) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || index < 0 || index >= c->result_count) return -1;
  return rae_wasm_kind_of(c->results[index].kind);
}
int64_t rae_wasm_result_int(int64_t handle, int64_t index) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || index < 0 || index >= c->result_count) return 0;
  return c->results[index].kind == WASM_I32 ? c->results[index].of.i32 : c->results[index].of.i64;
}
double rae_wasm_result_float(int64_t handle, int64_t index) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || index < 0 || index >= c->result_count) return 0;
  return c->results[index].kind == WASM_F32 ? c->results[index].of.f32 : c->results[index].of.f64;
}
int64_t rae_wasm_error_length(int64_t handle) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  return c ? (int64_t)strlen(c->error) : 0;
}
void rae_wasm_error_copy(int64_t handle, uint8_t* into) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (c) memcpy(into, c->error, strlen(c->error));
}
void rae_wasm_terminate(int64_t handle) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (c) wasm_runtime_terminate(c->inst);
}

/* ----- the waiting host call ----- */

int64_t rae_wasm_host_import(int64_t handle) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  return c ? c->host_import : -1;
}
int64_t rae_wasm_host_arg_int(int64_t handle, int64_t index) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || index < 0 || index >= c->host_arg_count) return 0;
  return c->host_args[index].kind == WASM_I32 ? c->host_args[index].of.i32 : c->host_args[index].of.i64;
}
double rae_wasm_host_arg_float(int64_t handle, int64_t index) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || index < 0 || index >= c->host_arg_count) return 0;
  return c->host_args[index].kind == WASM_F32 ? c->host_args[index].of.f32 : c->host_args[index].of.f64;
}
void rae_wasm_host_result(int64_t handle, int64_t index, int64_t integer, double real) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || index < 0 || index >= c->host_result_count) return;
  wasm_val_t* value = &c->host_results[index];
  if (value->kind == WASM_I32) value->of.i32 = (int32_t)integer;
  else if (value->kind == WASM_I64) value->of.i64 = integer;
  else if (value->kind == WASM_F32) value->of.f32 = (float)real;
  else value->of.f64 = real;
}
static void rae_wasm_host_release(RaeWasmInstance* c) {
  pthread_mutex_lock(&c->lock);
  if (c->state == RAE_WASM_HOST) c->state = RAE_WASM_RUNNING;
  pthread_cond_broadcast(&c->changed);
  pthread_mutex_unlock(&c->lock);
}
void rae_wasm_host_answer(int64_t handle) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (c) rae_wasm_host_release(c);
}
void rae_wasm_host_trap(int64_t handle, uint8_t* messageZ) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c) return;
  snprintf(c->host_message, sizeof c->host_message, "%s", (const char*)messageZ);
  c->host_trap = 1;
  rae_wasm_host_release(c);
}

/* ----- linear memory ----- */

int64_t rae_wasm_memory_size(int64_t handle) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c) return 0;
  wasm_memory_inst_t memory = wasm_runtime_get_default_memory(c->inst);
  if (!memory) return 0;
  return (int64_t)wasm_memory_get_cur_page_count(memory) * (int64_t)wasm_memory_get_bytes_per_page(memory);
}
int64_t rae_wasm_memory_read(int64_t handle, int64_t address, int64_t count, uint8_t* into) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || address < 0 || count < 0) return -1;
  if (count == 0) return 0;
  if (!wasm_runtime_validate_app_addr(c->inst, (uint64_t)address, (uint64_t)count)) {
    wasm_runtime_clear_exception(c->inst);
    return -1;
  }
  memcpy(into, wasm_runtime_addr_app_to_native(c->inst, (uint64_t)address), (size_t)count);
  return 0;
}
int64_t rae_wasm_memory_write(int64_t handle, int64_t address, uint8_t* from, int64_t offset, int64_t count) {
  RaeWasmInstance* c = (RaeWasmInstance*)(intptr_t)handle;
  if (!c || address < 0 || count < 0) return -1;
  if (count == 0) return 0;
  if (!wasm_runtime_validate_app_addr(c->inst, (uint64_t)address, (uint64_t)count)) {
    wasm_runtime_clear_exception(c->inst);
    return -1;
  }
  memcpy(wasm_runtime_addr_app_to_native(c->inst, (uint64_t)address), from + offset, (size_t)count);
  return 0;
}
