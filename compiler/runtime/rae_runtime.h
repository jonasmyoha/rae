#ifndef RAE_RUNTIME_H
#define RAE_RUNTIME_H

/* Never fuse `a * b + c` into one FMA instruction. Clang does by default on
 * arm64 (and wherever FMA exists), which rounds once instead of twice: the
 * same Rae program then computes different floats on different machines, and
 * the physics port (docs/physics-rae-port-design.md §4) must match Box3D's
 * reference build bit for bit. Every Rae compile line also passes
 * -ffp-contract=off; this pragma is the backstop for builds that do not (an
 * IDE project, a hand-written recipe), since every Rae translation unit
 * includes this header first. GCC ignores the pragma and relies on the flag. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <pthread.h>

/* The C twin of lib/net/NetSystem.rae's `hasKqueue` capability
 * (Target.isApple or Target.isBsd; docs/platform-conditional-code.md): the
 * kqueue shims of runtime_net.c and runtime_file_notify.c exist exactly
 * where the Rae side declares them. */
#if (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)) && \
    !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#define RAE_HAS_KQUEUE 1
#endif

/* The C twin of lib/net/NetSystem.rae's `hasPosixSockets` (Target.isUnix):
 * runtime_net.c's socket shims exist exactly where the Rae side declares them */
#if (defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
     defined(__NetBSD__)) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#define RAE_HAS_POSIX_SOCKETS 1
#endif

/* Ptr lowers directly to void* in ordinary declarations. Generic storage also
 * needs an identifier-safe spelling because the concrete type is embedded in
 * generated List/function names. This is the canonical TypeInfo spelling for
 * Buffer(void), which is Ptr's internal representation. It carries no
 * ownership and exists only at the native interop boundary. */
typedef void* Buffer_void;

/* Compiled-backend task runtime (Task(T)). The Rae type Task(T) lowers to
 * `RaeTask*` (type-erased): the worker thread stores its T result into the
 * `result` buffer (malloc'd to sizeof(T)); `task.get()` joins and reads it.
 * One pthread per spawned task; join happens exactly once (guarded). */
typedef struct {
  pthread_t thread;
  void* result;   /* malloc'd to sizeof(T), or NULL for a void task */
  _Atomic int done; /* stored by the worker after the result (a release); read
                       by isDone/tryGet on the owner's thread (an acquire) */
  int joined;     /* pthread_join called once */
  int taken;      /* tryGet handed the result out (owner's thread only) */
  /* A task on the scheduler's worker pool (lib/core/Scheduler.rae) instead
   * of a thread of its own: the thunk and its arguments, run by a worker */
  int on_pool;
  void* (*thunk)(void*);
  void* args;
  /* A resumable task (docs/lightweight-spawn-design.md §13): `step` runs it
   * until it finishes (1) or suspends (0) on another task or a timer; the
   * task waiting on this one, 0 when none, -1 once this one finished */
  int (*step)(void* args);
  _Atomic intptr_t waiter;
  /* A resumable task suspended on a socket (runtime_sched_io.c): what it
   * registered, under the scheduler's io lock */
  int io_armed;
  int io_filter;
  int io_timer;
  int64_t io_fd;
} RaeTask;

RaeTask* rae_task_new(size_t result_size);
/* Start a spawned task's thunk on a thread, or on the caller when no thread
 * can be had (runtime_threads.c) */
void rae_task_start(RaeTask* t, void* (*thunk)(void*), void* args);
/* Make `t` a scheduler task: the generated spawn site then hands it to
 * lib/core/Scheduler.rae's schedulerSubmit (runtime_threads.c) */
void rae_task_prepare(RaeTask* t, void* (*thunk)(void*), void* args);
/* The same for a resumable task: `step` is called until it answers 1 */
void rae_task_prepare_step(RaeTask* t, int (*step)(void* args), void* args);
/* A task's result is stored: mark it done and wake the task waiting on it */
void rae_task_complete(RaeTask* t);
/* Inside a resumable task: suspend until `t` is done (1), or 0 when it is
 * done already (or another task waits on it: then the get() waits) */
int rae_sched_wait_task(RaeTask* t);
/* Inside a resumable task: suspend until the monotonic clock reaches
 * `deadline_ns` (1), or 0 when it has */
int rae_sched_sleep_until(int64_t deadline_ns);
/* Inside a resumable task: suspend until `fd` is ready (`events`: poll bits,
 * -1 readable) or `timeout_ms` passes (1), or 0 when it cannot suspend
 * (runtime_sched_io.c) */
int rae_sched_io_wait(int64_t fd, int64_t events, int64_t timeout_ms);
void rae_sched_install_io(void (*io_loop)(void), void (*frame)(void));
/* A browser frame of `mainLoop`: run the tasks that can go on */
void rae_sched_frame(void);
/* Whether a socket wait suspends a task here: a spawn that can reach one goes
 * on the pool only then (else it is a thread, its waits blocking it) */
#ifdef RAE_HAS_KQUEUE
#define RAE_SCHED_IO_SUSPENDS 1
#else
#define RAE_SCHED_IO_SUSPENDS 0
#endif
int64_t rae_sched_now_ns(void);
/* A resumable frame's saved local: storage of `size` bytes that never moves */
void* rae_frame_slot(void** slot, size_t size);
void rae_frame_slots_free(void** slots, int count);
/* The policy half of the scheduler is Rae (lib/core/Scheduler.rae); the
 * generated program installs it at the start of main: the worker loop the
 * pool's threads run, the wait for an unfinished task, the default worker
 * count and a parallelLoop's chunk size. Without it (a program built without
 * the prelude) the runtime keeps its own C fallbacks. */
void rae_sched_install(void (*worker_loop)(int64_t index), void (*task_wait)(int64_t task),
                       int64_t (*default_workers)(void), int64_t (*chunk_size)(int64_t total, int64_t workers),
                       void (*wake)(int64_t task), void (*group_wait)(int64_t group));
/* A taskScope whose spawns lend views (docs/lightweight-spawn-design.md
 * §14.2) counts the tasks that borrow from it and waits for them before it
 * ends, wherever their handles went */
typedef struct { _Atomic int64_t pending; } RaeTaskGroup;
static inline void rae_group_add(RaeTaskGroup* g) {
  atomic_fetch_add_explicit(&g->pending, 1, memory_order_relaxed);
}
void rae_group_done(RaeTaskGroup* g);
void rae_group_wait(RaeTaskGroup* g);
/* parallelLoop (runtime_threads.c): run body(captures, first, end) over
 * [start, end) in chunks on the worker pool, returning when all are done. */
typedef void (*RaeParallelBody)(void* captures, int64_t first, int64_t end);
void rae_parallel_for(int64_t start, int64_t end, RaeParallelBody body, void* captures);
int64_t rae_ext_Parallel_workerCount(void);
/* RAE_PARALLEL_CHECK (runtime_threads.c): an `unsafe` List write in a
 * parallelLoop body reports its element; two iterations writing one stop the
 * program. */
void rae_parallel_check_write(const void* storage, int64_t index, int64_t iteration, const char* name);
void* rae_task_await(RaeTask* t);   /* join once; returns the result buffer */
/* Task(T).isDone(): the worker has stored its result. Never blocks. */
static inline int rae_task_is_done(RaeTask* t) {
  return t && atomic_load_explicit(&t->done, memory_order_acquire);
}
/* Task(T).tryGet(): true exactly once, when the task has finished and its
 * result was not handed out yet; the caller then joins (instant) and moves the
 * result out (c_expr.c). Only the owner's thread touches `taken`. */
static inline int rae_task_claim(RaeTask* t) {
  if (!t || t->taken || !rae_task_is_done(t)) return 0;
  t->taken = 1;
  return 1;
}
void rae_task_drop(RaeTask* t);     /* join (if not joined) + free; scope-exit drop */

#ifdef __GNUC__
#define RAE_UNUSED __attribute__((unused))
#else
#define RAE_UNUSED
#endif

/* ---- Int arithmetic (docs/integer-semantics.md) ----
 *
 * The C backend routes every `+ - * / %` on `Int` (signed 64-bit) through
 * these, so the language defines what C leaves undefined — and defines it
 * the SAME way in every build profile (a program that passed its tests
 * behaves identically when shipped):
 *   - `/` and `%` by zero are a runtime error: one line
 *     `file:line: runtime error: division by zero` and exit RAE_TRAP_EXIT_CODE.
 *     Returning 0 (what the bare C divide happened to do on arm64) or a
 *     SIGFPE are both wrong answers. `Int.min / -1` (the one other
 *     overflowing divide) traps too.
 *   - `+ - *` WRAP two's-complement, computed in unsigned so the wrap is
 *     defined behaviour rather than UB the optimizer may assume away. This is
 *     what every language with fixed-size integers does in production, and
 *     what a hash (lib/Noise) relies on.
 * A constant divisor of zero never reaches here: sema rejects it. */
#define RAE_TRAP_EXIT_CODE 70   /* sysexits EX_SOFTWARE: an internal, defined trap, not a crash */
static inline void rae_int_trap(const char* file, int line, const char* what) {
    fprintf(stderr, "%s:%d: runtime error: %s\n", file, line, what);
    fflush(stderr);
    exit(RAE_TRAP_EXIT_CODE);
}
static inline int64_t rae_int_div(int64_t a, int64_t b, const char* file, int line) {
    if (b == 0) rae_int_trap(file, line, "division by zero");
    if (a == INT64_MIN && b == -1) rae_int_trap(file, line, "integer overflow: Int.min / -1");
    return a / b;
}
static inline int64_t rae_int_mod(int64_t a, int64_t b, const char* file, int line) {
    if (b == 0) rae_int_trap(file, line, "division by zero (modulo)");
    if (b == -1) return 0;   /* Int.min % -1 is 0 mathematically, UB in C */
    return a % b;
}
static inline int64_t rae_int_add(int64_t a, int64_t b, const char* file, int line) {
    (void)file; (void)line; return (int64_t)((uint64_t)a + (uint64_t)b);
}
static inline int64_t rae_int_sub(int64_t a, int64_t b, const char* file, int line) {
    (void)file; (void)line; return (int64_t)((uint64_t)a - (uint64_t)b);
}
static inline int64_t rae_int_mul(int64_t a, int64_t b, const char* file, int line) {
    (void)file; (void)line; return (int64_t)((uint64_t)a * (uint64_t)b);
}

