/* Android's half of lib/net/NetworkConnection (docs/network-connections.md): the
 * same functions as runtime_network_apple.h, over tools/android/java's RaeNetwork.
 *
 * Platform reason: the NDK has no TLS and no way to the system's trust store, so the
 * connection itself is Java, on its own threads. Each connection has a small event
 * queue here: Java appends events, which signals a waiting worker and wakes the UI
 * loop (EventLoop.wake). Rae takes them one at a time and decides what they mean. No
 * other policy lives here.
 *
 * Header-only: the generated C of a program whose Rae imports the module includes it
 * (`cheader`). Empty off Android. Handles are integers (a pointer as int64_t, 0 =
 * none). */
#ifndef RAE_RUNTIME_NETWORK_ANDROID_H
#define RAE_RUNTIME_NETWORK_ANDROID_H

#if defined(__ANDROID__)
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <SDL3/SDL_system.h>

void rae_ext_EventLoop_wake(void);

typedef struct RaeNwEvent {
  int64_t kind;
  int64_t code;
  uint8_t* data;
  int64_t length;
  struct RaeNwEvent* next;
} RaeNwEvent;

typedef struct RaeNwConnection {
  jobject java;
  pthread_mutex_t lock;
  pthread_cond_t arrived;
  RaeNwEvent* head;
  RaeNwEvent* tail;
  RaeNwEvent* current;
  /* Java will send no more events */
  int finished;
  /* Rae has let go of the handle */
  int released;
} RaeNwConnection;

/* RaeNetwork is looked up once through the activity's class loader: a thread that
 * did not start in Java finds only the system's classes with FindClass */
static jclass rae_nw_class = NULL;
static jmethodID rae_nw_connect_method = NULL;
static jmethodID rae_nw_send_method = NULL;
static jmethodID rae_nw_close_method = NULL;
static pthread_mutex_t rae_nw_class_lock = PTHREAD_MUTEX_INITIALIZER;

static int rae_nw_failed_call(JNIEnv* env) {
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    return 1;
  }
  return 0;
}

static int rae_nw_find_class(JNIEnv* env) {
  pthread_mutex_lock(&rae_nw_class_lock);
  if (!rae_nw_class) {
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (activity) {
      jclass activityClass = (*env)->GetObjectClass(env, activity);
      jmethodID getLoader = (*env)->GetMethodID(env, activityClass, "getClassLoader",
                                                "()Ljava/lang/ClassLoader;");
      jobject loader = (*env)->CallObjectMethod(env, activity, getLoader);
      jclass loaderClass = (*env)->GetObjectClass(env, loader);
      jmethodID loadClass = (*env)->GetMethodID(env, loaderClass, "loadClass",
                                                "(Ljava/lang/String;)Ljava/lang/Class;");
      jstring name = (*env)->NewStringUTF(env, "org.rae.app.RaeNetwork");
      jclass found = (jclass)(*env)->CallObjectMethod(env, loader, loadClass, name);
      if (!rae_nw_failed_call(env) && found) {
        rae_nw_connect_method = (*env)->GetStaticMethodID(
            env, found, "connect",
            "(JLjava/lang/String;ILjava/lang/String;IZ)Lorg/rae/app/RaeNetwork;");
        rae_nw_send_method = (*env)->GetMethodID(env, found, "send", "([BII)V");
        rae_nw_close_method = (*env)->GetMethodID(env, found, "close", "()V");
        rae_nw_class = (jclass)(*env)->NewGlobalRef(env, found);
        (*env)->DeleteLocalRef(env, found);
      }
      (*env)->DeleteLocalRef(env, name);
      (*env)->DeleteLocalRef(env, loaderClass);
      (*env)->DeleteLocalRef(env, loader);
      (*env)->DeleteLocalRef(env, activityClass);
      (*env)->DeleteLocalRef(env, activity);
    }
  }
  int found = rae_nw_class != NULL;
  pthread_mutex_unlock(&rae_nw_class_lock);
  return found;
}

static void rae_nw_free_event(RaeNwEvent* event) {
  if (!event) return;
  free(event->data);
  free(event);
}

/* Free the connection once Java is finished with it and Rae has let go */
static void rae_nw_maybe_free(RaeNwConnection* c) {
  pthread_mutex_lock(&c->lock);
  int done = c->finished && c->released;
  pthread_mutex_unlock(&c->lock);
  if (!done) return;
  rae_nw_free_event(c->current);
  while (c->head) {
    RaeNwEvent* next = c->head->next;
    rae_nw_free_event(c->head);
    c->head = next;
  }
  pthread_mutex_destroy(&c->lock);
  pthread_cond_destroy(&c->arrived);
  free(c);
}

JNIEXPORT void JNICALL Java_org_rae_app_RaeNetwork_nativeEvent(
    JNIEnv* env, jclass unused, jlong handle, jint kind, jint code, jbyteArray data) {
  (void)unused;
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  RaeNwEvent* event = (RaeNwEvent*)calloc(1, sizeof *event);
  if (!c || !event) {
    free(event);
    return;
  }
  event->kind = kind;
  event->code = code;
  event->length = data ? (*env)->GetArrayLength(env, data) : 0;
  if (event->length > 0) {
    event->data = (uint8_t*)malloc((size_t)event->length);
    if (!event->data) {
      free(event);
      return;
    }
    (*env)->GetByteArrayRegion(env, data, 0, (jsize)event->length, (jbyte*)event->data);
  }
  pthread_mutex_lock(&c->lock);
  if (c->released) {
    pthread_mutex_unlock(&c->lock);
    rae_nw_free_event(event);
    return;
  }
  if (c->tail) c->tail->next = event; else c->head = event;
  c->tail = event;
  pthread_cond_signal(&c->arrived);
  pthread_mutex_unlock(&c->lock);
  rae_ext_EventLoop_wake();
}

