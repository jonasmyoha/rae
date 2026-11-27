/* Push notifications for lib/PushNotification: the device token and the
 * notifications that arrive (docs/push-notifications.md).
 *
 * Platform reason: the system hands the token and each notification to its
 * own callbacks (on iOS the app delegate and the UNUserNotificationCenter
 * delegate), which can run before Rae asks: a notification whose tap launched
 * the app comes before main has read anything. So what they hand over waits
 * here, under a lock, until Rae takes it: one notification per take, the
 * token and the last registration error as text. What a payload means, what
 * a headless run does and the stand-ins are Rae's.
 *
 * On iOS the header adds, through the Objective-C runtime (this header is
 * C), the two registration callbacks to SDL's app delegate and a
 * notification-center delegate set in application:willFinishLaunching. That
 * delegate keeps the foreground as LocalNotification has it (nothing shown)
 * and passes on only remote notifications (UNPushNotificationTrigger): a
 * local one is not a push. rae_ext_PushNotification_register makes the one
 * registerForRemoteNotifications call. On Android (FCM) the Java side calls
 * in (below). Elsewhere only the queue exists, for the simulated token and
 * arrivals.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/PushNotification. tools/ios/gen-ios.sh links UserNotifications for it
 * and writes the aps-environment entitlement. */
#ifndef RAE_RUNTIME_PUSH_NOTIFICATION_H
#define RAE_RUNTIME_PUSH_NOTIFICATION_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rae_runtime.h"

void rae_ext_EventLoop_wake(void);

/* Waiting notifications, oldest first: each a malloc'd "a<json>" (arrived while
 * the app ran) or "t<json>" (its tap opened the app); full drops the newest */
#define RAE_PUSH_WAITING_MAX 64
static pthread_mutex_t rae_push_lock = PTHREAD_MUTEX_INITIALIZER;
static char* rae_push_waiting[RAE_PUSH_WAITING_MAX];
static int rae_push_waiting_count = 0;
static char rae_push_token[512];
static char rae_push_error[512];

static void rae_push_notification_add(char kind, const char* payload, size_t length) {
  char* entry = (char*)malloc(length + 2);
  if (!entry) return;
  entry[0] = kind;
  if (length > 0) memcpy(entry + 1, payload, length);
  entry[length + 1] = 0;
  pthread_mutex_lock(&rae_push_lock);
  if (rae_push_waiting_count < RAE_PUSH_WAITING_MAX) {
    rae_push_waiting[rae_push_waiting_count++] = entry;
    entry = NULL;
  }
  pthread_mutex_unlock(&rae_push_lock);
  free(entry);
}

static void rae_push_notification_set(char* into, const char* text, size_t length) {
  pthread_mutex_lock(&rae_push_lock);
  if (length > 511) length = 511;
  memcpy(into, text, length);
  into[length] = 0;
  pthread_mutex_unlock(&rae_push_lock);
}

/* The oldest waiting notification, "" when none */
rae_String rae_ext_PushNotification_take(void) {
  char* entry = NULL;
  pthread_mutex_lock(&rae_push_lock);
  if (rae_push_waiting_count > 0) {
    entry = rae_push_waiting[0];
    memmove(rae_push_waiting, rae_push_waiting + 1, sizeof(char*) * (size_t)(rae_push_waiting_count - 1));
    rae_push_waiting_count--;
  }
  pthread_mutex_unlock(&rae_push_lock);
  rae_String taken = rae_ext_rae_str_from_cstr(entry ? entry : "");
  free(entry);
  return taken;
}

static rae_String rae_push_notification_read(const char* from) {
  pthread_mutex_lock(&rae_push_lock);
  rae_String text = rae_ext_rae_str_from_cstr(from);
  pthread_mutex_unlock(&rae_push_lock);
  return text;
}

/* The device token as the system gave it (hex on iOS), "" before it came */
rae_String rae_ext_PushNotification_token(void) { return rae_push_notification_read(rae_push_token); }

/* Why the last registration failed, "" when it did not */
rae_String rae_ext_PushNotification_error(void) { return rae_push_notification_read(rae_push_error); }

/* A test or a stand-in, in the system's place */
void rae_ext_PushNotification_simulate(rae_String payload, rae_Bool tapped) {
  rae_push_notification_add(tapped ? 't' : 'a', (const char*)payload.data, (size_t)payload.len);
  rae_ext_EventLoop_wake();
}

void rae_ext_PushNotification_simulateToken(rae_String token) {
  rae_push_notification_set(rae_push_token, (const char*)token.data, (size_t)token.len);
  rae_ext_EventLoop_wake();
}

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS
#include <objc/message.h>
#include <objc/runtime.h>

static void* rae_push_send(void* object, const char* selector) {
  return ((void* (*)(void*, SEL))objc_msgSend)(object, sel_registerName(selector));
}

