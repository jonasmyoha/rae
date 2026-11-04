/* A link that opens the app on iOS, for lib/AppLinks.
 *
 * Platform reason: UIKit hands a link to the scene delegate, and only there.
 * SDL's delegate has no scene:continueUserActivity: (a universal link while
 * the app runs), and at launch it passes links on as drop events before the
 * event queue exists, where they are lost. So this header adds that method
 * to SDL's delegate and wraps the other two: each only reads the URLs out and
 * pushes them (rae_app_links_push). A universal link is an NSUserActivity
 * with a webpageURL; a custom scheme is a UIOpenURLContext with a URL.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/AppLinks. Written through the Objective-C runtime, because this header
 * is C. Empty off iOS. */
#ifndef RAE_RUNTIME_APPLINKS_APPLE_H
#define RAE_RUNTIME_APPLINKS_APPLE_H

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <objc/message.h>
#include <objc/runtime.h>

void rae_app_links_push(const char* url);
void rae_ext_EventLoop_wake(void);

static IMP rae_app_links_sdl_will_connect = NULL;
static IMP rae_app_links_sdl_open_urls = NULL;

static void* rae_app_links_send(void* object, const char* selector) {
  return ((void* (*)(void*, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static void rae_app_links_push_url(void* url) {
  if (!url) return;
  void* text = rae_app_links_send(url, "absoluteString");
  const char* utf8 = text ? (const char*)rae_app_links_send(text, "UTF8String") : NULL;
  if (utf8) rae_app_links_push(utf8);
}

/* Each object of the set, by its URL property `urlSelector` */
static void rae_app_links_push_set(void* set, const char* urlSelector) {
  if (!set) return;
  void* items = rae_app_links_send(set, "allObjects");
  unsigned long count = ((unsigned long (*)(void*, SEL))objc_msgSend)(items, sel_registerName("count"));
  for (unsigned long i = 0; i < count; i++) {
    void* item = ((void* (*)(void*, SEL, unsigned long))objc_msgSend)(items, sel_registerName("objectAtIndex:"), i);
    rae_app_links_push_url(rae_app_links_send(item, urlSelector));
  }
}

/* The links that launched the app. Its main has not started: no wake. */
static void rae_app_links_will_connect(void* self, SEL cmd, void* scene, void* session, void* options) {
  rae_app_links_push_set(rae_app_links_send(options, "userActivities"), "webpageURL");
  rae_app_links_push_set(rae_app_links_send(options, "URLContexts"), "URL");
  ((void (*)(void*, SEL, void*, void*, void*))rae_app_links_sdl_will_connect)(self, cmd, scene, session, options);
}

/* A custom scheme link while the app runs */
static void rae_app_links_open_urls(void* self, SEL cmd, void* scene, void* contexts) {
  rae_app_links_push_set(contexts, "URL");
  rae_ext_EventLoop_wake();
  ((void (*)(void*, SEL, void*, void*))rae_app_links_sdl_open_urls)(self, cmd, scene, contexts);
}

/* A universal link while the app runs */
static void rae_app_links_continue_activity(void* self, SEL cmd, void* scene, void* activity) {
  (void)self; (void)cmd; (void)scene;
  rae_app_links_push_url(rae_app_links_send(activity, "webpageURL"));
  rae_ext_EventLoop_wake();
}

__attribute__((constructor)) static void rae_app_links_install(void) {
  Class delegate = objc_getClass("SDLUIKitSceneDelegate");
  if (!delegate) return;
  Method willConnect = class_getInstanceMethod(delegate, sel_registerName("scene:willConnectToSession:options:"));
  Method openUrls = class_getInstanceMethod(delegate, sel_registerName("scene:openURLContexts:"));
  if (!willConnect || !openUrls) return;
  rae_app_links_sdl_will_connect = method_setImplementation(willConnect, (IMP)rae_app_links_will_connect);
  rae_app_links_sdl_open_urls = method_setImplementation(openUrls, (IMP)rae_app_links_open_urls);
  class_addMethod(delegate, sel_registerName("scene:continueUserActivity:"),
                  (IMP)rae_app_links_continue_activity, "v@:@@");
}
#endif /* iOS */

#endif
