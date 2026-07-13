/* Apple-specific platform bridge for App Nap and foreground activation. Objective-C/Cocoa ABI boundary stays C.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

#if defined(__APPLE__)
/* App Nap opt-out. macOS throttles event delivery to processes
 * with low idle CPU once App Nap engages — observed in the field
 * via `sample <pid>` showing RunningBoardServices'
 * `rbs_acquire_appnap_assertion` on the main run loop. Throttled
 * apps still wake on bursts (mouse drag, scroll) but coalesce
 * single discrete events for seconds, exactly the "stops accepting
 * input" pattern users hit.
 *
 * Fix: call `[[NSProcessInfo processInfo] beginActivityWithOptions:
 * reason:]` with `NSActivityUserInitiated` (0x00FFFFFFULL). The
 * returned activity object must be retained for the process
 * lifetime; we stash it in a static and never release.
 *
 * Implementation goes through `objc_msgSend` directly so we don't
 * need an Objective-C compilation unit — the runtime is already
 * linked via the Cocoa framework (raylib apps pull it in) and via
 * Foundation otherwise. Lives OUTSIDE the `RAE_HAS_RAYLIB` block
 * because it's process-level, not raylib-specific; the symbol has
 * to be defined in both the rae driver build and compiled-target
 * builds so `vm_raylib.c`'s native wrapper can link. */
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreFoundation/CoreFoundation.h>
static void* g_appnap_activity = NULL;
void rae_ext_disableAppNap(void) {
  if (g_appnap_activity != NULL) return;
  Class npClass = objc_getClass("NSProcessInfo");
  if (!npClass) {
    fprintf(stderr, "[app-nap] NSProcessInfo class not found\n");
    return;
  }
  SEL piSel = sel_registerName("processInfo");
  SEL beginSel = sel_registerName("beginActivityWithOptions:reason:");
  void* processInfo = ((void* (*)(void*, SEL))objc_msgSend)((void*)npClass, piSel);
  if (!processInfo) {
    fprintf(stderr, "[app-nap] processInfo nil\n");
    return;
  }
  uint64_t options = 0x00FFFFFFULL;
  CFStringRef reason = CFSTR("Rae interactive UI loop");
  void* activity = ((void* (*)(void*, SEL, uint64_t, void*))objc_msgSend)(
    processInfo, beginSel, options, (void*)reason);
  if (activity) {
    SEL retainSel = sel_registerName("retain");
    ((void* (*)(void*, SEL))objc_msgSend)(activity, retainSel);
    g_appnap_activity = activity;
    fprintf(stderr, "[app-nap] disabled (activity=%p)\n", activity);
  } else {
    fprintf(stderr, "[app-nap] beginActivity returned nil\n");
  }
  fflush(stderr);
}

/* Bring THIS process's window to the foreground:
 * `[[NSApplication sharedApplication] activateIgnoringOtherApps:YES]`.
 *
 * Used after controlling Spotify (which can pull Spotify forward) so
 * focus returns to our own window and clicks keep landing. We activate
 * OURSELVES rather than "whatever was frontmost before the play": the
 * latter (via System Events `set frontmost`) reliably hands focus to
 * the launching app — SUMU when run from the dev tools — which drops us
 * out of the foreground and macOS then demotes us to a background
 * process (no Dock entry, unfocusable, dead close button). Activating
 * self has no such failure mode. Pure objc_msgSend; no ObjC unit. */
void rae_ext_activateSelf(void) {
  Class appCls = objc_getClass("NSApplication");
  if (!appCls) return;
  SEL sharedSel = sel_registerName("sharedApplication");
  void* app = ((void* (*)(Class, SEL))objc_msgSend)(appCls, sharedSel);
  if (!app) return;
  SEL actSel = sel_registerName("activateIgnoringOtherApps:");
  ((void (*)(void*, SEL, signed char))objc_msgSend)(app, actSel, (signed char)1);
}
#else
void rae_ext_disableAppNap(void) {
  /* No-op outside macOS — App Nap is a macOS-specific power
   * management feature. */
}
void rae_ext_activateSelf(void) {
  /* No-op outside macOS. */
}
#endif