/* Only a remote notification is a push: a local one has another trigger */
static int rae_push_is_remote(void* notification) {
  void* request = rae_push_send(notification, "request");
  void* trigger = request ? rae_push_send(request, "trigger") : NULL;
  Class pushTrigger = objc_getClass("UNPushNotificationTrigger");
  if (!trigger || !pushTrigger) return 0;
  return ((signed char (*)(void*, SEL, void*))objc_msgSend)(trigger, sel_registerName("isKindOfClass:"),
                                                            (void*)pushTrigger) != 0;
}

/* The notification's userInfo (the payload the server sent) as JSON text */
static void rae_push_add_notification(char kind, void* notification) {
  if (!notification || !rae_push_is_remote(notification)) return;
  void* content = rae_push_send(rae_push_send(notification, "request"), "content");
  void* userInfo = content ? rae_push_send(content, "userInfo") : NULL;
  Class serialization = objc_getClass("NSJSONSerialization");
  void* data = NULL;
  if (userInfo && serialization
      && ((signed char (*)(void*, SEL, void*))objc_msgSend)((void*)serialization, sel_registerName("isValidJSONObject:"),
                                                            userInfo)) {
    data = ((void* (*)(void*, SEL, void*, unsigned long, void*))objc_msgSend)(
        (void*)serialization, sel_registerName("dataWithJSONObject:options:error:"), userInfo, 0UL, NULL);
  }
  const char* bytes = data ? (const char*)rae_push_send(data, "bytes") : "{}";
  size_t length = data ? (size_t)((unsigned long (*)(void*, SEL))objc_msgSend)(data, sel_registerName("length")) : 2;
  rae_push_notification_add(kind, bytes, length);
  rae_ext_EventLoop_wake();
}

/* A notification while the app is in the foreground: shown as without a
 * delegate (nothing), and passed on */
static void rae_push_will_present(void* self, SEL cmd, void* center, void* notification, void* handler) {
  (void)self; (void)cmd; (void)center;
  rae_push_add_notification('a', notification);
  ((void (^)(unsigned long))handler)(0UL);
}

/* The person tapped a notification: it opened (or brought back) the app */
static void rae_push_did_receive(void* self, SEL cmd, void* center, void* response, void* handler) {
  (void)self; (void)cmd; (void)center;
  rae_push_add_notification('t', rae_push_send(response, "notification"));
  ((void (^)(void))handler)();
}

static void rae_push_did_register(void* self, SEL cmd, void* application, void* token) {
  (void)self; (void)cmd; (void)application;
  const unsigned char* bytes = (const unsigned char*)rae_push_send(token, "bytes");
  unsigned long length = ((unsigned long (*)(void*, SEL))objc_msgSend)(token, sel_registerName("length"));
  char hex[512];
  size_t written = 0;
  for (unsigned long i = 0; i < length && written + 2 < sizeof(hex); i++) {
    static const char digits[] = "0123456789abcdef";
    hex[written++] = digits[bytes[i] >> 4];
    hex[written++] = digits[bytes[i] & 15];
  }
  rae_push_notification_set(rae_push_token, hex, written);
  rae_push_notification_set(rae_push_error, "", 0);
  rae_ext_EventLoop_wake();
}

static void rae_push_did_fail(void* self, SEL cmd, void* application, void* error) {
  (void)self; (void)cmd; (void)application;
  void* description = error ? rae_push_send(error, "localizedDescription") : NULL;
  const char* text = description ? (const char*)rae_push_send(description, "UTF8String") : "registration failed";
  rae_push_notification_set(rae_push_error, text, strlen(text));
  rae_ext_EventLoop_wake();
}

/* The center's delegate is weak: this keeps it */
static void* rae_push_center_delegate = NULL;

static signed char rae_push_will_finish(void* self, SEL cmd, void* application, void* options) {
  (void)self; (void)cmd; (void)application; (void)options;
  Class center = objc_getClass("UNUserNotificationCenter");
  Class delegateClass = objc_getClass("RaePushNotificationDelegate");
  if (center && delegateClass && !rae_push_center_delegate) {
    rae_push_center_delegate = rae_push_send(rae_push_send((void*)delegateClass, "alloc"), "init");
    ((void (*)(void*, SEL, void*))objc_msgSend)(rae_push_send((void*)center, "currentNotificationCenter"),
                                                sel_registerName("setDelegate:"), rae_push_center_delegate);
  }
  return 1;
}