typedef uint32_t rae_Char32;
typedef uint32_t rae_Char;

#ifdef RAE_HAS_RAYLIB
typedef bool rae_Bool;
#else
typedef int8_t rae_Bool;
#endif

/* A mainLoop's frame, for a window system that blocks the loop (rae_runtime.c) */
void rae_set_live_frame(rae_Bool (*frame)(void*), void* state);

// Rae's owned-vs-borrowed String. `data` is the UTF-8 byte buffer
// (we keep it NUL-terminated in heap-allocated strings for cheap
// interop with C string APIs, but length is the source of truth).
//
// Ownership model (see docs/scope-exit-dealloc.md Layer 5 + the
// String-ownership design):
//   - is_owned == 0: borrowed / static literal. `drop` must NOT free.
//                    capacity is meaningless (kept 0).
//   - is_owned == 1: owned heap allocation made with malloc.
//                    `drop` frees `data`. `capacity` is the malloc'd
//                    size (in bytes, including the trailing NUL).
//
// Positional initializers like `(rae_String){p, n}` from generated
// C still work — C99 zero-fills the remaining fields, giving
// {data=p, len=n, capacity=0, is_owned=0}. That's the correct shape
// for a borrowed literal, which is what those sites mean.
typedef struct {
  uint8_t* data;
  int64_t len;
  int64_t capacity;
  int8_t is_owned;
} rae_String;

typedef enum {
  RAE_TYPE_NONE,
  RAE_TYPE_INT64,
  RAE_TYPE_INT32,
  RAE_TYPE_UINT64,
  RAE_TYPE_UINT32,
  RAE_TYPE_FLOAT64,
  RAE_TYPE_FLOAT32,
  RAE_TYPE_BOOL,
  RAE_TYPE_STRING,
  RAE_TYPE_CHAR,
  RAE_TYPE_ID,
  RAE_TYPE_KEY,
  RAE_TYPE_LIST,
  RAE_TYPE_BUFFER,
  RAE_TYPE_ANY
} RaeType;

typedef void (*RaeAnyDropFn)(void*);

typedef struct {
  void* data;
  int64_t length;
  int64_t capacity;
} RaeList;

typedef struct {
  void* data;
  int64_t length;
  int64_t capacity;
} RaeMap;

typedef struct {
  RaeType type;
  bool is_view;
  bool is_mod;
  union {
    int64_t i;
    double f;
    int8_t b;
    rae_String s;
    void* ptr;
  } as;
  RaeAnyDropFn drop;
} RaeAny;

void rae_flush_stdout(void);
void* rae_ext_rae_buf_alloc(int64_t count, int64_t elem_size);
void rae_ext_rae_buf_free(void* buf);
void* rae_ext_rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size);
void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elem_size);
/* `count` elements from `start` set to zero bytes, every type's default value
 * (List.addDefaults). One C-library call. */
/* `count` bytes of `s` from `start` copied into `buf` at byte `offset`
 * (memcpy; the caller checked both ranges). One C-library call. */
static inline void rae_ext_rae_str_copy_into(rae_String s, int64_t start, int64_t count, void* buf, int64_t offset) {
  if (buf && s.data && count > 0) memcpy((char*)buf + offset, s.data + start, (size_t)count);
}
static inline void rae_ext_rae_buf_zero(void* buf, int64_t start, int64_t count, int64_t elem_size) {
  if (buf && count > 0) memset((char*)buf + start * elem_size, 0, (size_t)(count * elem_size));
}
void rae_ext_rae_buf_set(void* buf, int64_t index, int64_t elem_size, const void* value);
void rae_ext_rae_buf_get(void* buf, int64_t index, int64_t elem_size, void* out_val);

/* Legacy buffer primitives for VM (where everything is still boxed in RaeAny/Value) */
void rae_ext_rae_buf_set_any(void* buf, int64_t index, RaeAny value);
RaeAny rae_ext_rae_buf_get_any(void* buf, int64_t index);

/* Legacy buffer intrinsics (single-arg alloc, value-sized elements) */
RAE_UNUSED static void* rae_ext___buf_alloc(int64_t count) { return rae_ext_rae_buf_alloc(count, sizeof(int64_t)); }
RAE_UNUSED static void rae_ext___buf_free(void* buf) { rae_ext_rae_buf_free(buf); }
/* List.prefetch: a hint that element `index` (elem_size bytes) is read
 * soon; every cache line of it is prefetched. It changes no result. */
static inline void rae_ext_rae_buf_prefetch(const void* buf, int64_t index, int64_t elem_size) {
  const char* element = (const char*)buf + index * elem_size;
  for (int64_t offset = 0; offset < elem_size; offset += 64) __builtin_prefetch(element + offset);
}
RAE_UNUSED static void rae_ext___buf_set_i64(void* buf, int64_t index, int64_t value) { if (buf) ((int64_t*)buf)[index] = value; }
RAE_UNUSED static void rae_ext___buf_set_any(void* buf, int64_t index, RaeAny value) { if (buf) ((int64_t*)buf)[index] = value.as.i; }
#define rae_ext___buf_set(buf, index, value) _Generic((value), \
    int64_t: rae_ext___buf_set_i64, \
    RaeAny: rae_ext___buf_set_any \
)(buf, index, value)
RAE_UNUSED static int64_t rae_ext___buf_get(void* buf, int64_t index) { return buf ? ((int64_t*)buf)[index] : 0; }
RAE_UNUSED static void rae_ext___buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len) {
    if (src && dst && len > 0) memmove((int64_t*)dst + dst_off, (int64_t*)src + src_off, (size_t)len * sizeof(int64_t));
}


/* Conversion Helpers */
RAE_UNUSED float rae_ext_rae_int_to_float(int64_t v);
RAE_UNUSED int64_t rae_ext_rae_float_to_int(float v);
/* A Float's IEEE-754 bits (lib/Math floatBits); inline like the Math functions. */
static inline int64_t rae_ext_rae_f32_bits(float v) { uint32_t b; memcpy(&b, &v, 4); return (int64_t)b; }
RAE_UNUSED static RaeAny rae_any_int(int64_t v) { return (RaeAny){.type = RAE_TYPE_INT64, .as.i = v}; }
RAE_UNUSED static RaeAny rae_any_int32(int32_t v) { return (RaeAny){.type = RAE_TYPE_INT32, .as.i = v}; }
RAE_UNUSED static RaeAny rae_any_uint64(uint64_t v) { return (RaeAny){.type = RAE_TYPE_UINT64, .as.i = (int64_t)v}; }
RAE_UNUSED static RaeAny rae_any_int_ptr(const int64_t* v) { return (RaeAny){.type = RAE_TYPE_INT64, .is_view = true, .as.ptr = (void*)v}; }
RAE_UNUSED static RaeAny rae_any_float(double v) { return (RaeAny){.type = RAE_TYPE_FLOAT64, .as.f = v}; }
RAE_UNUSED static RaeAny rae_any_float32(float v) { return (RaeAny){.type = RAE_TYPE_FLOAT32, .as.f = v}; }
RAE_UNUSED static RaeAny rae_any_float_ptr(const void* v) { return (RaeAny){.type = RAE_TYPE_FLOAT64, .is_view = true, .as.ptr = (void*)v}; }
RAE_UNUSED static RaeAny rae_any_bool(int8_t v) { return (RaeAny){.type = RAE_TYPE_BOOL, .as.b = v}; }
RAE_UNUSED static RaeAny rae_any_char(uint32_t v) { return (RaeAny){.type = RAE_TYPE_CHAR, .as.i = (int64_t)v}; }
RAE_UNUSED static RaeAny rae_any_char_ptr(const uint32_t* v) { return (RaeAny){.type = RAE_TYPE_CHAR, .is_view = true, .as.ptr = (void*)v}; }
RAE_UNUSED static RaeAny rae_any_string(rae_String v) { return (RaeAny){.type = RAE_TYPE_STRING, .as.s = v}; }
RAE_UNUSED static RaeAny rae_any_string_ptr(const rae_String* v) { return (RaeAny){.type = RAE_TYPE_STRING, .is_view = true, .as.ptr = (void*)v}; }

RAE_UNUSED static RaeAny rae_any_none(void) { return (RaeAny){.type = RAE_TYPE_NONE}; }
RAE_UNUSED static RaeAny rae_any_ptr(void* v) { return (RaeAny){.type = RAE_TYPE_BUFFER, .as.ptr = v}; }
RAE_UNUSED static RaeAny rae_any_owned_ptr(void* v, RaeAnyDropFn drop) {
    RaeAny out = {.type = RAE_TYPE_BUFFER, .as.ptr = v};
    out.drop = drop;
    return out;
}

/* A view boxed as Any: the box's pointer is untyped and its `is_view` flag
 * is what keeps it read-only, so const is cast away here, once. */
RAE_UNUSED static RaeAny rae_any_view(const void* v, RaeType type) {
    if (type == RAE_TYPE_ANY) {
         const RaeAny* res = (const RaeAny*)v;
         if (res->is_view || res->is_mod) return *res;
         RaeAny out = *res;
         out.is_view = true;
         return out;
    }
    return (RaeAny){.type = type, .is_view = true, .as.ptr = (void*)v};
}

RAE_UNUSED static RaeAny rae_any_mod(void* v, RaeType type) {
    if (type == RAE_TYPE_ANY) {
         RaeAny* res = (RaeAny*)v;
         if (res->is_view || res->is_mod) return *res;
         RaeAny out = *res;
         out.is_mod = true;
         return out;
    }
    return (RaeAny){.type = type, .is_mod = true, .as.ptr = v};
}

