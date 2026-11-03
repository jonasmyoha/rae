/* The battery for lib/Battery: UIDevice on iOS, IOKit power sources on macOS.
 *
 * Platform reason: on iOS, UIDevice calls through the Objective-C runtime
 * (setBatteryMonitoringEnabled:, batteryLevel, batteryState) and its change
 * notifications; on macOS, IOKit's power-source calls (IOPSCopyPowerSourcesInfo
 * and its CoreFoundation dictionaries, which Rae cannot read) and its change
 * notification. Each function makes one such call and answers the platform's
 * raw value; what a value means is Rae's (lib/Battery). The change
 * notifications only count and wake the frame loop (rae_ext_EventLoop_wake).
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/Battery; the compiler then links -framework IOKit on macOS. Empty off
 * Apple. */
#ifndef RAE_RUNTIME_BATTERY_APPLE_H
#define RAE_RUNTIME_BATTERY_APPLE_H

#include <stdint.h>

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>

void rae_ext_EventLoop_wake(void);

static volatile int64_t rae_battery_changes = 0;
static int rae_battery_watching = 0;

#if TARGET_OS_IOS
#include <objc/message.h>
#include <objc/runtime.h>

static void* rae_battery_device(void) {
  Class deviceClass = objc_getClass("UIDevice");
  if (!deviceClass) return NULL;
  void* device = ((void* (*)(void*, SEL))objc_msgSend)((void*)deviceClass, sel_registerName("currentDevice"));
  /* monitoring is off by default; batteryLevel reads -1 until it is on */
  ((void (*)(void*, SEL, signed char))objc_msgSend)(device, sel_registerName("setBatteryMonitoringEnabled:"), 1);
  return device;
}

static void rae_battery_changed(CFNotificationCenterRef center, void* observer, CFNotificationName name,
                                const void* object, CFDictionaryRef info) {
  (void)center; (void)observer; (void)name; (void)object; (void)info;
  rae_battery_changes++;
  rae_ext_EventLoop_wake();
}

/* The level as UIDevice reports it, 0..1, times 1000; -1000 unknown */
int64_t rae_ext_Battery_rawLevel(void) {
  void* device = rae_battery_device();
  if (!device) return -1000;
  float level = ((float (*)(void*, SEL))objc_msgSend)(device, sel_registerName("batteryLevel"));
  return (int64_t)(level * 1000.0f + (level < 0.0f ? -0.5f : 0.5f));
}

/* UIDeviceBatteryState: 0 unknown, 1 unplugged, 2 charging, 3 full */
int64_t rae_ext_Battery_rawState(void) {
  void* device = rae_battery_device();
  if (!device) return 0;
  return (int64_t)((long (*)(void*, SEL))objc_msgSend)(device, sel_registerName("batteryState"));
}

/* How many battery notifications have arrived; the first call starts
 * listening to UIDeviceBatteryLevelDidChange / StateDidChange */
int64_t rae_ext_Battery_changes(void) {
  if (!rae_battery_watching) {
    rae_battery_watching = 1;
    (void)rae_battery_device();
    CFNotificationCenterRef center = CFNotificationCenterGetLocalCenter();
    CFNotificationCenterAddObserver(center, NULL, rae_battery_changed, CFSTR("UIDeviceBatteryLevelDidChangeNotification"),
                                    NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(center, NULL, rae_battery_changed, CFSTR("UIDeviceBatteryStateDidChangeNotification"),
                                    NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
  }
  return rae_battery_changes;
}

#elif TARGET_OS_OSX
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

/* The internal battery's description (a copy the caller releases), NULL for
 * a Mac without one */
static CFDictionaryRef rae_battery_description(void) {
  CFTypeRef info = IOPSCopyPowerSourcesInfo();
  if (!info) return NULL;
  CFArrayRef sources = IOPSCopyPowerSourcesList(info);
  CFDictionaryRef found = NULL;
  for (CFIndex i = 0; sources && i < CFArrayGetCount(sources) && !found; i++) {
    CFDictionaryRef description = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(sources, i));
    if (!description) continue;
    CFStringRef type = (CFStringRef)CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey));
    if (type && CFEqual(type, CFSTR(kIOPSInternalBatteryType))) found = CFDictionaryCreateCopy(NULL, description);
  }
  if (sources) CFRelease(sources);
  CFRelease(info);
  return found;
}

static int rae_battery_number(CFDictionaryRef description, const char* key) {
  CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
  CFNumberRef number = (CFNumberRef)CFDictionaryGetValue(description, name);
  CFRelease(name);
  int value = -1;
  if (number && CFGetTypeID(number) == CFNumberGetTypeID()) CFNumberGetValue(number, kCFNumberIntType, &value);
  return value;
}

static int rae_battery_flag(CFDictionaryRef description, const char* key) {
  CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
  CFTypeRef value = CFDictionaryGetValue(description, name);
  CFRelease(name);
  return value == kCFBooleanTrue;
}

static void rae_battery_changed(void* context) {
  (void)context;
  rae_battery_changes++;
  rae_ext_EventLoop_wake();
}

/* The internal battery's charge in thousandths of its capacity (current *
 * 1000 / max); -1000 when the Mac has no battery */
int64_t rae_ext_Battery_rawLevel(void) {
  CFDictionaryRef description = rae_battery_description();
  if (!description) return -1000;
  int current = rae_battery_number(description, kIOPSCurrentCapacityKey);
  int maximum = rae_battery_number(description, kIOPSMaxCapacityKey);
  CFRelease(description);
  if (current < 0 || maximum <= 0) return -1000;
  return (int64_t)current * 1000 / maximum;
}

/* The internal battery's state as bits: 1 present, 2 charging, 4 charged,
 * 8 on AC power; 0 for a Mac without one */
int64_t rae_ext_Battery_rawState(void) {
  CFDictionaryRef description = rae_battery_description();
  if (!description) return 0;
  int64_t bits = 1;
  if (rae_battery_flag(description, kIOPSIsChargingKey)) bits |= 2;
  if (rae_battery_flag(description, kIOPSIsChargedKey)) bits |= 4;
  CFStringRef name = CFStringCreateWithCString(NULL, kIOPSPowerSourceStateKey, kCFStringEncodingUTF8);
  CFStringRef state = (CFStringRef)CFDictionaryGetValue(description, name);
  CFRelease(name);
  if (state && CFEqual(state, CFSTR(kIOPSACPowerValue))) bits |= 8;
  CFRelease(description);
  return bits;
}

/* How many power-source notifications have arrived; the first call starts
 * listening (IOPSNotificationCreateRunLoopSource on the main run loop) */
int64_t rae_ext_Battery_changes(void) {
  if (!rae_battery_watching) {
    rae_battery_watching = 1;
    CFRunLoopSourceRef source = IOPSNotificationCreateRunLoopSource(rae_battery_changed, NULL);
    if (source) {
      CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
      CFRelease(source);
    }
  }
  return rae_battery_changes;
}
#endif
#endif

#endif
