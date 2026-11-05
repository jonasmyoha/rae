/* Local notifications for lib/LocalNotification on iOS: UNUserNotificationCenter.
 *
 * Platform reason: Objective-C calls through the runtime (this header is C). Each
 * function makes one call on the notification center, or builds one request for it.
 * Whether a moment is in the past, how long until it, what a headless run does and
 * what the other platforms do are Rae's (lib/LocalNotification). Every iOS function
 * answers 1 when the call was made, 0 when the class is missing.
 *
 * A request added while the question is open is, as far as known, refused by the system
 * (UNErrorCodeNotificationsNotAllowed), and Rae cannot run in the block that gets the answer. So schedule and cancel run in order on one
 * serial dispatch queue, which ask suspends until the answer comes: the queue is what
 * holds them, and no list is kept here.
 *
 * rae_ext_LocalNotification_realNowMs is on every target: the wall clock without
 * RAE_NOW, since a notification rings by the phone's real clock.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/LocalNotification. tools/ios/gen-ios.sh links UserNotifications for it. */
#ifndef RAE_RUNTIME_LOCAL_NOTIFICATION_APPLE_H
#define RAE_RUNTIME_LOCAL_NOTIFICATION_APPLE_H

#include <stdint.h>
#include <time.h>
#include "rae_runtime.h"

int64_t rae_ext_LocalNotification_realNowMs(void) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (int64_t)now.tv_sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
}

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdatomic.h>

/* UNAuthorizationOptionSound | UNAuthorizationOptionAlert */
#define RAE_LOCAL_NOTIFICATION_OPTIONS 6UL

static void* rae_local_notification_center(void) {
  Class center = objc_getClass("UNUserNotificationCenter");
  if (!center) return NULL;
  return ((void* (*)(void*, SEL))objc_msgSend)((void*)center, sel_registerName("currentNotificationCenter"));
}

/* An NSString of a Rae string's UTF-8 bytes; NSUTF8StringEncoding is 4 */
static void* rae_local_notification_string(rae_String text) {
  void* made = ((void* (*)(void*, SEL))objc_msgSend)((void*)objc_getClass("NSString"), sel_registerName("alloc"));
  made = ((void* (*)(void*, SEL, const void*, unsigned long, unsigned long))objc_msgSend)(
      made, sel_registerName("initWithBytes:length:encoding:"), text.data, (unsigned long)text.len, 4UL);
  return ((void* (*)(void*, SEL))objc_msgSend)(made, sel_registerName("autorelease"));
}

static void* rae_local_notification_ids(rae_String id) {
  return ((void* (*)(void*, SEL, void*))objc_msgSend)((void*)objc_getClass("NSArray"),
                                                      sel_registerName("arrayWithObject:"),
                                                      rae_local_notification_string(id));
}

/* Suspended while a question is open (rae_ext_LocalNotification_ask) */
static dispatch_queue_t rae_local_notification_queue(void) {
  static dispatch_once_t once;
  static dispatch_queue_t queue;
  dispatch_once(&once, ^{
    queue = dispatch_queue_create("rae.localNotification", DISPATCH_QUEUE_SERIAL);
  });
  return queue;
}

/* The last answer: -1 none yet, 0 no, 1 yes */
static _Atomic int rae_local_notification_answer = -1;

int64_t rae_ext_LocalNotification_answer(void) {
  return (int64_t)atomic_load(&rae_local_notification_answer);
}

/* Asks the person the first time; afterward the system answers from what they chose */
int64_t rae_ext_LocalNotification_ask(void) {
  void* center = rae_local_notification_center();
  if (!center) return 0;
  dispatch_queue_t queue = rae_local_notification_queue();
  dispatch_suspend(queue);
  ((void (*)(void*, SEL, unsigned long, void*))objc_msgSend)(
      center, sel_registerName("requestAuthorizationWithOptions:completionHandler:"),
      RAE_LOCAL_NOTIFICATION_OPTIONS, (void*)^(signed char granted, void* error) {
        (void)error;
        atomic_store(&rae_local_notification_answer, granted ? 1 : 0);
        dispatch_resume(queue);
      });
  return 1;
}

