#include "sys_thread.h"
#include <stdlib.h>

#ifdef _WIN32
// Windows Implementation
bool sys_thread_create(sys_thread_t* thread, sys_thread_func_t func, void* arg) {
    *thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL);
    return *thread != NULL;
}

bool sys_thread_join(sys_thread_t thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return true;
}

bool sys_mutex_init(sys_mutex_t* mutex) {
    *mutex = CreateMutex(NULL, FALSE, NULL);
    return *mutex != NULL;
}

bool sys_mutex_lock(sys_mutex_t* mutex) {
    return WaitForSingleObject(*mutex, INFINITE) == WAIT_OBJECT_0;
}

bool sys_mutex_unlock(sys_mutex_t* mutex) {
    return ReleaseMutex(*mutex);
}

void sys_mutex_destroy(sys_mutex_t* mutex) {
    CloseHandle(*mutex);
}

void sys_sleep_ms(int ms) {
    Sleep(ms);
}

#else
// POSIX Implementation
#include <unistd.h>
#include <time.h>

bool sys_thread_create(sys_thread_t* thread, sys_thread_func_t func, void* arg) {
    return pthread_create(thread, NULL, func, arg) == 0;
}

bool sys_thread_join(sys_thread_t thread) {
    return pthread_join(thread, NULL) == 0;
}

bool sys_mutex_init(sys_mutex_t* mutex) {
    return pthread_mutex_init(mutex, NULL) == 0;
}

bool sys_mutex_lock(sys_mutex_t* mutex) {
    return pthread_mutex_lock(mutex) == 0;
}

bool sys_mutex_unlock(sys_mutex_t* mutex) {
    return pthread_mutex_unlock(mutex) == 0;
}

void sys_mutex_destroy(sys_mutex_t* mutex) {
    pthread_mutex_destroy(mutex);
}

void sys_sleep_ms(int ms) {
    usleep(ms * 1000);
}

#endif
