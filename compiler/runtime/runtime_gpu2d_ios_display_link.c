/* iOS frame pacing: the window's drawables come from a CAMetalDisplayLink
 * (iOS 17), which sets the frame rate and the frame latency of the layer
 * (docs/ui-animation-design.md section 10, "A CAMetalDisplayLink holds
 * 120 Hz"). Without it iOS keeps a longer latency for the layer in some
 * animations (a frame shown about 23 ms after it is handed over), and the
 * layer's 3 drawables then give 80-90 frames a second on a 120 Hz screen.
 *
 * Platform reason: wgpu-native takes its drawable with -[CAMetalLayer
 * nextDrawable] and has no way to be handed one, so the one layer SDL made
 * gets a runtime subclass whose nextDrawable returns the display link's
 * drawable (the per-object technique key-value observing uses; no system
 * class is changed). The link is the delegate's to feed: its update callback
 * keeps the drawable it hands out until nextDrawable takes it.
 *
 * - wgpuSurfaceConfigure aborts while a display link is attached to the
 *   layer, so the link stops before every configure and starts after it.
 * - A drawable no frame takes is given back. After 6 such updates in a row
 *   the link pauses, so an app that is not drawing is not woken 120 times a
 *   second; the next nextDrawable starts it again.
 * - nextDrawable waits for the link's update by running the run loop (the
 *   link is on this thread's run loop, the render thread's), at most 34 ms,
 *   and then answers nil (wgpu: a Timeout status, the frame skipped). It
 *   never asks the layer itself while the link is attached: that raised an
 *   Objective-C exception inside wgpu's acquire whenever a slow frame missed
 *   the wait, which wgpu cannot unwind through, and the app aborted.
 * - An app slower than the screen (a 3D game at 60 fps on a 120 Hz screen)
 *   only loses by the link: it hands out a fresh drawable every update and
 *   keeps the newest, so with the app's frames in flight the layer's 3
 *   drawables run out and the rate halves. When, over 120 drawables, the app
 *   asked for one on average more than 1.6 screen intervals apart, the link is
 *   stopped for good and the layer's own nextDrawable answers from then on.
 *   Gaps over 100 ms (an idle app, which pauses the link instead) do not
 *   count. (Counting the drawables the link hands out did not work: under
 *   load its updates come only as fast as drawables free up.)
 * - RAE_METAL_DISPLAY_LINK=0 turns it off, for comparisons; =keep never stops
 *   it for a slow app. */
#if defined(__APPLE__) && TARGET_OS_IOS && defined(__arm64__)
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>

typedef struct { float minimum, maximum, preferred; } RaeDisplayLinkRange;

static void* g_g2d_display_link = NULL;
static void* g_g2d_display_link_drawable = NULL;
static int g_g2d_display_link_unused = 0;
/* For the slow-app check: the screen's interval (s), the last request's time,
 * the requests and their summed gaps since the last check, and whether the
 * link has been given up for good */
static double g_g2d_display_link_interval = 1.0 / 60.0;
static double g_g2d_display_link_last_request = 0.0;
static int g_g2d_display_link_requests = 0;
static double g_g2d_display_link_gaps = 0.0;
static int g_g2d_display_link_given_up = 0;
static IMP g_g2d_layer_next_drawable = NULL;

static void* rae_g2d_send(void* target, const char* selector) {
    return ((void* (*)(void*, SEL))objc_msgSend)(target, sel_registerName(selector));
}

static void rae_g2d_display_link_drop_drawable(void) {
    if (g_g2d_display_link_drawable) rae_g2d_send(g_g2d_display_link_drawable, "release");
    g_g2d_display_link_drawable = NULL;
}

/* -[RaeDisplayLinkDelegate metalDisplayLink:needsUpdate:] */
static void rae_g2d_display_link_update(void* self, SEL cmd, void* link, void* update) {
    (void)self; (void)cmd;
    if (g_g2d_display_link_drawable) {
        rae_g2d_display_link_drop_drawable();
        g_g2d_display_link_unused++;
        if (g_g2d_display_link_unused >= 6) {
            ((void (*)(void*, SEL, signed char))objc_msgSend)(link, sel_registerName("setPaused:"), 1);
            return;
        }
    } else {
        g_g2d_display_link_unused = 0;
    }
    void* drawable = rae_g2d_send(update, "drawable");
    if (drawable) g_g2d_display_link_drawable = rae_g2d_send(drawable, "retain");
}

static void rae_g2d_display_link_stop(void);

/* Has the app, over the last window of running updates, taken too few of the
 * link's drawables to gain by it? */
static int rae_g2d_display_link_app_is_slow(void) {
    const char* setting = getenv("RAE_METAL_DISPLAY_LINK");
    if (setting && strcmp(setting, "keep") == 0) return 0;
    double now = CFAbsoluteTimeGetCurrent();
    double gap = now - g_g2d_display_link_last_request;
    g_g2d_display_link_last_request = now;
    if (gap <= 0.0 || gap > 0.1) return 0;
    g_g2d_display_link_requests++;
    g_g2d_display_link_gaps += gap;
    if (g_g2d_display_link_requests < 120) return 0;
    int slow = g_g2d_display_link_gaps / g_g2d_display_link_requests > 1.6 * g_g2d_display_link_interval;
    g_g2d_display_link_requests = 0;
    g_g2d_display_link_gaps = 0.0;
    return slow;
}

