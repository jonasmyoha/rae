/* runtime_atomics.h — the one-call shims under lib/Atomic.rae and lib/Signal.rae
 * (docs/realtime-language-design.md §6.4, §6.5).
 *
 * Platform reason (AGENTS.md "Runtime C rule"): an atomic operation is a CPU
 * instruction with a memory order that Rae cannot spell, and a semaphore is an
 * OS object. Each function below makes exactly one such call and holds no
 * policy; the meaning (an Atomic(T), a Signal, a ring) is Rae.
 *
 * Atomics work on one 8-byte cell inside a Rae Buffer(Int) (so the leak check
 * sees a forgotten free): `cell` is the buffer, `index` the element. Loads are
 * acquire, stores release, read-modify-writes acquire-release; the `relaxed`
 * forms are for counters that order nothing. All of them are lock-free on
 * every target Rae builds for (64-bit, and wasm32 with 64-bit atomics). */
#ifndef RAE_RUNTIME_ATOMICS_H
#define RAE_RUNTIME_ATOMICS_H

#include <stdint.h>

#define RAE_ATOMIC_CELL(cell, index) (((int64_t*)(cell)) + (index))

RAE_UNUSED static inline int64_t rae_ext_rae_atomic_load(void* cell, int64_t index) {
  return __atomic_load_n(RAE_ATOMIC_CELL(cell, index), __ATOMIC_ACQUIRE);
}
RAE_UNUSED static inline int64_t rae_ext_rae_atomic_load_relaxed(void* cell, int64_t index) {
  return __atomic_load_n(RAE_ATOMIC_CELL(cell, index), __ATOMIC_RELAXED);
}
RAE_UNUSED static inline void rae_ext_rae_atomic_store(void* cell, int64_t index, int64_t value) {
  __atomic_store_n(RAE_ATOMIC_CELL(cell, index), value, __ATOMIC_RELEASE);
}
RAE_UNUSED static inline void rae_ext_rae_atomic_store_relaxed(void* cell, int64_t index, int64_t value) {
  __atomic_store_n(RAE_ATOMIC_CELL(cell, index), value, __ATOMIC_RELAXED);
}
RAE_UNUSED static inline int64_t rae_ext_rae_atomic_exchange(void* cell, int64_t index, int64_t value) {
  return __atomic_exchange_n(RAE_ATOMIC_CELL(cell, index), value, __ATOMIC_ACQ_REL);
}
/* Writes `desired` if the cell holds `expected`; answers the value it held
 * before (equal to `expected` exactly when the write happened). */
RAE_UNUSED static inline int64_t rae_ext_rae_atomic_compare_exchange(void* cell, int64_t index,
                                                                     int64_t expected, int64_t desired) {
  __atomic_compare_exchange_n(RAE_ATOMIC_CELL(cell, index), &expected, desired, 0,
                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  return expected;
}
RAE_UNUSED static inline int64_t rae_ext_rae_atomic_fetch_add(void* cell, int64_t index, int64_t amount) {
  return __atomic_fetch_add(RAE_ATOMIC_CELL(cell, index), amount, __ATOMIC_ACQ_REL);
}
RAE_UNUSED static inline int64_t rae_ext_rae_atomic_fetch_add_relaxed(void* cell, int64_t index,
                                                                      int64_t amount) {
  return __atomic_fetch_add(RAE_ATOMIC_CELL(cell, index), amount, __ATOMIC_RELAXED);
}

/* Signal (runtime_signal.c): a counting semaphore. post never waits. */
int64_t rae_ext_rae_signal_new(void);
void rae_ext_rae_signal_post(int64_t signal) RAE_NONBLOCKING;
void rae_ext_rae_signal_wait(int64_t signal) RAE_NONALLOCATING;
rae_Bool rae_ext_rae_signal_try_wait(int64_t signal) RAE_NONBLOCKING;
void rae_ext_rae_signal_free(int64_t signal);

/* The real-time check and mark (runtime_realtime.c, lib/Realtime.rae). */
void rae_ext_Realtime_markThread(rae_Bool realtime) RAE_NONBLOCKING;
rae_Bool rae_ext_Realtime_threadIsMarked(void) RAE_NONBLOCKING;
/* Real-time priority for a `spawn thread` (lib/Realtime promoteToRealtime,
 * runtime_realtime.c): one scheduler call each, per platform */
#if defined(__APPLE__)
int64_t rae_ext_Realtime_timebaseNumerator(void);
int64_t rae_ext_Realtime_timebaseDenominator(void);
int64_t rae_ext_Realtime_setTimeConstraint(int64_t periodTicks, int64_t computationTicks, int64_t constraintTicks);
int64_t rae_ext_Realtime_setStandardPolicy(void);
int64_t rae_ext_Realtime_workgroupTokenSize(void);
int64_t rae_ext_Realtime_workgroupJoin(int64_t workgroup, uint8_t* token);
void rae_ext_Realtime_workgroupLeave(int64_t workgroup, uint8_t* token);
#elif defined(_WIN32)
int64_t rae_ext_Realtime_mmcssBegin(void);
int64_t rae_ext_Realtime_mmcssEnd(int64_t handle);
#elif defined(__ANDROID__)
int64_t rae_ext_Realtime_setNice(int64_t value);
#elif defined(__linux__)
int64_t rae_ext_Realtime_setFifo(int64_t priority);
int64_t rae_ext_Realtime_setOther(void);
#endif
rae_Bool rae_ext_Realtime_reportWanted(void);

#endif