/* One request `seconds` from this call, with the default sound; the same id replaces it.
 * The trigger is made when the queue reaches it, from the time left then, so a held one
 * rings at its moment. One whose moment passed while it was held is not added. */
int64_t rae_ext_LocalNotification_schedule(rae_String id, double seconds, rae_String title, rae_String body) {
  void* center = rae_local_notification_center();
  Class contentClass = objc_getClass("UNMutableNotificationContent");
  Class soundClass = objc_getClass("UNNotificationSound");
  Class triggerClass = objc_getClass("UNTimeIntervalNotificationTrigger");
  Class requestClass = objc_getClass("UNNotificationRequest");
  if (!center || !contentClass || !soundClass || !triggerClass || !requestClass) return 0;
  void* content = ((void* (*)(void*, SEL))objc_msgSend)((void*)contentClass, sel_registerName("new"));
  ((void (*)(void*, SEL, void*))objc_msgSend)(content, sel_registerName("setTitle:"),
                                              rae_local_notification_string(title));
  ((void (*)(void*, SEL, void*))objc_msgSend)(content, sel_registerName("setBody:"),
                                              rae_local_notification_string(body));
  ((void (*)(void*, SEL, void*))objc_msgSend)(
      content, sel_registerName("setSound:"),
      ((void* (*)(void*, SEL))objc_msgSend)((void*)soundClass, sel_registerName("defaultSound")));
  void* identifier = rae_local_notification_string(id);
  ((void* (*)(void*, SEL))objc_msgSend)(identifier, sel_registerName("retain"));
  int64_t calledMs = rae_ext_LocalNotification_realNowMs();
  dispatch_async(rae_local_notification_queue(), ^{
    double left = seconds - (double)(rae_ext_LocalNotification_realNowMs() - calledMs) / 1000.0;
    if (left > 0.0) {
      void* trigger = ((void* (*)(void*, SEL, double, signed char))objc_msgSend)(
          (void*)triggerClass, sel_registerName("triggerWithTimeInterval:repeats:"), left, 0);
      void* request = ((void* (*)(void*, SEL, void*, void*, void*))objc_msgSend)(
          (void*)requestClass, sel_registerName("requestWithIdentifier:content:trigger:"), identifier, content,
          trigger);
      ((void (*)(void*, SEL, void*, void*))objc_msgSend)(
          center, sel_registerName("addNotificationRequest:withCompletionHandler:"), request, NULL);
    }
    ((void (*)(void*, SEL))objc_msgSend)(identifier, sel_registerName("release"));
    ((void (*)(void*, SEL))objc_msgSend)(content, sel_registerName("release"));
  });
  return 1;
}

int64_t rae_ext_LocalNotification_cancel(rae_String id) {
  void* center = rae_local_notification_center();
  if (!center) return 0;
  void* ids = rae_local_notification_ids(id);
  ((void* (*)(void*, SEL))objc_msgSend)(ids, sel_registerName("retain"));
  dispatch_async(rae_local_notification_queue(), ^{
    ((void (*)(void*, SEL, void*))objc_msgSend)(
        center, sel_registerName("removePendingNotificationRequestsWithIdentifiers:"), ids);
    ((void (*)(void*, SEL))objc_msgSend)(ids, sel_registerName("release"));
  });
  return 1;
}

int64_t rae_ext_LocalNotification_cancelAll(void) {
  void* center = rae_local_notification_center();
  if (!center) return 0;
  dispatch_async(rae_local_notification_queue(), ^{
    ((void (*)(void*, SEL))objc_msgSend)(center, sel_registerName("removeAllPendingNotificationRequests"));
  });
  return 1;
}

int64_t rae_ext_LocalNotification_clearDelivered(void) {
  void* center = rae_local_notification_center();
  if (!center) return 0;
  ((void (*)(void*, SEL))objc_msgSend)(center, sel_registerName("removeAllDeliveredNotifications"));
  return 1;
}
#endif

#endif /* RAE_RUNTIME_LOCAL_NOTIFICATION_APPLE_H */
