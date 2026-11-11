/* runtime_signal.c — the semaphore under lib/Signal.rae
 * (docs/realtime-language-design.md §6.5).
 *
 * Platform reason: a semaphore is an OS object. Each function makes one call:
 *   Apple:  dispatch_semaphore (macOS has no unnamed POSIX semaphores);
 *   other POSIX (Linux, Android): sem_t, which glibc and bionic build on a futex;
 *   the browser without threads: no other thread can post while one waits, so
 *     a counter stands in and `wait` takes a post or returns at once.
 * post never waits, so it may be called from a real-time thread. */
#if defined(__APPLE__)
#include <dispatch/dispatch.h>

int64_t rae_ext_rae_signal_new(void) {
  return (int64_t)(intptr_t)dispatch_semaphore_create(0);
}
void rae_ext_rae_signal_post(int64_t signal) {
  dispatch_semaphore_signal((dispatch_semaphore_t)(intptr_t)signal);
}
void rae_ext_rae_signal_wait(int64_t signal) {
  dispatch_semaphore_wait((dispatch_semaphore_t)(intptr_t)signal, DISPATCH_TIME_FOREVER);
}
rae_Bool rae_ext_rae_signal_try_wait(int64_t signal) {
  return dispatch_semaphore_wait((dispatch_semaphore_t)(intptr_t)signal, DISPATCH_TIME_NOW) == 0;
}
void rae_ext_rae_signal_free(int64_t signal) {
  dispatch_release((dispatch_semaphore_t)(intptr_t)signal);
}

#elif !defined(__wasm__) || defined(RAE_WASM_THREADS)
#include <semaphore.h>

int64_t rae_ext_rae_signal_new(void) {
  sem_t* semaphore = (sem_t*)malloc(sizeof(sem_t));
  if (semaphore && sem_init(semaphore, 0, 0) != 0) { free(semaphore); semaphore = NULL; }
  return (int64_t)(intptr_t)semaphore;
}
void rae_ext_rae_signal_post(int64_t signal) {
  sem_post((sem_t*)(intptr_t)signal);
}
void rae_ext_rae_signal_wait(int64_t signal) {
  sem_wait((sem_t*)(intptr_t)signal);
}
rae_Bool rae_ext_rae_signal_try_wait(int64_t signal) {
  return sem_trywait((sem_t*)(intptr_t)signal) == 0;
}
void rae_ext_rae_signal_free(int64_t signal) {
  sem_destroy((sem_t*)(intptr_t)signal);
  free((sem_t*)(intptr_t)signal);
}

#else
int64_t rae_ext_rae_signal_new(void) {
  return (int64_t)(intptr_t)calloc(1, sizeof(int64_t));
}
void rae_ext_rae_signal_post(int64_t signal) {
  *(int64_t*)(intptr_t)signal += 1;
}
void rae_ext_rae_signal_wait(int64_t signal) {
  if (*(int64_t*)(intptr_t)signal > 0) *(int64_t*)(intptr_t)signal -= 1;
}
rae_Bool rae_ext_rae_signal_try_wait(int64_t signal) {
  if (*(int64_t*)(intptr_t)signal <= 0) return 0;
  *(int64_t*)(intptr_t)signal -= 1;
  return 1;
}
void rae_ext_rae_signal_free(int64_t signal) {
  free((int64_t*)(intptr_t)signal);
}
#endif
