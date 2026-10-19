/* Apple-specific platform bridge for App Nap and foreground activation. Objective-C/Cocoa ABI boundary stays C.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 *
 * Apple only: the Rae side declares these under `when Target.isApple`
 * (lib/app3d/RenderScale.rae, lib/sys/Spotify.rae, lib/Raylib.rae), so there
 * are no stubs elsewhere.
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
/* NSProcessInfoThermalState: 0 nominal, 1 fair, 2 serious, 3 critical. Lets the
 * render loop shed load (dynamic resolution / frame cap) BEFORE the OS throttles
 * the GPU, instead of only reacting to dropped frames (#530). macOS + iOS. */
int64_t rae_ext_thermalState(void) {
  Class npClass = objc_getClass("NSProcessInfo");
  if (!npClass) return 0;
  void* pi = ((void* (*)(void*, SEL))objc_msgSend)((void*)npClass, sel_registerName("processInfo"));
  if (!pi) return 0;
  return (int64_t)((long (*)(void*, SEL))objc_msgSend)(pi, sel_registerName("thermalState"));
}

/* ---- the layer the window surface presents into (macOS) ----------------
 *
 * WHY THIS EXISTS: about half of the Rae windows on macOS stayed black from
 * startup, logging "[present] Occluded(196609)" every frame, until another
 * window covered them FULLY and was moved away again.
 *
 * wgpu 29's Metal backend (wgpu-native v29.0.1.1, wgpu PR #9141, the fix for
 * wgpu issue #8309) checks NSWindow.occlusionState before EVERY drawable
 * acquire, and returns Occluded without a drawable while the "visible" bit
 * is clear. That bit is clear for a window that has just been created
 * (measured: frame 1 always reads "not visible" until the app is activated),
 * and when it stays clear the window gets no frame at all. A full cover and
 * uncover is a real occlusion change, which is why only that brought the
 * window back. Upstream knows: wgpu issue #9430 ("First frame does not render
 * due to being somehow occluded"), fix PR #10302 (not merged as of
 * 2026-10-19).
 *
 * wgpu finds the window by walking up from the CAMetalLayer to the first
 * layer that has a delegate and asking that delegate for its `window`. So the
 * surface gets its own CAMetalLayer, a sublayer filling SDL's view layer,
 * whose delegate answers `window` with nil: wgpu then skips its check. Rae
 * does not lose the protection the check was for (a one-second stall in
 * nextDrawable while the window is really covered): the present already
 * refuses to acquire while SDL reports the window hidden, minimized or
 * occluded (rae_g2d_window_visible), and SDL's flag follows AppKit's
 * occlusion NOTIFICATIONS, so it never starts out wrong.
 *
 * RAE_WGPU_OCCLUSION_CHECK=1 keeps the old path (wgpu presents into SDL's
 * layer and checks occlusionState itself), to compare. */
#include <TargetConditionals.h>
#if TARGET_OS_OSX
typedef struct { double x, y, width, height; } RaeLayerRect;

/* The delegate's `window`: none */
static void* rae_layer_delegate_window(void* self, SEL cmd) { (void)self; (void)cmd; return NULL; }

/* The delegate's actionForLayer:forKey:: NSNull, so a resize sets the
 * layer's frame at once instead of animating it over 0.25 s */
static void* rae_layer_delegate_action(void* self, SEL cmd, void* layer, void* key) {
  (void)self; (void)cmd; (void)layer; (void)key;
  return ((void* (*)(void*, SEL))objc_msgSend)((void*)objc_getClass("NSNull"), sel_registerName("null"));
}

static void* rae_layer_delegate(void) {
  static void* delegate = NULL;
  if (delegate) return delegate;
  Class cls = objc_getClass("RaeSurfaceLayerDelegate");
  if (!cls) {
    cls = objc_allocateClassPair(objc_getClass("NSObject"), "RaeSurfaceLayerDelegate", 0);
    class_addMethod(cls, sel_registerName("window"), (IMP)rae_layer_delegate_window, "@@:");
    class_addMethod(cls, sel_registerName("actionForLayer:forKey:"), (IMP)rae_layer_delegate_action, "@@:@@");
    objc_registerClassPair(cls);
  }
  void* instance = ((void* (*)(void*, SEL))objc_msgSend)((void*)cls, sel_registerName("alloc"));
  delegate = ((void* (*)(void*, SEL))objc_msgSend)(instance, sel_registerName("init"));
  return delegate;
}

/* Fit the presentation layer to its parent: `width` x `height` points, the
 * parent's pixel scale */
void rae_presentation_layer_fit(void* layer, int width, int height) {
  if (!layer) return;
  void* parent = ((void* (*)(void*, SEL))objc_msgSend)(layer, sel_registerName("superlayer"));
  if (parent) {
    double scale = ((double (*)(void*, SEL))objc_msgSend)(parent, sel_registerName("contentsScale"));
    ((void (*)(void*, SEL, double))objc_msgSend)(layer, sel_registerName("setContentsScale:"), scale);
  }
  RaeLayerRect frame = { 0.0, 0.0, (double)width, (double)height };
  ((void (*)(void*, SEL, RaeLayerRect))objc_msgSend)(layer, sel_registerName("setFrame:"), frame);
}

/* The layer the surface presents into: a new CAMetalLayer filling
 * `sdlLayer` (SDL's view layer), its delegate the windowless one above; or
 * `sdlLayer` itself under RAE_WGPU_OCCLUSION_CHECK=1 */
void* rae_presentation_layer(void* sdlLayer, int width, int height) {
  const char* keep = getenv("RAE_WGPU_OCCLUSION_CHECK");
  if (!sdlLayer || (keep && keep[0] == '1')) return sdlLayer;
  Class metalLayer = objc_getClass("CAMetalLayer");
  if (!metalLayer) return sdlLayer;
  /* alloc/init, not +layer: there may be no autorelease pool this early */
  void* layer = ((void* (*)(void*, SEL))objc_msgSend)((void*)metalLayer, sel_registerName("alloc"));
  layer = ((void* (*)(void*, SEL))objc_msgSend)(layer, sel_registerName("init"));
  if (!layer) return sdlLayer;
  /* The sublayer only ever shows the drawables Rae presents */
  ((void (*)(void*, SEL, signed char))objc_msgSend)(layer, sel_registerName("setOpaque:"), (signed char)1);
  /* kCALayerWidthSizable | kCALayerHeightSizable: follow the parent during a
   * live resize too; rae_presentation_layer_fit sets it exactly on configure */
  ((void (*)(void*, SEL, unsigned int))objc_msgSend)(layer, sel_registerName("setAutoresizingMask:"), 2u | 16u);
  ((void (*)(void*, SEL, void*))objc_msgSend)(layer, sel_registerName("setDelegate:"), rae_layer_delegate());
  ((void (*)(void*, SEL, void*))objc_msgSend)(sdlLayer, sel_registerName("addSublayer:"), layer);
  /* The parent holds it from here; it goes with SDL's view */
  ((void (*)(void*, SEL))objc_msgSend)(layer, sel_registerName("release"));
  rae_presentation_layer_fit(layer, width, height);
  return layer;
}
#endif  /* TARGET_OS_OSX */
#endif  /* __APPLE__ */