RAE_UNUSED static RaeAny rae_any_identity(RaeAny a) { return a; }
RAE_UNUSED static RaeAny rae_any_identity_ptr(const RaeAny* a) { 
    RaeAny res = *a;
    res.is_view = true;
    return res;
}

// Helpers for reference structs
typedef struct { const int64_t* ptr; } rae_View_Int64;
typedef struct { int64_t* ptr; } rae_Mod_Int64;
typedef struct { const int32_t* ptr; } rae_View_Int32;
typedef struct { int32_t* ptr; } rae_Mod_Int32;
typedef struct { const uint64_t* ptr; } rae_View_UInt64;
typedef struct { uint64_t* ptr; } rae_Mod_UInt64;
typedef struct { const uint32_t* ptr; } rae_View_UInt32;
typedef struct { uint32_t* ptr; } rae_Mod_UInt32;
/* Float (== Float32, f32) is Rae's default float; Float64 is the explicit
 * high-precision type. Both view/mod wrappers exist so the two never alias
 * each other's pointer width. */
typedef struct { const float* ptr; }  rae_View_Float;
typedef struct { float* ptr; }  rae_Mod_Float;
typedef struct { const double* ptr; } rae_View_Float64;
typedef struct { double* ptr; } rae_Mod_Float64;
typedef struct { const float* ptr; } rae_View_Float32;
typedef struct { float* ptr; } rae_Mod_Float32;
typedef struct { const rae_Bool* ptr; } rae_View_Bool;
typedef struct { rae_Bool* ptr; } rae_Mod_Bool;
typedef struct { const uint32_t* ptr; } rae_View_Char32;
typedef struct { uint32_t* ptr; } rae_Mod_Char32;
typedef struct { const uint32_t* ptr; } rae_View_Char;
typedef struct { uint32_t* ptr; } rae_Mod_Char;
typedef struct { const rae_String* ptr; } rae_View_String;
typedef struct { rae_String* ptr; } rae_Mod_String;