/* -[RaeLinkedMetalLayer nextDrawable] */
static void* rae_g2d_linked_next_drawable(void* self, SEL cmd) {
    if (g_g2d_display_link && rae_g2d_display_link_app_is_slow()) {
        rae_g2d_display_link_stop();
        g_g2d_display_link_given_up = 1;
        fprintf(stderr, "[display link] the app is slower than the screen: drawables from the layer from now on\n");
    }
    if (g_g2d_display_link) {
        if (((signed char (*)(void*, SEL))objc_msgSend)(g_g2d_display_link, sel_registerName("isPaused"))) {
            g_g2d_display_link_unused = 0;
            ((void (*)(void*, SEL, signed char))objc_msgSend)(g_g2d_display_link, sel_registerName("setPaused:"), 0);
        }
        for (int i = 0; i < 34 && !g_g2d_display_link_drawable; i++) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
        }
        void* drawable = g_g2d_display_link_drawable;
        g_g2d_display_link_drawable = NULL;
        if (drawable) {
            return rae_g2d_send(drawable, "autorelease");
        }
        /* never the layer's own while the link is attached */
        return NULL;
    }
    return ((void* (*)(void*, SEL))g_g2d_layer_next_drawable)(self, cmd);
}

/* Before wgpuSurfaceConfigure */
static void rae_g2d_display_link_stop(void) {
    if (!g_g2d_display_link) return;
    rae_g2d_send(g_g2d_display_link, "invalidate");
    rae_g2d_send(g_g2d_display_link, "release");
    g_g2d_display_link = NULL;
    rae_g2d_display_link_drop_drawable();
}

/* After wgpuSurfaceConfigure: give the layer its subclass once, and a new link */
static void rae_g2d_display_link_start(void* layer) {
    const char* setting = getenv("RAE_METAL_DISPLAY_LINK");
    if (!layer || (setting && strcmp(setting, "0") == 0) || g_g2d_display_link_given_up) return;
    Class linkClass = objc_getClass("CAMetalDisplayLink");
    if (!linkClass) return;
    static Class delegateClass = NULL;
    static void* delegate = NULL;
    if (!delegateClass) {
        delegateClass = objc_allocateClassPair(objc_getClass("NSObject"), "RaeDisplayLinkDelegate", 0);
        class_addMethod(delegateClass, sel_registerName("metalDisplayLink:needsUpdate:"), (IMP)rae_g2d_display_link_update, "v@:@@");
        objc_registerClassPair(delegateClass);
        delegate = rae_g2d_send(rae_g2d_send((void*)delegateClass, "alloc"), "init");
    }
    Class layerClass = object_getClass((id)layer);
    if (strcmp(class_getName(layerClass), "RaeLinkedMetalLayer") != 0) {
        Class linked = objc_getClass("RaeLinkedMetalLayer");
        if (!linked) {
            linked = objc_allocateClassPair(layerClass, "RaeLinkedMetalLayer", 0);
            Method next = class_getInstanceMethod(layerClass, sel_registerName("nextDrawable"));
            class_addMethod(linked, sel_registerName("nextDrawable"), (IMP)rae_g2d_linked_next_drawable, method_getTypeEncoding(next));
            objc_registerClassPair(linked);
            g_g2d_layer_next_drawable = method_getImplementation(next);
        }
        object_setClass((id)layer, linked);
    }
    void* link = rae_g2d_send((void*)linkClass, "alloc");
    link = ((void* (*)(void*, SEL, void*))objc_msgSend)(link, sel_registerName("initWithMetalLayer:"), layer);
    if (!link) return;
    ((void (*)(void*, SEL, void*))objc_msgSend)(link, sel_registerName("setDelegate:"), delegate);
    void* screen = rae_g2d_send((void*)objc_getClass("UIScreen"), "mainScreen");
    long fastest = ((long (*)(void*, SEL))objc_msgSend)(screen, sel_registerName("maximumFramesPerSecond"));
    float rate = fastest > 0 ? (float)fastest : 60.0f;
    g_g2d_display_link_interval = 1.0 / rate;
    RaeDisplayLinkRange range = { rate, rate, rate };
    ((void (*)(void*, SEL, RaeDisplayLinkRange))objc_msgSend)(link, sel_registerName("setPreferredFrameRateRange:"), range);
    ((void (*)(void*, SEL, float))objc_msgSend)(link, sel_registerName("setPreferredFrameLatency:"), 1.0f);
    void* runLoop = rae_g2d_send((void*)objc_getClass("NSRunLoop"), "currentRunLoop");
    ((void (*)(void*, SEL, void*, const void*))objc_msgSend)(link, sel_registerName("addToRunLoop:forMode:"), runLoop, CFSTR("kCFRunLoopDefaultMode"));
    g_g2d_display_link = link;
}
#endif
