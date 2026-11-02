/* Haptic feedback for lib/Haptics on iOS: UIImpactFeedbackGenerator,
 * UISelectionFeedbackGenerator and UINotificationFeedbackGenerator.
 *
 * Platform reason: UIKit calls, made through the Objective-C runtime (this
 * header is C). Each function makes ONE call on a generator: impactOccurred,
 * selectionChanged, notificationOccurred: or prepare. The generator is a
 * UIKit object that must outlive the call (prepare warms the Taptic Engine
 * for the next one), so it is made once per kind and kept; that is the only
 * state. When to prepare, which kind to play and what a headless run does are
 * Rae's (lib/Haptics). UIKit wants the main thread, where an SDL app's frame
 * loop runs.
 *
 * Generator slots: 0, 1, 2 the light, medium and heavy impact (the
 * UIImpactFeedbackStyle values), 3 selection, 4 notification. Every function
 * answers 1 when the call was made, 0 when the class is missing.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/Haptics; empty on every target but iOS. */
#ifndef RAE_RUNTIME_HAPTICS_APPLE_H
#define RAE_RUNTIME_HAPTICS_APPLE_H

#include <stdint.h>

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS
#include <objc/message.h>
#include <objc/runtime.h>

static void* rae_haptics_generators[5];

/* The generator of `slot`, made on first use */
static void* rae_haptics_generator(int64_t slot) {
  if (slot < 0 || slot > 4) return NULL;
  if (rae_haptics_generators[slot]) return rae_haptics_generators[slot];
  const char* name = slot < 3 ? "UIImpactFeedbackGenerator"
                     : slot == 3 ? "UISelectionFeedbackGenerator"
                                 : "UINotificationFeedbackGenerator";
  Class kind = objc_getClass(name);
  if (!kind) return NULL;
  void* made = ((void* (*)(void*, SEL))objc_msgSend)((void*)kind, sel_registerName("alloc"));
  if (slot < 3) {
    made = ((void* (*)(void*, SEL, long))objc_msgSend)(made, sel_registerName("initWithStyle:"), (long)slot);
  } else {
    made = ((void* (*)(void*, SEL))objc_msgSend)(made, sel_registerName("init"));
  }
  rae_haptics_generators[slot] = made;
  return made;
}

/* Warm the Taptic Engine for the next feedback of `slot` */
int64_t rae_ext_Haptics_prepare(int64_t slot) {
  void* generator = rae_haptics_generator(slot);
  if (!generator) return 0;
  ((void (*)(void*, SEL))objc_msgSend)(generator, sel_registerName("prepare"));
  return 1;
}

/* An impact: style 0 light, 1 medium, 2 heavy */
int64_t rae_ext_Haptics_impact(int64_t style) {
  void* generator = rae_haptics_generator(style);
  if (!generator) return 0;
  ((void (*)(void*, SEL))objc_msgSend)(generator, sel_registerName("impactOccurred"));
  return 1;
}

/* The tick of a selection that moved one step */
int64_t rae_ext_Haptics_selection(void) {
  void* generator = rae_haptics_generator(3);
  if (!generator) return 0;
  ((void (*)(void*, SEL))objc_msgSend)(generator, sel_registerName("selectionChanged"));
  return 1;
}

/* A notification: kind 0 success, 1 warning, 2 error (UINotificationFeedbackType) */
int64_t rae_ext_Haptics_notify(int64_t kind) {
  void* generator = rae_haptics_generator(4);
  if (!generator) return 0;
  ((void (*)(void*, SEL, long))objc_msgSend)(generator, sel_registerName("notificationOccurred:"), (long)kind);
  return 1;
}
#endif

#endif
