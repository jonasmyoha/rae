/* Android launch shim (tools/android).
 *
 * SDLActivity (SDL's Java half) loads libSDL3.so and the app's libmain.so and
 * calls SDL_main on its own thread. The app's emitted C is compiled with
 * `-Dmain=rae_android_app_main`, so this file's SDL_main runs first:
 *
 *   1. stdout and stderr go to logcat (tag "rae"): an Android app's stdout
 *      is /dev/null, and the app's log is how a run is read (`adb logcat
 *      -s rae`);
 *   2. the launch intent's "raeEnv" extra sets environment variables
 *      (`am start ... --es raeEnv "RAE_WALKER_PHYSICS_TEST=1 RAE_X=2"`), the
 *      way a desktop run sets them on the command line;
 *   3. the files the app reads (staged by tools/stage-app-assets.sh, packed
 *      into the APK's assets/) are copied to the app's internal storage once
 *      per APK: Rae reads files with fopen, and an APK's assets are not files.
 *      raeAssets.txt lists them; its first line is the bundle's stamp, and a
 *      copy is skipped when the stamp matches the last one;
 *   4. the working directory becomes that copy, so every relative read
 *      ("assets/...", "lib/...") resolves as at the repo root;
 * then the app's main runs, and its exit code is logged as
 * "@@RAE_APP_EXIT@@ code=N" (what tools/android/run-android.sh waits for).
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int rae_android_app_main(int argc, char** argv);

static int rae_android_log_pipe[2];

static void* rae_android_log_thread(void* unused) {
  (void)unused;
  char line[2048];
  size_t used = 0;
  for (;;) {
    char chunk[512];
    ssize_t got = read(rae_android_log_pipe[0], chunk, sizeof(chunk));
    if (got <= 0) break;
    for (ssize_t i = 0; i < got; i++) {
      char c = chunk[i];
      if (c == '\n' || used == sizeof(line) - 1) {
        line[used] = 0;
        __android_log_write(ANDROID_LOG_INFO, "rae", line);
        used = 0;
        if (c == '\n') continue;
      }
      line[used++] = c;
    }
  }
  return NULL;
}

static void rae_android_log_to_logcat(void) {
  if (pipe(rae_android_log_pipe) != 0) return;
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  dup2(rae_android_log_pipe[1], STDOUT_FILENO);
  dup2(rae_android_log_pipe[1], STDERR_FILENO);
  pthread_t thread;
  if (pthread_create(&thread, NULL, rae_android_log_thread, NULL) == 0) pthread_detach(thread);
}

/* The launch intent's "raeEnv" extra: KEY=VALUE pairs separated by spaces */
static void rae_android_env_from_intent(void) {
  JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
  jobject activity = (jobject)SDL_GetAndroidActivity();
  if (!env || !activity) return;
  jclass activityClass = (*env)->GetObjectClass(env, activity);
  jmethodID getIntent = (*env)->GetMethodID(env, activityClass, "getIntent", "()Landroid/content/Intent;");
  jobject intent = getIntent ? (*env)->CallObjectMethod(env, activity, getIntent) : NULL;
  if (intent) {
    jclass intentClass = (*env)->GetObjectClass(env, intent);
    jmethodID getStringExtra = (*env)->GetMethodID(env, intentClass, "getStringExtra",
                                                   "(Ljava/lang/String;)Ljava/lang/String;");
    jstring key = (*env)->NewStringUTF(env, "raeEnv");
    jstring value = (jstring)(*env)->CallObjectMethod(env, intent, getStringExtra, key);
    if (value) {
      const char* text = (*env)->GetStringUTFChars(env, value, NULL);
      char* pairs = strdup(text ? text : "");
      char* save = NULL;
      for (char* pair = strtok_r(pairs, " ", &save); pair; pair = strtok_r(NULL, " ", &save)) {
        char* equals = strchr(pair, '=');
        if (!equals) continue;
        *equals = 0;
        setenv(pair, equals + 1, 1);
        printf("[android] %s=%s\n", pair, equals + 1);
      }
      free(pairs);
      (*env)->ReleaseStringUTFChars(env, value, text);
      (*env)->DeleteLocalRef(env, value);
    }
    (*env)->DeleteLocalRef(env, key);
    (*env)->DeleteLocalRef(env, intentClass);
    (*env)->DeleteLocalRef(env, intent);
  }
  (*env)->DeleteLocalRef(env, activityClass);
  (*env)->DeleteLocalRef(env, activity);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static void rae_android_make_parents(const char* path) {
  char partial[1024];
  snprintf(partial, sizeof(partial), "%s", path);
  for (char* slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
    *slash = 0;
    mkdir(partial, 0700);
    *slash = '/';
  }
}

/* Copy the APK's bundled files to <internal storage>/bundle; false when the
 * bundle could not be made */
static int rae_android_unpack(const char* bundle) {
  size_t listSize = 0;
  char* list = (char*)SDL_LoadFile("raeAssets.txt", &listSize);  /* from the APK's assets/ */
  if (!list) {
    printf("[android] no raeAssets.txt in the APK: %s\n", SDL_GetError());
    return 0;
  }
  char* save = NULL;
  char* stamp = strtok_r(list, "\n", &save);
  char stampPath[1024];
  snprintf(stampPath, sizeof(stampPath), "%s/.raeStamp", bundle);
  size_t oldSize = 0;
  char* oldStamp = (char*)SDL_LoadFile(stampPath, &oldSize);
  int current = oldStamp && stamp && strcmp(oldStamp, stamp) == 0;
  SDL_free(oldStamp);
  if (current) {
    SDL_free(list);
    return 1;
  }
  int copied = 0, failed = 0;
  Uint64 started = SDL_GetTicks();
  for (char* file = strtok_r(NULL, "\n", &save); file; file = strtok_r(NULL, "\n", &save)) {
    size_t size = 0;
    void* data = SDL_LoadFile(file, &size);
    char target[1024];
    snprintf(target, sizeof(target), "%s/%s", bundle, file);
    rae_android_make_parents(target);
    if (!data || !SDL_SaveFile(target, data, size)) {
      printf("[android] could not unpack %s: %s\n", file, SDL_GetError());
      failed++;
    } else {
      copied++;
    }
    SDL_free(data);
  }
  if (failed == 0 && stamp) SDL_SaveFile(stampPath, stamp, strlen(stamp) + 1);
  printf("[android] unpacked %d files into %s in %llu ms (%d failed)\n", copied, bundle,
         (unsigned long long)(SDL_GetTicks() - started), failed);
  SDL_free(list);
  return failed == 0;
}

int SDL_main(int argc, char** argv) {
  rae_android_log_to_logcat();
  rae_android_env_from_intent();
  const char* internal = SDL_GetAndroidInternalStoragePath();
  if (internal && *internal) {
    char bundle[1024];
    snprintf(bundle, sizeof(bundle), "%s/bundle", internal);
    mkdir(bundle, 0700);
    rae_android_unpack(bundle);
    if (chdir(bundle) != 0) printf("[android] chdir %s failed: %s\n", bundle, strerror(errno));
    /* A home for what the app writes (settings, screenshots, logs) */
    if (!getenv("HOME")) setenv("HOME", internal, 1);
  }
  const char* cache = SDL_GetAndroidCachePath();
  if (cache && *cache && !getenv("TMPDIR")) setenv("TMPDIR", cache, 1);
  int code = rae_android_app_main(argc, argv);
  printf("@@RAE_APP_EXIT@@ code=%d\n", code);
  fflush(stdout);
  return code;
}
