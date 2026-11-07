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
 *   link is on this thread's run loop, the render thread's), at most 34 ms;
 *   then, or with no link, the layer's own nextDrawable answers.
 * - RAE_METAL_DISPLAY_LINK=0 turns it off, for comparisons. */
#if defined(__APPLE__) && TARGET_OS_IOS && defined(__arm64__)
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>

typedef struct { float minimum, maximum, preferred; } RaeDisplayLinkRange;

static void* g_g2d_display_link = NULL;
static void* g_g2d_display_link_drawable = NULL;
static int g_g2d_display_link_unused = 0;
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

/* -[RaeLinkedMetalLayer nextDrawable] */
static void* rae_g2d_linked_next_drawable(void* self, SEL cmd) {
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
        if (drawable) return rae_g2d_send(drawable, "autorelease");
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
    if (!layer || (setting && strcmp(setting, "0") == 0)) return;
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
    RaeDisplayLinkRange range = { rate, rate, rate };
    ((void (*)(void*, SEL, RaeDisplayLinkRange))objc_msgSend)(link, sel_registerName("setPreferredFrameRateRange:"), range);
    ((void (*)(void*, SEL, float))objc_msgSend)(link, sel_registerName("setPreferredFrameLatency:"), 1.0f);
    void* runLoop = rae_g2d_send((void*)objc_getClass("NSRunLoop"), "currentRunLoop");
    ((void (*)(void*, SEL, void*, const void*))objc_msgSend)(link, sel_registerName("addToRunLoop:forMode:"), runLoop, CFSTR("kCFRunLoopDefaultMode"));
    g_g2d_display_link = link;
}
#endif