RAE_UNUSED static RaeAny rae_any_view_int64(rae_View_Int64 v) { return rae_any_view(v.ptr, RAE_TYPE_INT64); }
RAE_UNUSED static RaeAny rae_any_mod_int64(rae_Mod_Int64 v) { return rae_any_mod(v.ptr, RAE_TYPE_INT64); }
RAE_UNUSED static RaeAny rae_any_view_int32(rae_View_Int32 v) { return rae_any_view(v.ptr, RAE_TYPE_INT32); }
RAE_UNUSED static RaeAny rae_any_mod_int32(rae_Mod_Int32 v) { return rae_any_mod(v.ptr, RAE_TYPE_INT32); }
RAE_UNUSED static RaeAny rae_any_view_uint64(rae_View_UInt64 v) { return rae_any_view(v.ptr, RAE_TYPE_UINT64); }
RAE_UNUSED static RaeAny rae_any_mod_uint64(rae_Mod_UInt64 v) { return rae_any_mod(v.ptr, RAE_TYPE_UINT64); }
RAE_UNUSED static RaeAny rae_any_view_uint32(rae_View_UInt32 v) { return rae_any_view(v.ptr, RAE_TYPE_UINT32); }
RAE_UNUSED static RaeAny rae_any_mod_uint32(rae_Mod_UInt32 v) { return rae_any_mod(v.ptr, RAE_TYPE_UINT32); }
RAE_UNUSED static RaeAny rae_any_view_float64(rae_View_Float64 v) { return rae_any_view(v.ptr, RAE_TYPE_FLOAT64); }
RAE_UNUSED static RaeAny rae_any_mod_float64(rae_Mod_Float64 v) { return rae_any_mod(v.ptr, RAE_TYPE_FLOAT64); }
RAE_UNUSED static RaeAny rae_any_view_float32(rae_View_Float32 v) { return rae_any_view(v.ptr, RAE_TYPE_FLOAT32); }
RAE_UNUSED static RaeAny rae_any_mod_float32(rae_Mod_Float32 v) { return rae_any_mod(v.ptr, RAE_TYPE_FLOAT32); }
RAE_UNUSED static RaeAny rae_any_view_bool(rae_View_Bool v) { return rae_any_view(v.ptr, RAE_TYPE_BOOL); }
RAE_UNUSED static RaeAny rae_any_mod_bool(rae_Mod_Bool v) { return rae_any_mod(v.ptr, RAE_TYPE_BOOL); }
RAE_UNUSED static RaeAny rae_any_view_char32(rae_View_Char32 v) { return rae_any_view(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_mod_char32(rae_Mod_Char32 v) { return rae_any_mod(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_view_char(rae_View_Char v) { return rae_any_view(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_mod_char(rae_Mod_Char v) { return rae_any_mod(v.ptr, RAE_TYPE_CHAR); }
RAE_UNUSED static RaeAny rae_any_view_string(rae_View_String v) { return rae_any_view(v.ptr, RAE_TYPE_STRING); }
RAE_UNUSED static RaeAny rae_any_mod_string(rae_Mod_String v) { return rae_any_mod(v.ptr, RAE_TYPE_STRING); }

RAE_UNUSED static bool rae_any_is_none(RaeAny a) { return a.type == RAE_TYPE_NONE; }
RAE_UNUSED static bool rae_any_eq(RaeAny a, RaeAny b) {
    if (a.type != b.type) return false;
    if (a.type == RAE_TYPE_NONE) return true;
    if (a.type == RAE_TYPE_STRING) {
        if (a.as.s.len != b.as.s.len) return false;
        if (a.as.s.len == 0) return true;
        if (!a.as.s.data || !b.as.s.data) return a.as.s.data == b.as.s.data;
        return memcmp(a.as.s.data, b.as.s.data, a.as.s.len) == 0;
    }
    return a.as.i == b.as.i;
}

RAE_UNUSED static RaeAny rae_any_bool_ptr(rae_Bool* v) { return rae_any_view(v, RAE_TYPE_BOOL); }

#define rae_any(X) _Generic(((X)), \
    int64_t: rae_any_int, \
    int32_t: rae_any_int32, \
    uint64_t: rae_any_uint64, \
    double: rae_any_float, \
    float: rae_any_float32, \
    rae_String: rae_any_string, \
    RaeAny: rae_any_identity, \
    RaeAny*: rae_any_identity_ptr, \
    bool: rae_any_bool, \
    int8_t: rae_any_bool, \
    rae_Bool*: rae_any_bool_ptr, \
    uint32_t: rae_any_char, \
    uint8_t: rae_any_int, \
    rae_View_Int64: rae_any_view_int64, \
    rae_Mod_Int64: rae_any_mod_int64, \
    rae_View_Int32: rae_any_view_int32, \
    rae_Mod_Int32: rae_any_mod_int32, \
    rae_View_UInt64: rae_any_view_uint64, \
    rae_Mod_UInt64: rae_any_mod_uint64, \
    rae_View_UInt32: rae_any_view_uint32, \
    rae_Mod_UInt32: rae_any_mod_uint32, \
    rae_View_Float64: rae_any_view_float64, \
    rae_Mod_Float64: rae_any_mod_float64, \
    rae_View_Float32: rae_any_view_float32, \
    rae_Mod_Float32: rae_any_mod_float32, \
    rae_View_Bool: rae_any_view_bool, \
    rae_Mod_Bool: rae_any_mod_bool, \
    rae_View_Char32: rae_any_view_char32, \
    rae_Mod_Char32: rae_any_mod_char32, \
    rae_View_Char: rae_any_view_char, \
    rae_Mod_Char: rae_any_mod_char, \
    rae_View_String: rae_any_view_string, \
    rae_Mod_String: rae_any_mod_string, \
    default: rae_any_ptr \
)(X)

/* Reading an Any box (the compiler's boxed-value ABI) from Rae:
 * lib/core/AnyText.rae formats it. Each is one field read; a `view` / `mod`
 * box holds a pointer to the value, read at the box type's width. A box of
 * kind RAE_TYPE_ANY points at another box: rae_ext_rae_any_resolved follows it. */
static inline RaeAny rae_ext_rae_any_resolved(RaeAny value) {
  while (value.type == RAE_TYPE_ANY && value.as.ptr) {
    RaeAny inner = *(RaeAny*)value.as.ptr;
    if (value.is_view) inner.is_view = true;
    if (value.is_mod) inner.is_mod = true;
    value = inner;
  }
  return value;
}
static inline int64_t rae_ext_rae_any_kind(RaeAny value) { return (int64_t)value.type; }
static inline int64_t rae_ext_rae_any_int(RaeAny value) {
  bool ref = value.is_view || value.is_mod;
  switch (value.type) {
    case RAE_TYPE_INT32: return ref ? *(int32_t*)value.as.ptr : (int32_t)value.as.i;
    case RAE_TYPE_UINT32: case RAE_TYPE_CHAR: return ref ? *(uint32_t*)value.as.ptr : (uint32_t)value.as.i;
    default: return ref ? *(int64_t*)value.as.ptr : value.as.i;
  }
}
static inline uint64_t rae_ext_rae_any_uint64(RaeAny value) {
  return (value.is_view || value.is_mod) ? *(uint64_t*)value.as.ptr : (uint64_t)value.as.i;
}
static inline double rae_ext_rae_any_float(RaeAny value) {
  bool ref = value.is_view || value.is_mod;
  if (value.type == RAE_TYPE_FLOAT32) return ref ? *(float*)value.as.ptr : (float)value.as.f;
  return ref ? *(double*)value.as.ptr : value.as.f;
}
static inline rae_Bool rae_ext_rae_any_bool(RaeAny value) {
  return (value.is_view || value.is_mod) ? *(int8_t*)value.as.ptr : value.as.b;
}
rae_String rae_ext_rae_str_from_buf(const uint8_t* data, int64_t len);
/* A String or Key box's text as an OWNED copy (the box only borrows it) */
static inline rae_String rae_ext_rae_any_string(RaeAny value) {
  rae_String text = (value.is_view || value.is_mod) ? *(rae_String*)value.as.ptr : value.as.s;
  return rae_ext_rae_str_from_buf(text.data, text.len);
}
static inline rae_Bool rae_ext_rae_any_string_missing(RaeAny value) {
  rae_String text = (value.is_view || value.is_mod) ? *(rae_String*)value.as.ptr : value.as.s;
  return text.data == NULL;
}
static inline int64_t rae_ext_rae_any_address(RaeAny value) { return (int64_t)(intptr_t)value.as.ptr; }

/* log / logS write their text (runtime_system_log.c) */
void rae_ext_rae_log_write(rae_String text, rae_Bool newline);


rae_String rae_ext_rae_str_from_cstr(const void* s);
rae_String rae_ext_rae_str_from_buf(const uint8_t* data, int64_t len);
void* rae_ext_rae_str_to_cstr(rae_String s);
// Free `s.data` only when `s.is_owned`. Safe to call on borrowed
// strings / static literals — those are no-ops. Codegen calls this
// from auto-emitted scope-exit drops AND from explicit `drop(s)`.
void rae_ext_rae_str_free(rae_String s);

// Deep-copy: returns an independently-owned String with freshly
// malloc'd data. Used by codegen wherever Rae semantics call for
// a value copy: `let b: String = a`, struct field init with a
// String, pass-by-value into a `String` (not `view String`) param.
rae_String rae_string_copy(rae_String src);

// Rae extern alias for rae_string_copy — Rae's extern resolution
// expects the C symbol to share the `func ...` declaration name,
// and `rae_ext_rae_string_copy` is what lib/string.rae declares.
// Takes a `view String` (rae_View_String in C) so the Rae signature
// matches; derefs and forwards.
RAE_UNUSED static inline rae_String rae_ext_rae_string_copy(rae_View_String src) {
  if (!src.ptr) return (rae_String){NULL, 0, 0, 0};
  return rae_string_copy(*src.ptr);
}

// Construct a borrowed/literal String. data must outlive the
// returned struct (typically because it's a static literal or a
// pool entry the compiler emitted). is_owned=0, capacity=0.
RAE_UNUSED static inline rae_String rae_string_from_literal(const uint8_t* data, int64_t len) {
  return (rae_String){(uint8_t*)data, len, 0, 0};
}

// Pointer-form drop. Existing `rae_ext_rae_str_free` takes by value
// for legacy ABI reasons; the auto-drop pass prefers pointer form
// so it can clear is_owned to prevent double-free if a drop site
// gets reached twice through some path.
void rae_ext_rae_str_free(rae_String s);
RAE_UNUSED static inline void rae_string_drop(rae_String* s) {
  if (!s) return;
  if (s->is_owned && s->data) {
    // Route through rae_ext_rae_str_free so the temp pool is kept
    // in sync (pool_remove) and mem-stats sees the free (untag).
    // Without this, a String dropped via cascade looks like a
    // permanent leak under RAE_MEM_STATS=1 and a subsequent pool
    // flush would double-free the same heap.
    rae_ext_rae_str_free(*s);
  }
  s->data = NULL;
  s->len = 0;
  s->capacity = 0;
  s->is_owned = 0;
}

// Borrow constructor: take an existing rae_String and return a
// borrowed view (is_owned=0). Used by codegen to wrap identifier
// refs that get fed into rae_ext_rae_str_interp, so the helper
// doesn't free the user's local at the end of the interpolation.
RAE_UNUSED static inline rae_String rae_string_borrow(rae_String s) {
  return (rae_String){s.data, s.len, 0, 0};
}

// String temp pool — used for statement-scope cleanup of heap
// allocations that the language semantically wants gone at the
// end of the statement (interpolation results, intermediate concat
// allocations created by compiler-emitted helpers).
//
// Codegen contract:
//   - emit_stmt wraps each potentially-temp-producing statement
//     with `int __m = rae_string_pool_mark(); ... rae_string_pool_flush(__m);`
//   - rae_ext_rae_str_interp registers its result before returning
//     (so the flush sweeps it up)
//   - let/assign/struct-init that captures a registered result
//     wraps with rae_string_pool_take(expr) to detach the entry
//     and keep ownership in the captured binding.
//
// Sized for ~hundreds of nested temps per statement; statements
// that genuinely exceed this still won't crash — extra registrations
// are silently dropped (which means they'll leak, the conservative
// failure mode).
void rae_string_pool_register(void* ptr);
int rae_string_pool_mark(void);
void rae_string_pool_flush(int saved);
void rae_string_pool_remove(void* ptr);
void rae_string_pool_release(int saved);
typedef struct { void** entries; int count; } RaePoolSave;
void rae_string_pool_detach(int saved, RaePoolSave* save);
void rae_string_pool_reattach(RaePoolSave* save);
int rae_string_pool_contains(void* ptr);
RAE_UNUSED static inline rae_String rae_string_pool_take(rae_String s) {
  rae_string_pool_remove(s.data);
  return s;
}

// Phase 2 struct-field deep-copy with move-when-safe optimization.
// When the source is an owned heap that is NOT in the temp pool
// (e.g. a function parameter received via caller-side pool_take, or
// a value already moved out of the pool), we can transfer ownership
// to the new struct field instead of allocating a fresh heap. The
// source is zeroed out so the original variable no longer aliases
// the heap.
//
// When the source IS in the pool, the caller's surrounding flush
// will free it — so we MUST deep-copy here to give the struct
// field a private heap that survives the flush.
//
// Borrowed/literal sources (is_owned=0) and NULL sources fall
// through to rae_string_copy, which handles them correctly.
RAE_UNUSED static inline rae_String rae_string_move_or_copy(rae_String* src) {
  if (src && src->is_owned && src->data &&
      !rae_string_pool_contains(src->data)) {
    rae_String moved = *src;
    src->data = NULL;
    src->len = 0;
    src->capacity = 0;
    src->is_owned = 0;
    return moved;
  }
  return rae_string_copy(src ? *src : (rae_String){NULL, 0, 0, 0});
}

// Re-register an owned String return value into the caller's pool.
// Used at the tail of String-returning function ret-epilogues: after
// `pool_take(__ret_val)` detaches the result from the callee pool and
// `pool_flush(__rae_spm_func)` sweeps callee-scope temps, this puts
// the surviving return value back into the pool so the *caller's*
// surrounding mark/flush can sweep it if the caller doesn't take
// ownership (typical for nested `concat(concat(a,b), c)` chains
// where the inner concat's heap would otherwise dangle).
//
// Guarded on is_owned + data so literal/borrowed/NULL returns don't
// pollute the pool. is_owned=0 inputs (literal-backed Strings, view
// returns) are correctly a no-op here.
RAE_UNUSED static inline rae_String rae_string_pool_register_owned(rae_String s) {
  if (s.is_owned && s.data) {
    rae_string_pool_register(s.data);
  }
  return s;
}

// N-way interpolation/concat helper. `n` is the number of
// rae_String parts; each is passed via varargs. Concatenates all
// parts into a single owned heap String, frees each part whose
// is_owned=1 (so compiler-emitted temps in the chain — e.g.
// `rae_text_int64` results — get cleaned up here), and
// registers the returned String with the temp pool for
// statement-scope cleanup.
rae_String rae_ext_rae_str_interp(int n, ...);

rae_String rae_ext_rae_str_concat(rae_String a, rae_String b);
rae_String rae_ext_rae_str_concat_cstr(rae_String a, rae_String b); // Legacy/helper name
rae_Bool rae_ext_rae_str_eq(rae_String a, rae_String b);
/* The String byte primitives lib/String.rae builds its algorithms on
 * (docs/runtime-c-audit.md row 3). Inline, so a Rae loop over bytes compiles
 * to plain loads; each is one access or one C-library call (memcmp, memchr),
 * range-checked so a wrong index can never read outside the String. */
static inline int64_t rae_ext_rae_str_len(rae_String s) {
  return s.len;
}
/* The byte at `index` (0..255), -1 out of range */
static inline int64_t rae_ext_rae_str_byte_at(rae_String s, int64_t index) {
  if (!s.data || index < 0 || index >= s.len) return -1;
  return (int64_t)s.data[index];
}
/* memcmp of a[aStart ..) and b[bStart ..) over `count` bytes: < 0, 0, > 0;
 * a range outside either String compares as different (2) */
static inline int64_t rae_ext_rae_str_compare_bytes(rae_String a, int64_t aStart, rae_String b, int64_t bStart, int64_t count) {
  if (count <= 0) return 0;
  if (aStart < 0 || bStart < 0 || aStart + count > a.len || bStart + count > b.len || !a.data || !b.data) return 2;
  return (int64_t)memcmp(a.data + aStart, b.data + bStart, (size_t)count);
}
/* memchr: the first index >= `from` holding `byte`, or -1 */
static inline int64_t rae_ext_rae_str_find_byte(rae_String s, int64_t byte, int64_t from) {
  if (!s.data || from < 0 || from >= s.len) return -1;
  const uint8_t* hit = (const uint8_t*)memchr(s.data + from, (int)byte, (size_t)(s.len - from));
  return hit ? (int64_t)(hit - s.data) : -1;
}
/* strtod of the number starting at `index` (lib/core/JsonScan.rae): the C
 * library's correctly rounded decimal-to-double. The number's characters are
 * copied out first, so a String that is not NUL-terminated is never overread. */
static inline double rae_ext_rae_str_f64_at(rae_String s, int64_t index) {
  char digits[64]; int count = 0;
  if (!s.data || index < 0) return 0.0;
  while (index + count < s.len && count < 63) {
    uint8_t byte = s.data[index + count];
    if (!((byte >= '0' && byte <= '9') || byte == '-' || byte == '+' || byte == '.' || byte == 'e' || byte == 'E')) break;
    digits[count++] = (char)byte;
  }
  digits[count] = 0;
  return strtod(digits, NULL);
}
/* `text` cut to its first `length` bytes in place, no allocation (lib/Io.rae
 * readLine drops the line ending with it): only a shorter length changes it,
 * and an owned buffer is NUL-terminated again at the new end. */
static inline void rae_ext_rae_str_shorten(rae_Mod_String text, int64_t length) {
  rae_String* target = text.ptr;
  if (!target || length < 0 || length >= target->len) return;
  target->len = length;
  if (target->data && target->is_owned) target->data[length] = 0;
}
rae_String rae_ext_rae_str_from_bytes(uint8_t* buffer, int64_t offset, int64_t count);
rae_String rae_ext_rae_str_from_small_bytes(int64_t first, int64_t second, int64_t third, int64_t fourth, int64_t count);
rae_String rae_ext_rae_str_from_packed(uint64_t first, uint64_t second, uint64_t third, int64_t count);
/* A Char32's code point (lib/core/Text.rae; the cast Rae has no `as` for) */
static inline int64_t rae_ext_rae_char_code(uint32_t value) {
  return (int64_t)value;
}
/* A code point as a Char32: the conversion Rae has no `as` for yet (lib/String
 * at); nothing but the cast */
static inline uint32_t rae_ext_rae_char_from_code(int64_t code) {
  return (uint32_t)code;
}
rae_String rae_ext_rae_str_sub(rae_String s, int64_t start, int64_t len);
/* A library-level runtime error (lib/core Core.rae runtimeError): one line on
 * stderr, exit RAE_TRAP_EXIT_CODE. runtime_filesystem.c. */
void rae_ext_rae_runtime_error(rae_String message);
void rae_ext_rae_runtime_warning(rae_String message);
__attribute__((cold, noinline)) void rae_list_set_out_of_range(int64_t index, int64_t length);
/* runtime_core_memory.c: per-thread alternate signal stack for the crash
 * handler; the emitted spawn thunk calls it first. No-op on WASM. */
void rae_thread_install_altstack(void);
double rae_ext_rae_str_to_f64(rae_String s);

rae_String rae_ext_rae_io_read_line_raw(void);
void rae_ext_rae_io_write_error(rae_String text);
rae_Char rae_ext_rae_io_read_char(void);

/* Diagnostics: dump cumulative alloc/free counters to stderr.
 * No-op unless RAE_MEM_STATS=1 was set in the environment. Used to
 * isolate which allocation class (string body, List/Map buffer, ...)
 * is leaking when residual RSS growth is small but linear. */
void rae_ext_rae_mem_stats_dump(void);
void rae_mem_stats_silence_exit_report(void);
int64_t rae_ext_rae_mem_stats_outstanding(void);
int64_t rae_ext_rae_mem_stats_unknown_frees(void); /* #761: untracked-free count */
int64_t rae_ext_rae_mem_stats_buf_outstanding(void);
int64_t rae_ext_rae_mem_stats_buf_outstanding_bytes(void);
/* Cumulative allocation count (strings + buffers), ALWAYS live — no env
 * var. Sample either side of a hot path and assert the delta to pin an
 * "allocates nothing" guarantee. */
int64_t rae_ext_rae_mem_alloc_total(void);

/* Bit intrinsics (lib/Math.rae popcount / leadingZeros / trailingZeros).
 * Defined here, static inline, so each call compiles to the one CPU
 * instruction instead of a call into the runtime: the physics bitsets and
 * id pools walk set bits with trailingZeros in their inner loops. An input of
 * 0 has a defined answer, the bit width (the builtins are undefined on 0). */
static inline int64_t rae_ext_rae_popcount(int64_t x) {
  return (int64_t)__builtin_popcountll((unsigned long long)x);
}
static inline int64_t rae_ext_rae_leading_zeros(int64_t x) {
  return x == 0 ? 64 : (int64_t)__builtin_clzll((unsigned long long)x);
}
static inline int64_t rae_ext_rae_trailing_zeros(int64_t x) {
  return x == 0 ? 64 : (int64_t)__builtin_ctzll((unsigned long long)x);
}
static inline int64_t rae_ext_Math_trailingZerosUInt64(uint64_t x) {
  return x == 0 ? 64 : (int64_t)__builtin_ctzll((unsigned long long)x);
}
static inline int64_t rae_ext_Math_trailingZerosUInt32(uint32_t x) {
  return x == 0 ? 32 : (int64_t)__builtin_ctz((unsigned int)x);
}
static inline int64_t rae_ext_Math_trailingZerosInt32(int32_t x) {
  return x == 0 ? 32 : (int64_t)__builtin_ctz((unsigned int)x);
}

/* Channel(T) MPSC cross-thread channel (#271) — see lib/channel.rae. */
int64_t rae_ext_rae_chan_new(int64_t elem_size);
void* rae_ext_rae_chan_ring(int64_t ch);
int64_t rae_ext_rae_chan_reserve(int64_t ch);
void rae_ext_rae_chan_commit(int64_t ch);
int64_t rae_ext_rae_chan_take(int64_t ch);
void rae_ext_rae_chan_release(int64_t ch);
int64_t rae_ext_rae_chan_count(int64_t ch);
int64_t rae_ext_rae_chan_received(int64_t ch);
void rae_ext_rae_chan_free(int64_t ch);

void rae_ext_rae_sys_exit(int64_t code);
rae_String rae_ext_rae_sys_get_env(rae_String name);
/* #995: program arguments. The emitted main calls rae_runtime_set_args(argc,
 * argv) first thing; the pair below exposes everything AFTER the executable. */
void rae_runtime_set_args(int argc, char** argv);
int64_t rae_ext_rae_sys_arg_count(void);
rae_String rae_ext_rae_sys_arg_at(int64_t index);
rae_String rae_ext_rae_sys_read_file(rae_String path);
/* Binary counterpart: a Buffer of one-byte-per-Int values, for container
 * formats whose content is not text. */
void* rae_ext_rae_sys_read_file_bytes(rae_String path, rae_Mod_Int64 out);
rae_String rae_ext_rae_sys_read_file_text(rae_String path, int64_t offset, int64_t len);
void* rae_ext_rae_sys_dir_open(rae_String folder);
rae_String rae_ext_rae_sys_dir_next(void* dir);
void rae_ext_rae_sys_dir_close(void* dir);
/* captureAndBlurRegion and loadCircleCroppedTexture are declared
 * in the raylib.h-scope helper block above (RAE_HAS_RAYLIB);
 * see implementations in rae_runtime.c. */
void rae_ext_disableAppNap(void);
int64_t rae_ext_thermalState(void);  /* #530: 0 nominal..3 critical (Apple), 0 else */
rae_Bool rae_ext_rae_sys_write_file(rae_String path, rae_String content);
rae_Bool rae_ext_rae_sys_rename(rae_String oldPath, rae_String newPath);
rae_Bool rae_ext_rae_sys_delete(rae_String path);
rae_Bool rae_ext_rae_sys_make_dir(rae_String path);
rae_Bool rae_ext_rae_sys_exists(rae_String path);
rae_Bool rae_ext_rae_sys_lock_file(rae_String path);
rae_Bool rae_ext_rae_sys_unlock_file(rae_String path);
double rae_ext_rae_sys_file_mtime(rae_String path);
/* OS file-change notifications (runtime_file_notify.c, lib/FileNotify.rae):
 * kqueue only, declared in Rae under `when hasKqueue` */
#ifdef RAE_HAS_KQUEUE
int64_t rae_ext_FileNotify_openForEvents(rae_String path);
int64_t rae_ext_FileNotify_watchHandle(int64_t queue, int64_t fd);
int64_t rae_ext_FileNotify_drain(int64_t queue);
int64_t rae_ext_FileNotify_wakeOnChange(int64_t queue);
int64_t rae_ext_FileNotify_takePending(int64_t queue);
#endif
/* The socket system-call shim (runtime_net.c, lib/net/NetSystem.rae): one
 * call per function, the result or -errno; the policy is lib/net/Tcp.rae. */
int64_t rae_ext_NetSys_constant(int64_t which);
int64_t rae_ext_NetSys_resolve(rae_String host, int64_t port, rae_Bool passive, uint8_t* records, int64_t maxRecords);
int64_t rae_ext_NetSys_socketFor(uint8_t* records, int64_t index);
int64_t rae_ext_NetSys_bindTo(int64_t fd, uint8_t* records, int64_t index);
int64_t rae_ext_NetSys_connectTo(int64_t fd, uint8_t* records, int64_t index);
int64_t rae_ext_NetSys_listen(int64_t fd, int64_t backlog);
int64_t rae_ext_NetSys_accept(int64_t fd);
int64_t rae_ext_NetSys_getFileFlags(int64_t fd);
int64_t rae_ext_NetSys_setFileFlags(int64_t fd, int64_t flags);
int64_t rae_ext_NetSys_setCloseOnExec(int64_t fd);
int64_t rae_ext_NetSys_setIntOption(int64_t fd, int64_t level, int64_t option, int64_t value);
int64_t rae_ext_NetSys_socketError(int64_t fd);
int64_t rae_ext_NetSys_receive(int64_t fd, uint8_t* buffer, int64_t offset, int64_t maxBytes);
int64_t rae_ext_NetSys_send(int64_t fd, uint8_t* buffer, int64_t offset, int64_t count, int64_t flags);
int64_t rae_ext_NetSys_sendText(int64_t fd, rae_String text, int64_t flags);
/* runtime_crypto_platform.c: CommonCrypto's SHA-1, Apple only (lib/crypto/Sha1.rae
 * declares it under `when hasCommonCrypto`) */
#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
int64_t rae_ext_Sha1_commonCrypto(uint8_t* bytes, int64_t offset, int64_t count, uint8_t* digest);
#endif
int64_t rae_ext_NetSys_pollOne(int64_t fd, int64_t events, int64_t timeoutMs);
#ifdef RAE_HAS_KQUEUE
/* The kqueue poller: declared in Rae under `when hasKqueue` */
int64_t rae_ext_NetSys_pollerCreate(void);
int64_t rae_ext_NetSys_pollerChange(int64_t queue, int64_t fd, int64_t filter, int64_t flags);
int64_t rae_ext_NetSys_pollerWait(int64_t queue, int64_t* events, int64_t maxEvents, int64_t timeoutMs);
#endif
int64_t rae_ext_NetSys_close(int64_t fd);
int64_t rae_ext_NetSys_localPort(int64_t fd);
rae_String rae_ext_NetSys_bytesToText(uint8_t* buffer, int64_t offset, int64_t count);
int64_t rae_ext_rae_sys_rss_kb(void);

/* Audio — SFX + looping ambient over the SDL3 backend (runtime_audio_sdl3.c, #46). */
void    rae_ext_audio_init(void);
int64_t rae_ext_audio_load_wav(rae_String path);
void    rae_ext_audio_play(int64_t clip, float volume);
int64_t rae_ext_audio_loop(int64_t clip, float volume);
void    rae_ext_audio_tick(void);
void    rae_ext_audio_set_muted(int64_t muted);

rae_String rae_ext_rae_str_f64(double v);
rae_String rae_ext_json_number(float v);
rae_String rae_ext_rae_str_f64_ptr(const double* v);
rae_String rae_ext_rae_str_string(rae_String s);
rae_String rae_ext_rae_str_string_ptr(const rae_String* s);
rae_String rae_ext_rae_str_cstr(const char* s); // Legacy/helper

/* JSON helpers */
// #761: defined in runtime_buffers_math.c so
// it can mem-tag + pool-register its result like every other owned-String
// producer (rae_ext_rae_str_interp etc.). Without that, a toJson result bound to
// a local (`let s = obj.toJson()`) was freed as an UNTRACKED heap (mem-stats
// outstanding=-1), while inlined use silently LEAKED it (untagged, invisible).
rae_String rae_json_build(const char* s, int64_t len);
/* The text a generated toJson writes: 512 bytes on the stack, moved to a
 * doubling heap block when they run out, so a JSON text of any length fits.
 * rae_jout_finish hands the text over as an owned String. */
typedef struct { char* data; int64_t len; int64_t cap; char local[512]; } rae_JsonOut;
static inline void rae_jout_init(rae_JsonOut* out) {
  out->data = out->local; out->len = 0; out->cap = (int64_t)sizeof out->local; out->local[0] = 0;
}
static inline int rae_jout_reserve(rae_JsonOut* out, int64_t extra) {
  int64_t needed = out->len + extra + 1;
  if (needed <= out->cap) return 1;
  int64_t cap = out->cap * 2;
  while (cap < needed) cap *= 2;
  char* data = (char*)malloc((size_t)cap);
  if (!data) return 0;
  memcpy(data, out->data, (size_t)out->len + 1);
  if (out->data != out->local) free(out->data);
  out->data = data; out->cap = cap;
  return 1;
}
/* snprintf straight into the output (a macro, so each call is the plain
 * snprintf the old fixed buffer made); only a text that does not fit grows
 * the output and is formatted again. The arguments are evaluated twice then,
 * so the generated code passes only plain values. */
#define rae_jout_printf(out, ...) do { \
    rae_JsonOut* rae_jout_target = (out); \
    int rae_jout_count = snprintf(rae_jout_target->data + rae_jout_target->len, \
        (size_t)(rae_jout_target->cap - rae_jout_target->len), __VA_ARGS__); \
    if (rae_jout_count >= 0 && rae_jout_target->len + rae_jout_count + 1 > rae_jout_target->cap) { \
      if (!rae_jout_reserve(rae_jout_target, rae_jout_count)) break; \
      snprintf(rae_jout_target->data + rae_jout_target->len, \
        (size_t)(rae_jout_target->cap - rae_jout_target->len), __VA_ARGS__); \
    } \
    if (rae_jout_count >= 0) rae_jout_target->len += rae_jout_count; \
  } while (0)
/* Bytes copied in as they are: the keys, separators and String values of a
 * toJson, which need no formatting (an snprintf per piece made toJson of a
 * record of Strings 1.2x slower than the old fixed-buffer writer) */
static inline void rae_jout_put(rae_JsonOut* out, const char* data, int64_t len) {
  if (len <= 0 || !rae_jout_reserve(out, len)) return;
  memcpy(out->data + out->len, data, (size_t)len);
  out->len += len;
  out->data[out->len] = 0;
}
#define rae_jout_text(out, literal) rae_jout_put((out), (literal), (int64_t)(sizeof(literal) - 1))
static inline rae_String rae_jout_finish(rae_JsonOut* out) {
  rae_String text = rae_json_build(out->data, out->len);
  if (out->data != out->local) free(out->data);
  return text;
}
rae_String rae_ext_rae_str_cstr_ptr(const char** s); // Legacy/helper

int64_t rae_ext_nextTick(void);
int64_t rae_ext_nowMs(void);

/* Cooked Hosek-Wilkie coefficients, pushed from Rae. index 0..35, laid out
 * three vec4 per channel. Two entry points, ONE table (runtime_sky_state.h):
 * the deferred sky is bound through lib/gbuffer.rae and the forward one
 * through lib/gpu3d.rae, and a Rae extern's C name follows its module. */
void rae_ext_Gbuffer_skyHosekPush(int64_t index, float value);
void rae_ext_Gpu3d_skyHosekPush(int64_t index, float value);
/* The forward background. Called between gpu3d.begin and the geometry; takes
 * no camera because the frame's own is used. */
void rae_ext_Gpu3d_skyDraw(float skyKind, float turbidity, float skyExposure, float sunSizeRad,
                           float sunX, float sunY, float sunZ,
                           float sunR, float sunG, float sunB,
                           float zenR, float zenG, float zenB, float bands,
                           float horR, float horG, float horB, float discI,
                           float clearR, float clearG, float clearB);
int64_t rae_ext_nowNs(void);
void rae_ext_rae_sleep(int64_t ms);
int64_t rae_ext_Time_sleepNs(int64_t ns);  /* one nanosleep (lib/Time.rae waitUntil) */

/* lib/Math.rae's scalar functions over `Float`, which is f32 (see
 * docs/primitive-types.md): the C side takes and returns `float` and uses the
 * f-suffixed libm entry points. Computing in float rather than round-tripping
 * through double keeps the CPU result in the representation the GPU uses.
 * Higher-precision variants belong on explicit Float64 APIs.
 *
 * The EXACTLY specified ones are static inline here, so a call in generated
 * code inlines — sqrtf, floorf, ceilf and roundf become single instructions —
 * instead of crossing into the separately compiled runtime
 * (docs/float4-design.md §6.4: the solver's clamps and vector lengths). sqrtf
 * is correctly rounded IEEE and the rest are exact, so a C compiler folding a
 * constant argument gets the same bits as the instruction.
 *
 * The transcendental ones (sin ... log, pow) stay out of line in the runtime
 * object on purpose: libm does not round them correctly, so a compiler that
 * folded `sinf(constant)` with its own implementation (GCC uses MPFR) could
 * produce different bits from the runtime's call. */
float rae_ext_Math_sin(float x);
float rae_ext_Math_cos(float x);
float rae_ext_Math_tan(float x);
float rae_ext_Math_asin(float x);
float rae_ext_Math_acos(float x);
float rae_ext_Math_atan(float x);
float rae_ext_Math_atan2(float y, float x);
float rae_ext_Math_pow(float base, float exp);
float rae_ext_Math_exp(float x);
float rae_ext_Math_math_log(float x);
static inline float rae_ext_Math_sqrt(float x) { return sqrtf(x); }
/* IEEE remainder: x - n*y with n the nearest integer to x/y. Exact (no
 * rounding), so deterministic on every platform — Box3D's b3UnwindAngle
 * relies on it. */
static inline float rae_ext_Math_remainder(float x, float y) { return remainderf(x, y); }
static inline float rae_ext_Math_floor(float x) { return floorf(x); }
static inline float rae_ext_Math_ceil(float x) { return ceilf(x); }
static inline float rae_ext_Math_round(float x) { return roundf(x); }
/* The Float whose IEEE-754 bits are the low 32 bits of `bits` (lib/Math
 * floatFromBits), the inverse of rae_ext_rae_f32_bits. */
static inline float rae_ext_Math_floatFromBits(int64_t bits) {
  uint32_t low = (uint32_t)bits; float x; memcpy(&x, &low, sizeof x); return x;
}



RAE_UNUSED static RaeAny rae_any_unwrap(RaeAny v) {
    v.is_view = false;
    v.is_mod = false;
    return v;
}

RAE_UNUSED static void rae_any_drop(RaeAny* v) {
    if (!v) return;
    if (v->is_view || v->is_mod) {
        *v = rae_any_none();
        return;
    }
    switch (v->type) {
        case RAE_TYPE_STRING:
            rae_string_drop(&v->as.s);
            break;
        case RAE_TYPE_BUFFER:
        case RAE_TYPE_LIST:
        case RAE_TYPE_ANY:
            if (v->as.ptr) {
                if (v->drop) v->drop(v->as.ptr);
                free(v->as.ptr);
            }
            break;
        default:
            break;
    }
    *v = rae_any_none();
}

/* Representation-aware copy of a RaeAny (the C shape of `opt T`).
 * Used by synthesised struct deep-copies for `opt` fields — the
 * base-type-only classifier used to emit rae_string_copy on the
 * RaeAny representation (#138). String payloads get a private heap;
 * pointer payloads (BUFFER/LIST/ANY) have no generic deep-copy, so
 * they're shared as a borrow (is_view=1) — rae_any_drop then no-ops
 * on the copy instead of double-freeing the shared payload. */
RAE_UNUSED static inline RaeAny rae_any_copy(RaeAny v) {
    if (v.is_view || v.is_mod) return v;
    if (v.type == RAE_TYPE_STRING) {
        v.as.s = rae_string_copy(v.as.s);
        return v;
    }
    if (v.type == RAE_TYPE_BUFFER || v.type == RAE_TYPE_LIST ||
        v.type == RAE_TYPE_ANY) {
        v.is_view = true;
        return v;
    }
    return v;
}

/* Crypto function declarations */
void rae_ext_rae_crypto_lock(RaeAny key, RaeAny nonce, RaeAny plain, int64_t plain_len, RaeAny mac, RaeAny cipher);
int64_t rae_ext_rae_crypto_unlock(RaeAny key, RaeAny nonce, RaeAny mac, RaeAny cipher, int64_t cipher_len, RaeAny plain);
void rae_ext_rae_crypto_argon2i(rae_String password, rae_String salt, int64_t nb_blocks, int64_t nb_iterations, RaeAny hash_buf, int64_t hash_len);

/* Raylib wrapper function declarations */
#ifdef RAE_HAS_RAYLIB
#include <raylib.h>
void rae_ext_initWindow(int64_t width, int64_t height, rae_String title);
void rae_ext_setConfigFlags(int64_t flags);
/* Streaming textures for CPU-rendered images. loadStreamTexture creates a
 * blank RGBA8 texture; updateStreamTexture uploads `count` packed 0xRRGGBB
 * Int pixels (expanded to opaque RGBA8). `pixels` is a Buffer(Int) (void*). */
Texture rae_ext_loadStreamTexture(int64_t width, int64_t height);
void rae_ext_updateStreamTexture(Texture texture, const int64_t* pixels, int64_t count);
rae_Bool rae_ext_windowShouldClose(void);
void rae_ext_closeWindow(void);
void rae_ext_setTargetFPS(int64_t fps);
/* GLFW wait-events bindings. Block the thread until an OS event arrives
 * or the timeout (in seconds) elapses; the negative-timeout case is
 * an unbounded wait -- prefer waitEvents() for that intent.
 * postEmptyEvent() wakes any thread currently blocked in a wait.
 * Must be called after initWindow(). */
void rae_ext_waitEventsTimeout(float seconds);
void rae_ext_waitEvents(void);
void rae_ext_postEmptyEvent(void);
void rae_ext_beginDrawing(void);
void rae_ext_endDrawing(void);
void rae_ext_clearBackground(Color color);
rae_Bool rae_ext_isKeyDown(int64_t key);
rae_Bool rae_ext_isKeyPressed(int64_t key);
int64_t rae_ext_getMouseX(void);
int64_t rae_ext_getMouseY(void);
rae_Bool rae_ext_isMouseButtonDown(int64_t button);
rae_Bool rae_ext_isMouseButtonPressed(int64_t button);
rae_Bool rae_ext_isMouseButtonReleased(int64_t button);
int64_t rae_ext_getScreenWidth(void);
int64_t rae_ext_getScreenHeight(void);
int64_t rae_ext_getCurrentMonitor(void);
int64_t rae_ext_getMonitorWidth(int64_t monitor);
int64_t rae_ext_getMonitorHeight(int64_t monitor);
void rae_ext_setWindowSize(int64_t width, int64_t height);
void rae_ext_setWindowPosition(int64_t x, int64_t y);
int64_t rae_ext_getWindowPositionX(void);
int64_t rae_ext_getWindowPositionY(void);
double rae_ext_getTime(void);
void rae_ext_drawCircle(float x, float y, float radius, Color color);
void rae_ext_drawCircleGradient(int64_t x, int64_t y, float radius, Color color1, Color color2);
void rae_ext_drawRectangle(float x, float y, float width, float height, Color color);
void rae_ext_drawRectangleLines(float x, float y, float width, float height, Color color);
void rae_ext_drawRectangleRounded(float x, float y, float width, float height, float roundness, int64_t segments, Color color);
void rae_ext_drawRectangleGradientV(int64_t x, int64_t y, int64_t width, int64_t height, Color color1, Color color2);
void rae_ext_drawRectangleGradientH(int64_t x, int64_t y, int64_t width, int64_t height, Color color1, Color color2);
void rae_ext_drawText(rae_String text, float x, float y, float fontSize, Color color);
void rae_ext_drawSphere(Vector3 centerPos, float radius, Color color);
void rae_ext_drawCube(Vector3 pos, float width, float height, float length, Color color);
void rae_ext_drawCubeWires(Vector3 pos, float width, float height, float length, Color color);
void rae_ext_drawCylinder(Vector3 position, float radiusTop, float radiusBottom, float height, int64_t slices, Color color);
void rae_ext_drawGrid(int64_t slices, float spacing);
void rae_ext_beginMode3D(Camera3D camera);
void rae_ext_endMode3D(void);
void rae_ext_beginMode2D(Camera2D camera);
void rae_ext_endMode2D(void);
Color rae_ext_colorFromHSV(float hue, float saturation, float value);
void rae_ext_takeScreenshot(rae_String fileName);
Texture rae_ext_loadTexture(rae_String fileName);
void rae_ext_unloadTexture(Texture texture);
void rae_ext_drawTexture(Texture texture, float x, float y, Color tint);
void rae_ext_drawTextureEx(Texture texture, Vector2 pos, float rotation, float scale, Color tint);
int64_t rae_ext_measureText(rae_String text, int64_t fontSize);
void rae_ext_loadFontInto(int64_t slot, rae_String path, int64_t fontSize);
void rae_ext_unloadFontSlot(int64_t slot);
rae_Bool rae_ext_isFontSlotLoaded(int64_t slot);
void rae_ext_drawTextWithFont(int64_t slot, rae_String text, float x, float y, float fontSize, float spacing, Color color);
#endif

/* Integers formatted by their RAE type (the compiler picks rae_str_<width> from
 * the type name, c_expr.c rae_int_formatter): the _Generic rae_ext_rae_str
 * below sees only C types, and there UInt32 is Char's uint32_t (it printed as
 * a code point) and Int8 is Bool's int8_t where bool is missing (it printed as
 * true/false). Each takes the value or a pointer to it (how a view or mod
 * binding can reach the formatter). The <NAME>_value / _pointer functions
 * are emitted into every program with the rae_text_* wrappers (c_backend.c
 * emit_text_wrappers), so they format through lib/core/Text.rae. */
#define RAE_INT_STR_DISPATCH(NAME, CTYPE, X) _Generic((X), \
    CTYPE*: NAME##_pointer, const CTYPE*: NAME##_pointer, default: NAME##_value)(X)
#define rae_str_int8(X) RAE_INT_STR_DISPATCH(rae_str_int8, int8_t, X)
#define rae_str_int16(X) RAE_INT_STR_DISPATCH(rae_str_int16, int16_t, X)
#define rae_str_int32(X) RAE_INT_STR_DISPATCH(rae_str_int32, int32_t, X)
#define rae_str_uint8(X) RAE_INT_STR_DISPATCH(rae_str_uint8, uint8_t, X)
#define rae_str_uint16(X) RAE_INT_STR_DISPATCH(rae_str_uint16, uint16_t, X)
#define rae_str_uint32(X) RAE_INT_STR_DISPATCH(rae_str_uint32, uint32_t, X)
#define rae_str_uint64(X) RAE_INT_STR_DISPATCH(rae_str_uint64, uint64_t, X)

/* Text of an interpolated / toString'd value, by its C type. The integer,
 * Bool and Char entries are the `rae_text_*` wrappers the code generator
 * emits into every program (they call lib/core/Text.rae; docs/runtime-c-audit.md
 * row 4), and `rae_text_any` formats an Any through lib/core/AnyText.rae (row
 * 12); floats and Strings stay C. */
#define rae_ext_rae_str(X) _Generic((X), \
    int64_t: rae_text_int64, \
    int64_t*: rae_text_int64_ptr, \
    const int64_t*: rae_text_int64_ptr, \
    double: rae_ext_rae_str_f64, \
    double*: rae_ext_rae_str_f64_ptr, \
    const double*: rae_ext_rae_str_f64_ptr, \
    float: rae_ext_rae_str_f64, \
    bool: rae_text_bool, \
    int8_t: rae_text_bool, \
    rae_String: rae_ext_rae_str_string, \
    rae_String*: rae_ext_rae_str_string_ptr, \
    uint32_t: rae_text_char, \
    uint32_t*: rae_text_char_ptr, \
    unsigned char: rae_text_int64, \
    int16_t: rae_text_int64, \
    uint16_t: rae_text_int64, \
    int32_t: rae_text_int64, \
    uint64_t: rae_text_uint64, \
    RaeAny: rae_text_any, \
    default: rae_ext_rae_str_string \
)(X)

#ifdef RAE_HAS_WEBGPU
/* WebGPU context bootstrap accessors (#501). Bound from Rae via extern("…") in
 * lib/webgpu/context.rae. Declared here (not by a WebGPU header) so the
 * generated C sees real prototypes — otherwise an implicit declaration would
 * assume `int` return and truncate the returned 64-bit handle pointers. */
int   rae_wgpu_ctx_bootstrap(void);
void* rae_wgpu_ctx_device(void);
void* rae_wgpu_ctx_queue(void);
void* rae_wgpu_ctx_adapter(void);
void* rae_wgpu_ctx_instance(void);
void  rae_wgpu_ctx_poll(int wait);
void  rae_wgpu_watch_submission(uint64_t index);      /* #938 */
uint32_t rae_wgpu_submission_done(uint64_t index);   /* #938 */
void* rae_wgpu_null_ptr(void);
/* #528 GPU timestamp timing */
int   rae_wgpu_have_timestamp(void);
int   rae_wgpu_have_rg11b10(void);
void  rae_wgpu_report(const char* tag);   /* live wgpu object counts */
void  rae_wgpu_report_periodic(void);     /* every 120 presented frames; RAE_WGPU_REPORT=0 silences */
int   rae_g2d_window_visible(void);   /* 0 while window hidden/minimized/occluded (gpu2d.windowVisible) */
int   rae_wgpu_map_read(void* buffer, uint64_t size, void* dst);
/* Owned asynchronous map requests; serialized on the WebGPU context thread. */
void* rae_wgpu_read_start(void* buffer, uint64_t offset, uint64_t size);
int   rae_wgpu_read_start_status(void* request);
int   rae_wgpu_read_poll(void* request);
int   rae_wgpu_read_copy(void* request, void* destination, uint64_t capacity);
void* rae_wgpu_read_release(void* request);
#endif

/* G-buffer instanced-draw context accessors (#502). Bound from Rae via
 * extern("…") in lib/gbuffer.rae (drawRecords now runs in Rae over the WebGPU
 * bindings). Declared here — not by a WebGPU header — so the generated C sees
 * real prototypes and does not truncate the 64-bit handle pointers. Unguarded:
 * the stub build (no real geometry pass) provides no-op versions too. */
void  rae_gb_bump_terrain_tex_gen(void);
int64_t rae_gb_terrain_tex_gen(void);
void* rae_gb_terrain_array_view(void);   /* #912: the C material array's 2d-array view, adopted by Rae */
void rae_gb_terrain_array_init(int64_t w, int64_t h, int64_t layers);
void rae_gb_terrain_array_write(int64_t layer, const int64_t* pixels, int64_t w, int64_t h);
void* rae_gb_sprite_array_view(void);
int64_t rae_gb_sprite_tex_gen(void);
void rae_gb_sprite_array_init(int64_t w, int64_t h, int64_t layers);
void rae_gb_sprite_array_write(int64_t layer, const int64_t* pixels, int64_t w, int64_t h);
/* G-buffer metaball clusters are Rae manager objects (#922); C keeps the WGSL. */
/* #921: the skinned meshes + palette are the Rae SkinStore's (no rae_gb_skin_* accessors). */
/* One-command-buffer submit for the raw-encoder passes (shadow, fullscreen,
 * transparent, the legacy GpuTiming): a general FFI gap, not renderer state. */
void rae_gb_submit(void* cmd);
/* #906/#923: the whole deferred frame (G-buffer targets, frame uniform, draws,
 * a Recording per frame, the post-pass targets / uniforms / pipelines) is Rae
 * manager state; C keeps the WGSL sources, the offscreen size and the
 * render scale. rae_Mat4 is forward-declared for the remaining Mat4 externs. */
struct rae_Mat4;
int64_t rae_gb_prepare(void);
int64_t rae_gb_offscreen_w(void);
int64_t rae_gb_offscreen_h(void);
/* Dynamic resolution (#530). */
void   rae_gb_set_render_scale(double s);
double rae_gb_render_scale(void);
/* Render pipelines + WGSL shader modules created in Rae (#503). */
/* G-buffer inspector built in Rae (#503). */
int64_t rae_g2d_format(void);
/* #920: the presentable target is the Rae canvas's texture; C reports the
 * configured surface and borrows the texture for present / readback. */
int64_t rae_g2d_surface_ready(void);
int64_t rae_g2d_surface_width(void);
int64_t rae_g2d_surface_height(void);
/* Deferred passes migrated to Rae (#504): composite first. */
int64_t rae_gb_deferred_prepare(void);
void* rae_gb_composite_source_view(void);
int64_t rae_gb_composite_source_index(void);
/* SSAO pass in Rae (#504). */
void rae_gb_ssao_upload(float camX, float camY, float camZ);
void* rae_gb_ao_view(void);
void* rae_gb_light_ubuf(void);
int64_t rae_gb_light_bytes(void);
/* Lighting pass in Rae (#504). */
void rae_gb_light_upload(float camX, float camY, float camZ, float exposure,
                         float sunX, float sunY, float sunZ,
                         float sunR, float sunG, float sunB,
                         float skyR, float skyG, float skyB,
                         float gndR, float gndG, float gndB,
                         float skyKind, float turbidity, float skyExposure,
                         float sunSizeRad, float zenR, float zenG, float zenB,
                         float bands, float horR, float horG, float horB, float discI);
void* rae_gb_light_pipeline(void);
void* rae_gb_lit_view(void);
void* rae_gb_lit_texture(void);
void* rae_gb_lit_copy_texture(void);
void* rae_gb_lit_copy_view(void);
int64_t rae_gb_lit_format(void);
void rae_ext_Gbuffer_skyHosekPush(int64_t index, float value);
/* TAA pass in Rae (#504). */
int64_t rae_gb_taa_ready(void);
int64_t rae_gb_taa_begin(void);
void rae_gb_taa_end(void);
void* rae_gb_taa_pipeline(void);
void* rae_gb_taa_ubuf(void);
void* rae_gb_taa_target_view(void);
void* rae_gb_taa_history_view(void);
int64_t rae_gb_taa_is_enabled(void);
void rae_gb_set_taa_enabled(int64_t e);
void rae_gb_set_fog(int64_t on, float r, float g, float b, float start, float end);
/* Depth-pyramid build in Rae (#504). */
int64_t rae_gb_pyramid_ready(void);
void* rae_gb_pyr_from_depth_pipeline(void);
void* rae_gb_pyr_reduce_pipeline(void);
void* rae_gb_pyr_src_view(int64_t i);
void* rae_gb_pyr_rt_view(int64_t i);
/* Shadow cascades (#925): the ShadowCache (lib/ShadowMaps.rae) owns the targets,
 * pipelines, queue and passes; C keeps the WGSL and receives the three sampled
 * inputs BORROWED for the forward scene / skin binds. */
void rae_g3d_set_shadow_inputs(void* frame_ubuf, void* array_view, void* sampler);
/* gpu2d frame lifecycle in Rae (#504). */
void rae_g2d_frame_reset(void);
void rae_g2d_present(void* texture, int64_t width, int64_t height);
void rae_g2d_tick(void);
/* #915: the design->physical transform (8 floats = 2*vec4) the canvas writes
 * into its viewport uniform each flush; batches, clips and scissor are Rae. */
void rae_g2d_xform(float* out);
/* #908: CPU image decode for the Rae image pass (textures + upload are manager objects). */
void* rae_g2d_decode_image(rae_String path);
int64_t rae_g2d_decoded_width(void);
int64_t rae_g2d_decoded_height(void);
void rae_g2d_decode_free(void* rgba);
/* #909/#915: the text pass + glyph batch are Rae; C keeps the CPU atlas pixels. */
int64_t rae_g2d_text_atlas_max(void);
void* rae_sdf_atlas_pixels(int64_t handle);
int64_t rae_sdf_atlas_width(int64_t handle);
int64_t rae_sdf_atlas_height(int64_t handle);
/* Procedural texture registration (#539): upload RGBA pixels generated in Rae. */
/* Forward renderer (#514): frame prepare + handle accessors for the Rae-side
 * forward render pass (gpu3d.beginForward). Mirrors the rae_gb_* deferred set. */
int   rae_g3d_frame_prepare(const float* frame, int64_t count);
void* rae_g3d_hdr_view(void);
void* rae_g3d_normal_view(void);
void* rae_g3d_velocity_view(void);
void* rae_g3d_ambient_view(void);
void* rae_g3d_depth_view(void);
void* rae_g3d_pipeline(void);
void* rae_g3d_bind(void);
void  rae_g3d_set_frame(void* enc, void* pass);
void* rae_g3d_pass(void);
void* rae_g3d_encoder(void);
int64_t rae_g3d_frame_active(void);
void  rae_g3d_clear_frame(void);
void  rae_g3d_submit_cmd(void* cmd);
int   rae_g3d_tonemap_prepare(void);
void* rae_g3d_tonemap_pipeline(void);
void* rae_g3d_tonemap_bind(int64_t slot);
int64_t rae_g3d_tonemap_pending(void);
void  rae_g3d_present_frame(void* texture, int64_t width, int64_t height);
int   rae_g3d_taa_prepare(void);
void* rae_g3d_taa_pipeline(void);
void* rae_g3d_taa_bind(int64_t slot);
void* rae_g3d_taa_view(int64_t slot);
int   rae_g3d_ssao_prepare(void);
int64_t rae_g3d_ssao_enabled(void);
int64_t rae_g3d_ao_debug_on(void);
void* rae_g3d_ssao_pipeline(void);
void* rae_g3d_ssao_bind(void);
void* rae_g3d_ao_view(void);
void* rae_g3d_ao_apply_pipeline(void);
void* rae_g3d_ao_apply_bind(void);
void* rae_g3d_ao_debug_pipeline(void);
void* rae_g3d_ao_debug_bind(void);
void  rae_g3d_clear_ao(void);
int   rae_g3d_sky_prepare(float skyKind, float turbidity, float skyExposure, float sunSizeRad,
                          float sunX, float sunY, float sunZ,
                          float sunR, float sunG, float sunB,
                          float zenR, float zenG, float zenB, float bands,
                          float horR, float horG, float horB, float discI,
                          float clearR, float clearG, float clearB);
void* rae_g3d_sky_pipeline(void);
void* rae_g3d_sky_bind(void);
int   rae_g3d_push_draw_record(int64_t mesh, const struct rae_Mat4* model, const struct rae_Mat4* prevModel,
                               float r, float g, float b, float metallic,
                               float emR, float emG, float emB, float roughness);
int   rae_g3d_push_metaball_cluster(const float* packedBalls, int64_t count,
                                    const float* packedColors, float smoothing,
                                    float metallic, float roughness,
                                    float emR, float emG, float emB);
void* rae_g3d_sdf_pipeline(void);
void* rae_g3d_sdf_bind(int64_t slot);
int   rae_g3d_push_skinned_draw(int64_t mesh, const struct rae_Mat4* model,
                                float r, float g, float b, float metallic, float roughness,
                                void* palette);   /* #921: the SkinStore palette, borrowed */
void* rae_g3d_skin_pipeline(void);
void* rae_g3d_skin_bind(void);

#endif
