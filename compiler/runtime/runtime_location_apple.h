/* Where the device is, for lib/Location: Core Location on iOS and macOS.
 *
 * Platform reason: CLLocationManager calls, made through the Objective-C
 * runtime (this header is C), and its delegate, the only way Core Location
 * answers. Each rae_ext_Location_* function makes ONE call on the manager
 * (or reads what the delegate recorded); the manager and its delegate are
 * made once and kept, because Core Location answers through them later. The
 * delegate only records the answer (a fix, or an error code) and wakes the
 * frame loop (rae_ext_EventLoop_wake); asking, the timeout and what an error
 * means are Rae's (lib/Location).
 *
 * The manager is made on the thread of the first call, the frame loop's
 * (main) thread, whose run loop delivers the delegate's calls; a program
 * that never runs one (a command line tool) never hears back and times out.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/Location; the compiler then links -framework CoreLocation. Empty off
 * Apple. */
#ifndef RAE_RUNTIME_LOCATION_APPLE_H
#define RAE_RUNTIME_LOCATION_APPLE_H

#include <stdint.h>

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <objc/message.h>
#include <objc/runtime.h>

void rae_ext_EventLoop_wake(void);

typedef struct { double latitude, longitude; } RaeLocationCoordinate;

static void* rae_location_manager = NULL;
/* What the delegate recorded: 0 nothing yet, 1 a fix, 2 an error */
static volatile int rae_location_state = 0;
static double rae_location_fix[4];  /* latitude, longitude, accuracy (m), time (ms since 1970) */
static volatile int64_t rae_location_error = 0;
static volatile int64_t rae_location_authorization_changes = 0;

static void rae_location_did_update(void* self, SEL cmd, void* manager, void* locations) {
  (void)self; (void)cmd; (void)manager;
  void* location = ((void* (*)(void*, SEL))objc_msgSend)(locations, sel_registerName("lastObject"));
  if (!location) return;
  RaeLocationCoordinate at =
      ((RaeLocationCoordinate(*)(void*, SEL))objc_msgSend)(location, sel_registerName("coordinate"));
  double accuracy = ((double (*)(void*, SEL))objc_msgSend)(location, sel_registerName("horizontalAccuracy"));
  void* stamp = ((void* (*)(void*, SEL))objc_msgSend)(location, sel_registerName("timestamp"));
  double seconds = stamp ? ((double (*)(void*, SEL))objc_msgSend)(stamp, sel_registerName("timeIntervalSince1970")) : 0.0;
  rae_location_fix[0] = at.latitude;
  rae_location_fix[1] = at.longitude;
  rae_location_fix[2] = accuracy;
  rae_location_fix[3] = seconds * 1000.0;
  rae_location_state = 1;
  rae_ext_EventLoop_wake();
}

static void rae_location_did_fail(void* self, SEL cmd, void* manager, void* error) {
  (void)self; (void)cmd; (void)manager;
  rae_location_error = error ? (int64_t)((long (*)(void*, SEL))objc_msgSend)(error, sel_registerName("code")) : -1;
  rae_location_state = 2;
  rae_ext_EventLoop_wake();
}

static void rae_location_did_change_authorization(void* self, SEL cmd, void* manager) {
  (void)self; (void)cmd; (void)manager;
  rae_location_authorization_changes++;
  rae_ext_EventLoop_wake();
}

/* The manager with its delegate, made on first use; NULL without Core Location */
static void* rae_location_get_manager(void) {
  if (rae_location_manager) return rae_location_manager;
  Class managerClass = objc_getClass("CLLocationManager");
  if (!managerClass) return NULL;
  Class delegateClass = objc_getClass("RaeLocationDelegate");
  if (!delegateClass) {
    delegateClass = objc_allocateClassPair(objc_getClass("NSObject"), "RaeLocationDelegate", 0);
    class_addMethod(delegateClass, sel_registerName("locationManager:didUpdateLocations:"),
                    (IMP)rae_location_did_update, "v@:@@");
    class_addMethod(delegateClass, sel_registerName("locationManager:didFailWithError:"),
                    (IMP)rae_location_did_fail, "v@:@@");
    class_addMethod(delegateClass, sel_registerName("locationManagerDidChangeAuthorization:"),
                    (IMP)rae_location_did_change_authorization, "v@:@");
    Protocol* protocol = objc_getProtocol("CLLocationManagerDelegate");
    if (protocol) class_addProtocol(delegateClass, protocol);
    objc_registerClassPair(delegateClass);
  }
  void* delegate = ((void* (*)(void*, SEL))objc_msgSend)(
      ((void* (*)(void*, SEL))objc_msgSend)((void*)delegateClass, sel_registerName("alloc")), sel_registerName("init"));
  void* manager = ((void* (*)(void*, SEL))objc_msgSend)(
      ((void* (*)(void*, SEL))objc_msgSend)((void*)managerClass, sel_registerName("alloc")), sel_registerName("init"));
  /* the manager holds its delegate weakly: the delegate's +1 is never released */
  ((void (*)(void*, SEL, void*))objc_msgSend)(manager, sel_registerName("setDelegate:"), delegate);
  rae_location_manager = manager;
  return manager;
}

