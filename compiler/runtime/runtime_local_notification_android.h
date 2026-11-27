/* The notification permission on Android, for lib/LocalNotification's ask and
 * answer (and so lib/PushNotification's, which shares them).
 *
 * Platform reason: Android 13 (API 33) asks the person for POST_NOTIFICATIONS
 * at run time, through the activity (SDL_RequestAndroidPermission, one call);
 * before 13 notifications need no question and are allowed. The answer comes
 * to a callback later, so it waits here. What it means is Rae's.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/LocalNotification. tools/android/gen-android.sh declares the permission
 * for such an app. Empty off Android. */
#ifndef RAE_RUNTIME_LOCAL_NOTIFICATION_ANDROID_H
#define RAE_RUNTIME_LOCAL_NOTIFICATION_ANDROID_H

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <stdatomic.h>
#include <stdint.h>

/* The last answer: -1 none yet, 0 no, 1 yes */
static _Atomic int rae_local_notification_android_answer = -1;

static void SDLCALL rae_local_notification_android_answered(void* userdata, const char* permission, bool granted) {
  (void)userdata; (void)permission;
  atomic_store(&rae_local_notification_android_answer, granted ? 1 : 0);
}

int64_t rae_ext_LocalNotification_androidAsk(void) {
  if (SDL_GetAndroidSDKVersion() < 33) {
    atomic_store(&rae_local_notification_android_answer, 1);
    return 1;
  }
  return SDL_RequestAndroidPermission("android.permission.POST_NOTIFICATIONS",
                                      rae_local_notification_android_answered, NULL) ? 1 : 0;
}

int64_t rae_ext_LocalNotification_androidAnswer(void) {
  return (int64_t)atomic_load(&rae_local_notification_android_answer);
}
#endif

#endif /* RAE_RUNTIME_LOCAL_NOTIFICATION_ANDROID_H */