static void rae_push_add_to_app_delegate(const char* name) {
  Class delegate = objc_getClass(name);
  if (!delegate) return;
  class_addMethod(delegate, sel_registerName("application:didRegisterForRemoteNotificationsWithDeviceToken:"),
                  (IMP)rae_push_did_register, "v@:@@");
  class_addMethod(delegate, sel_registerName("application:didFailToRegisterForRemoteNotificationsWithError:"),
                  (IMP)rae_push_did_fail, "v@:@@");
  class_addMethod(delegate, sel_registerName("application:willFinishLaunchingWithOptions:"),
                  (IMP)rae_push_will_finish, "c@:@@");
}

__attribute__((constructor)) static void rae_push_install(void) {
  Class base = objc_getClass("NSObject");
  if (base && !objc_getClass("RaePushNotificationDelegate")) {
    Class made = objc_allocateClassPair(base, "RaePushNotificationDelegate", 0);
    if (made) {
      Protocol* protocol = objc_getProtocol("UNUserNotificationCenterDelegate");
      if (protocol) class_addProtocol(made, protocol);
      class_addMethod(made, sel_registerName("userNotificationCenter:willPresentNotification:withCompletionHandler:"),
                      (IMP)rae_push_will_present, "v@:@@@?");
      class_addMethod(made,
                      sel_registerName("userNotificationCenter:didReceiveNotificationResponse:withCompletionHandler:"),
                      (IMP)rae_push_did_receive, "v@:@@@?");
      objc_registerClassPair(made);
    }
  }
  /* iOS 13 and later run the scene delegate as the app delegate */
  rae_push_add_to_app_delegate("SDLUIKitSceneDelegate");
  rae_push_add_to_app_delegate("SDLUIKitDelegate");
}

/* Ask APNs for the device token; it comes to the delegate above */
int64_t rae_ext_PushNotification_register(void) {
  Class application = objc_getClass("UIApplication");
  if (!application) return 0;
  rae_push_send(rae_push_send((void*)application, "sharedApplication"), "registerForRemoteNotifications");
  return 1;
}
#endif /* iOS */


#if defined(__ANDROID__)
/* Android: Firebase Cloud Messaging, through tools/android/firebase/java's
 * RaePushService (the token, a message while the app runs) and RaeActivity
 * (the tap that opened the app). Each Java callback hands its text here. The
 * Java class is in the app only when it was built with the app's
 * google-services.json; without it register answers 0. */
#include <jni.h>
#include <SDL3/SDL_system.h>

static void rae_push_from_java(JNIEnv* env, jstring text, char kind) {
  if (!text) return;
  const char* utf8 = (*env)->GetStringUTFChars(env, text, NULL);
  if (!utf8) return;
  if (kind == 'k') rae_push_notification_set(rae_push_token, utf8, strlen(utf8));
  else if (kind == 'e') rae_push_notification_set(rae_push_error, utf8, strlen(utf8));
  else rae_push_notification_add(kind, utf8, strlen(utf8));
  (*env)->ReleaseStringUTFChars(env, text, utf8);
  rae_ext_EventLoop_wake();
}

JNIEXPORT void JNICALL Java_org_rae_app_push_RaePushService_nativePushToken(JNIEnv* env, jclass cls, jstring token) {
  (void)cls;
  rae_push_from_java(env, token, 'k');
  rae_push_notification_set(rae_push_error, "", 0);
}

JNIEXPORT void JNICALL Java_org_rae_app_push_RaePushService_nativePushError(JNIEnv* env, jclass cls, jstring error) {
  (void)cls;
  rae_push_from_java(env, error, 'e');
}

JNIEXPORT void JNICALL Java_org_rae_app_push_RaePushService_nativePushArrived(JNIEnv* env, jclass cls, jstring payload) {
  (void)cls;
  rae_push_from_java(env, payload, 'a');
}

JNIEXPORT void JNICALL Java_org_rae_app_RaeActivity_nativePushTapped(JNIEnv* env, jclass cls, jstring payload) {
  (void)cls;
  rae_push_from_java(env, payload, 't');
}

/* Ask FCM for the device token (RaePushService.register); 0 when the app was
 * built without Firebase */
int64_t rae_ext_PushNotification_register(void) {
  JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
  if (!env) return 0;
  jclass service = (*env)->FindClass(env, "org/rae/app/push/RaePushService");
  if (!service) {
    (*env)->ExceptionClear(env);
    rae_push_notification_set(rae_push_error, "built without Firebase (no google-services.json)", 48);
    return 0;
  }
  jmethodID registerMethod = (*env)->GetStaticMethodID(env, service, "register", "()Z");
  jboolean asked = registerMethod ? (*env)->CallStaticBooleanMethod(env, service, registerMethod) : JNI_FALSE;
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    asked = JNI_FALSE;
  }
  (*env)->DeleteLocalRef(env, service);
  return asked ? 1 : 0;
}
#endif /* Android */

#endif /* RAE_RUNTIME_PUSH_NOTIFICATION_H */