/* CLAuthorizationStatus: 0 not determined, 1 restricted, 2 denied, 3 always,
 * 4 when in use; -1 without Core Location */
int64_t rae_ext_Location_authorization(void) {
  void* manager = rae_location_get_manager();
  if (!manager) return -1;
  return (int64_t)((int (*)(void*, SEL))objc_msgSend)(manager, sel_registerName("authorizationStatus"));
}

/* How many times the authorization changed (the delegate's count) */
int64_t rae_ext_Location_authorizationChanges(void) { return rae_location_authorization_changes; }

/* Whether Location Services are on for the whole device (asked only to tell
 * "off" from "denied": both read as denied) */
int64_t rae_ext_Location_servicesEnabled(void) {
  Class managerClass = objc_getClass("CLLocationManager");
  if (!managerClass) return 0;
  return ((signed char (*)(void*, SEL))objc_msgSend)((void*)managerClass, sel_registerName("locationServicesEnabled")) ? 1 : 0;
}

/* Ask the person for when-in-use access (the answer arrives as an
 * authorization change) */
int64_t rae_ext_Location_requestWhenInUse(void) {
  void* manager = rae_location_get_manager();
  if (!manager) return 0;
  ((void (*)(void*, SEL))objc_msgSend)(manager, sel_registerName("requestWhenInUseAuthorization"));
  return 1;
}

/* The accuracy to ask for, in meters (CLLocationAccuracy) */
int64_t rae_ext_Location_setDesiredAccuracy(double meters) {
  void* manager = rae_location_get_manager();
  if (!manager) return 0;
  ((void (*)(void*, SEL, double))objc_msgSend)(manager, sel_registerName("setDesiredAccuracy:"), meters);
  return 1;
}

/* Ask for the current place once; the answer arrives through the delegate */
int64_t rae_ext_Location_requestOnce(void) {
  void* manager = rae_location_get_manager();
  if (!manager) return 0;
  rae_location_state = 0;
  ((void (*)(void*, SEL))objc_msgSend)(manager, sel_registerName("requestLocation"));
  return 1;
}

/* Stop a request that is no longer wanted (a timeout) */
int64_t rae_ext_Location_stop(void) {
  void* manager = rae_location_get_manager();
  if (!manager) return 0;
  ((void (*)(void*, SEL))objc_msgSend)(manager, sel_registerName("stopUpdatingLocation"));
  return 1;
}

/* CLAccuracyAuthorization: 0 full, 1 reduced (the person's approximate
 * location choice) */
int64_t rae_ext_Location_accuracyReduced(void) {
  void* manager = rae_location_get_manager();
  if (!manager) return 0;
  return (int64_t)((long (*)(void*, SEL))objc_msgSend)(manager, sel_registerName("accuracyAuthorization"));
}

/* What the delegate recorded: 0 nothing yet, 1 a fix (read with
 * rae_ext_Location_fix), 2 an error (rae_ext_Location_errorCode) */
int64_t rae_ext_Location_state(void) { return rae_location_state; }

/* One number of the recorded fix: 0 latitude, 1 longitude, 2 accuracy in
 * meters, 3 its time in ms since 1970 */
double rae_ext_Location_fix(int64_t field) {
  if (field < 0 || field > 3) return 0.0;
  return rae_location_fix[field];
}

/* The recorded error's CLError code (0 location unknown, 1 denied, ...) */
int64_t rae_ext_Location_errorCode(void) { return rae_location_error; }
#endif

#endif
