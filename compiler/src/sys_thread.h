#ifndef SYS_THREAD_H
#define SYS_THREAD_H

#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE sys_thread_t;
typedef HANDLE sys_mutex_t;
#else
#include <pthread.h>
typedef pthread_t sys_thread_t;
typedef pthread_mutex_t sys_mutex_t;
#endif

// Thread function signature
typedef void* (*sys_thread_func_t)(void* arg);

// Thread API
bool sys_thread_create(sys_thread_t* thread, sys_thread_func_t func, void* arg);
bool sys_thread_join(sys_thread_t thread);
sys_thread_t sys_thread_self(void);

// Mutex API
bool sys_mutex_init(sys_mutex_t* mutex);
bool sys_mutex_lock(sys_mutex_t* mutex);
bool sys_mutex_unlock(sys_mutex_t* mutex);
void sys_mutex_destroy(sys_mutex_t* mutex);

// Sleep API (for polling loops)
void sys_sleep_ms(int ms);

#endif /* SYS_THREAD_H */
