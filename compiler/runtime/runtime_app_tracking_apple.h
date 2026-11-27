/* App Tracking Transparency, the advertising id and SKAdNetwork for
 * lib/AppTracking (docs/app-tracking.md).
 *
 * Platform reason: Objective-C calls through the runtime (this header is C).
 * Each iOS function makes one call: ATTrackingManager's status or request,
 * ASIdentifierManager's advertising id, or one SKAdNetwork update, the newest
 * form the running iOS has (16.1 fine + coarse + lock window, 15.4 fine
 * only, 14.0 the old conversion value). What a status means, which values are
 * valid and what a headless run does are Rae's.
 *
 * The simulated status is on every target: a headless run's stand-in for the
 * person's answer (lib/AppTracking keeps the state machine, this holds the
 * one number between calls).
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/AppTracking. tools/ios/gen-ios.sh links AppTrackingTransparency,
 * AdSupport and StoreKit for it and writes NSUserTrackingUsageDescription. */
#ifndef RAE_RUNTIME_APP_TRACKING_APPLE_H
#define RAE_RUNTIME_APP_TRACKING_APPLE_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "rae_runtime.h"

/* A headless run's status: 0 not determined, 1 restricted, 2 denied, 3 authorized */
static _Atomic int rae_app_tracking_simulated = 0;

int64_t rae_ext_AppTracking_simulatedStatus(void) { return (int64_t)atomic_load(&rae_app_tracking_simulated); }

void rae_ext_AppTracking_setSimulatedStatus(int64_t status) { atomic_store(&rae_app_tracking_simulated, (int)status); }

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

static void* rae_app_tracking_send(void* object, const char* selector) {
  return ((void* (*)(void*, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static int rae_app_tracking_responds(void* object, const char* selector) {
  return ((signed char (*)(void*, SEL, SEL))objc_msgSend)(object, sel_registerName("respondsToSelector:"),
                                                          sel_registerName(selector)) != 0;
}

/* ATTrackingAuthorizationStatus: 0 not determined, 1 restricted, 2 denied, 3 authorized;
 * -1 when the class is missing */
int64_t rae_ext_AppTracking_status(void) {
  Class manager = objc_getClass("ATTrackingManager");
  if (!manager) return -1;
  return (int64_t)((unsigned long (*)(void*, SEL))objc_msgSend)((void*)manager,
                                                                sel_registerName("trackingAuthorizationStatus"));
}

/* Asks the person (once per install; afterward the system answers at once). The
 * answer is read with rae_ext_AppTracking_status. */
int64_t rae_ext_AppTracking_request(void) {
  Class manager = objc_getClass("ATTrackingManager");
  if (!manager) return 0;
  ((void (*)(void*, SEL, void*))objc_msgSend)((void*)manager,
                                              sel_registerName("requestTrackingAuthorizationWithCompletionHandler:"),
                                              (void*)^(unsigned long status) { (void)status; });
  return 1;
}

/* The advertising id (IDFA) as text; all zeros when tracking is not authorized */
rae_String rae_ext_AppTracking_advertisingId(void) {
  Class manager = objc_getClass("ASIdentifierManager");
  void* shared = manager ? rae_app_tracking_send((void*)manager, "sharedManager") : NULL;
  void* identifier = shared ? rae_app_tracking_send(shared, "advertisingIdentifier") : NULL;
  void* text = identifier ? rae_app_tracking_send(identifier, "UUIDString") : NULL;
  const char* utf8 = text ? (const char*)rae_app_tracking_send(text, "UTF8String") : NULL;
  return rae_ext_rae_str_from_cstr(utf8 ? utf8 : "");
}

/* SKAdNetwork's coarse value constant: 0 low, 1 medium, 2 high */
static void* rae_app_tracking_coarse(int64_t coarse) {
  const char* name = coarse == 2 ? "SKAdNetworkCoarseConversionValueHigh"
                     : coarse == 1 ? "SKAdNetworkCoarseConversionValueMedium"
                                   : "SKAdNetworkCoarseConversionValueLow";
  void** constant = (void**)dlsym(RTLD_DEFAULT, name);
  return constant ? *constant : NULL;
}

/* One conversion value update: 3 when iOS took fine, coarse and the lock window
 * (16.1), 2 the fine value only (15.4), 1 the old conversion value (14.0), 0 none */
int64_t rae_ext_AppTracking_updateConversion(int64_t fine, int64_t coarse, rae_Bool lockWindow) {
  Class network = objc_getClass("SKAdNetwork");
  if (!network) return 0;
  void* coarseValue = rae_app_tracking_coarse(coarse);
  if (coarseValue &&
      rae_app_tracking_responds((void*)network, "updatePostbackConversionValue:coarseValue:lockWindow:completionHandler:")) {
    ((void (*)(void*, SEL, long, void*, signed char, void*))objc_msgSend)(
        (void*)network, sel_registerName("updatePostbackConversionValue:coarseValue:lockWindow:completionHandler:"),
        (long)fine, coarseValue, lockWindow ? 1 : 0, NULL);
    return 3;
  }
  if (rae_app_tracking_responds((void*)network, "updatePostbackConversionValue:completionHandler:")) {
    ((void (*)(void*, SEL, long, void*))objc_msgSend)(
        (void*)network, sel_registerName("updatePostbackConversionValue:completionHandler:"), (long)fine, NULL);
    return 2;
  }
  ((void (*)(void*, SEL, long))objc_msgSend)((void*)network, sel_registerName("updateConversionValue:"), (long)fine);
  return 1;
}
#endif /* iOS */

#endif /* RAE_RUNTIME_APP_TRACKING_APPLE_H */