JNIEXPORT void JNICALL Java_org_rae_app_RaeNetwork_nativeFinished(
    JNIEnv* env, jclass unused, jlong handle) {
  (void)env;
  (void)unused;
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  if (!c) return;
  pthread_mutex_lock(&c->lock);
  c->finished = 1;
  pthread_mutex_unlock(&c->lock);
  rae_nw_maybe_free(c);
}

/* Open a connection: to `hostZ`:`port` (TCP or TLS), or to the ws:// / wss://
 * `urlZ` (a WebSocket client). `insecure` accepts any server certificate (only for a
 * test against a self-signed server). The handle, or 0. */
int64_t rae_ext_Nw_connect(uint8_t* hostZ, int64_t port, uint8_t* urlZ, int64_t mode,
                           int64_t insecure) {
  JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
  if (!env || !rae_nw_find_class(env)) return 0;
  RaeNwConnection* c = (RaeNwConnection*)calloc(1, sizeof *c);
  if (!c) return 0;
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->arrived, NULL);
  jstring host = (*env)->NewStringUTF(env, (const char*)hostZ);
  jstring url = (*env)->NewStringUTF(env, (const char*)urlZ);
  jobject java = (*env)->CallStaticObjectMethod(
      env, rae_nw_class, rae_nw_connect_method, (jlong)(intptr_t)c, host, (jint)port, url,
      (jint)mode, insecure ? JNI_TRUE : JNI_FALSE);
  int failed = rae_nw_failed_call(env);
  (*env)->DeleteLocalRef(env, host);
  (*env)->DeleteLocalRef(env, url);
  if (failed || !java) {
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->arrived);
    free(c);
    return 0;
  }
  c->java = (*env)->NewGlobalRef(env, java);
  (*env)->DeleteLocalRef(env, java);
  return (int64_t)(intptr_t)c;
}

/* Send `count` bytes of `bytes` from `offset`. On a WebSocket `opcode` says the
 * message's kind (1 text, 2 binary, 8 close with `closeCode`, 9 ping); elsewhere it is
 * ignored. A failure arrives as a send-failed event. */
int64_t rae_ext_Nw_send(int64_t handle, uint8_t* bytes, int64_t offset, int64_t count,
                        int64_t opcode, int64_t closeCode) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
  if (!c || !env) return -1;
  jbyteArray data = (*env)->NewByteArray(env, (jsize)count);
  if (!data) {
    rae_nw_failed_call(env);
    return -1;
  }
  if (count > 0) {
    (*env)->SetByteArrayRegion(env, data, 0, (jsize)count, (const jbyte*)(bytes + offset));
  }
  (*env)->CallVoidMethod(env, c->java, rae_nw_send_method, data, (jint)opcode, (jint)closeCode);
  int failed = rae_nw_failed_call(env);
  (*env)->DeleteLocalRef(env, data);
  return failed ? -1 : 0;
}

/* The next event's kind, waiting up to `timeoutMs` (0: none came). Blocking, so a
 * worker may wait here. */
int64_t rae_ext_Nw_nextEvent(int64_t handle, int64_t timeoutMs) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  if (!c) return 0;
  pthread_mutex_lock(&c->lock);
  rae_nw_free_event(c->current);
  c->current = NULL;
  if (!c->head && timeoutMs > 0) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec deadline;
    int64_t nanoseconds = (int64_t)now.tv_usec * 1000 + (timeoutMs % 1000) * 1000000;
    deadline.tv_sec = now.tv_sec + (time_t)(timeoutMs / 1000) + (time_t)(nanoseconds / 1000000000);
    deadline.tv_nsec = (long)(nanoseconds % 1000000000);
    while (!c->head) {
      if (pthread_cond_timedwait(&c->arrived, &c->lock, &deadline) != 0) break;
    }
  }
  int64_t kind = 0;
  if (c->head) {
    c->current = c->head;
    c->head = c->head->next;
    if (!c->head) c->tail = NULL;
    kind = c->current->kind;
  }
  pthread_mutex_unlock(&c->lock);
  return kind;
}

int64_t rae_ext_Nw_eventCode(int64_t handle) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  return c && c->current ? c->current->code : 0;
}

int64_t rae_ext_Nw_eventLength(int64_t handle) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  return c && c->current ? c->current->length : 0;
}

void rae_ext_Nw_eventCopy(int64_t handle, uint8_t* into) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  if (c && c->current && c->current->length > 0) {
    memcpy(into, c->current->data, (size_t)c->current->length);
  }
}

/* Close now: the events stop, and the memory goes when Java's threads have ended */
void rae_ext_Nw_release(int64_t handle) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
  if (!c) return;
  pthread_mutex_lock(&c->lock);
  c->released = 1;
  pthread_mutex_unlock(&c->lock);
  if (env && c->java) {
    (*env)->CallVoidMethod(env, c->java, rae_nw_close_method);
    rae_nw_failed_call(env);
    (*env)->DeleteGlobalRef(env, c->java);
    c->java = NULL;
  }
  rae_nw_maybe_free(c);
}

#endif /* __ANDROID__ */
#endif
