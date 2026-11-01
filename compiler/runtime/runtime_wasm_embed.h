/* The C glue of lib/wasm (docs/wasm-embedding.md): WAMR's fast interpreter
 * behind integer handles, one WAMR call per function where it can be. Rae
 * holds the policy: which imports a module may have, what a host function
 * does, how long a call may run.
 *
 * Host functions are Rae, and Rae cannot hand C a function to call. So a
 * call into an instance runs on the instance's own worker thread, and an
 * import the module calls is ONE generic trampoline: it posts a host call
 * (the import, its arguments) and waits; the Rae caller, waiting in
 * rae_wasm_call_wait, answers it (rae_wasm_host_*) and the call goes on.
 * The same wait is the watchdog: rae_wasm_terminate stops a running call
 * from any thread.
 *
 * Values cross as an Int (i32 / i64, sign-extended) or a Float64 (f32 /
 * f64), with the kind: 0 i32, 1 i64, 2 f32, 3 f64. Text is copied into a
 * buffer Rae sized from the matching *_length call. tools/wamr/build.sh
 * compiles the glue (tools/wamr/rae_wasm_glue.c) into librae_wamr.a with
 * WAMR. This header is only prototypes, in the runtime folder so that every
 * build finds it; a program that includes it (lib/wasm's `cheader`) links
 * that library. */
#ifndef RAE_RUNTIME_WASM_EMBED_H
#define RAE_RUNTIME_WASM_EMBED_H

#include <stdint.h>

/* The runtime, once per process: 1 when it is ready */
int64_t rae_wasm_init(void);

/* A module from `count` bytes (copied): its handle, or 0 with the reason in
 * rae_wasm_last_error */
int64_t rae_wasm_load(uint8_t* bytes, int64_t offset, int64_t count);
int64_t rae_wasm_last_error_length(void);
void rae_wasm_last_error_copy(uint8_t* into);
void rae_wasm_unload(int64_t module);

/* The module's imports: kind (0 function, 1 table, 2 memory, 3 global),
 * names, and a function's parameter / result kinds */
int64_t rae_wasm_import_count(int64_t module);
int64_t rae_wasm_import_kind(int64_t module, int64_t index);
int64_t rae_wasm_import_module_length(int64_t module, int64_t index);
void rae_wasm_import_module_copy(int64_t module, int64_t index, uint8_t* into);
int64_t rae_wasm_import_name_length(int64_t module, int64_t index);
void rae_wasm_import_name_copy(int64_t module, int64_t index, uint8_t* into);
int64_t rae_wasm_import_param_count(int64_t module, int64_t index);
int64_t rae_wasm_import_param_kind(int64_t module, int64_t index, int64_t param);
int64_t rae_wasm_import_result_count(int64_t module, int64_t index);
int64_t rae_wasm_import_result_kind(int64_t module, int64_t index, int64_t result);

/* Let the module call function import `index` (the trampoline answers it);
 * then rae_wasm_link loads the module again so the granted imports link */
int64_t rae_wasm_grant(int64_t module, int64_t index);
int64_t rae_wasm_link(int64_t module);

/* An instance: its handle, or 0 (rae_wasm_last_error). `maxMemoryPages` caps
 * its linear memory (64 KiB pages; 0 = the module's own maximum). */
int64_t rae_wasm_instantiate(int64_t module, int64_t stackBytes, int64_t heapBytes, int64_t maxMemoryPages);
void rae_wasm_destroy(int64_t instance);

/* A call by export name with `count` arguments set by rae_wasm_arg first;
 * 0 when it started, -1 when there is no such function, -2 when the
 * arguments do not match it, -3 when a call is running already */
void rae_wasm_arg(int64_t instance, int64_t index, int64_t kind, int64_t integer, double real);
int64_t rae_wasm_call_start(int64_t instance, uint8_t* nameZ, int64_t count);
/* Wait up to `timeoutMs` for the call: 0 still running, 1 a host call is
 * waiting, 2 finished, 3 trapped */
int64_t rae_wasm_call_wait(int64_t instance, int64_t timeoutMs);
int64_t rae_wasm_result_count(int64_t instance);
int64_t rae_wasm_result_kind(int64_t instance, int64_t index);
int64_t rae_wasm_result_int(int64_t instance, int64_t index);
double rae_wasm_result_float(int64_t instance, int64_t index);
int64_t rae_wasm_error_length(int64_t instance);
void rae_wasm_error_copy(int64_t instance, uint8_t* into);
/* Stop the running call (any thread): it traps, "terminated" */
void rae_wasm_terminate(int64_t instance);

/* The waiting host call: its import's index in the module, its arguments,
 * then its results and the answer (or a trap with a message) */
int64_t rae_wasm_host_import(int64_t instance);
int64_t rae_wasm_host_arg_int(int64_t instance, int64_t index);
double rae_wasm_host_arg_float(int64_t instance, int64_t index);
void rae_wasm_host_result(int64_t instance, int64_t index, int64_t integer, double real);
void rae_wasm_host_answer(int64_t instance);
void rae_wasm_host_trap(int64_t instance, uint8_t* messageZ);

/* The instance's linear memory: its size in bytes, and copies in and out
 * (0, or -1 outside the memory) */
int64_t rae_wasm_memory_size(int64_t instance);
int64_t rae_wasm_memory_read(int64_t instance, int64_t address, int64_t count, uint8_t* into);
int64_t rae_wasm_memory_write(int64_t instance, int64_t address, uint8_t* from, int64_t offset, int64_t count);

#endif
