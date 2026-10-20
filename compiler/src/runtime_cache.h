#ifndef RAE_RUNTIME_CACHE_H
#define RAE_RUNTIME_CACHE_H

#include <stdbool.h>
#include <stddef.h>

/* The compiled C runtime (runtime/rae_runtime.c, a unity build of every
 * runtime_*.c), kept in a cache so a native build compiles it once per set
 * of flags instead of once per program. Writes the object's path into
 * `out_path`; false when it could not be compiled (the caller reports the
 * C error). `compile_command` is the gcc command WITHOUT the source file and
 * the output: every flag that reaches the runtime's compile. */
bool runtime_cached_object(const char* compile_command,
                           const char* runtime_dir,
                           char* out_path,
                           size_t cap);

#endif
