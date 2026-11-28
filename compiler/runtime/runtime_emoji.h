/* Color emoji for lib/Gpu2dEmoji (docs/text-emoji.md): one emoji cluster (a grapheme, "🔥",
 * "👍🏽", "🇫🇮", a ZWJ family) drawn by the system's color emoji font into a square RGBA bitmap.
 *
 * Platform reason: the emoji font and its drawing are the platform's: on Apple CoreText draws
 * Apple Color Emoji's bitmaps, sequences and ligatures; on Android the Java text stack draws the
 * system's emoji font (tools/android/java/org/rae/app/RaeEmoji.java), reached through JNI; in
 * the browser the page's 2D canvas. Those fonts are not ours to parse. The C draws the cluster centered in the square and returns; the
 * raster size, the cache, the straight alpha and every fallback are Rae's.
 *
 * CoreText is loaded when first needed (dlopen), so a program links no new framework: its few
 * functions are looked up once.
 *
 * In the browser the page's text stack draws it (a 2D canvas's fillText, read back), and the
 * bytes are straight alpha (the function says 2 instead of 1). On Linux the function says 0 and
 * Rae draws the emoji from Noto Color Emoji itself (lib/text/ColorEmojiFont).
 *
 * Header-only: included (`cheader`) by programs whose Rae imports lib/Gpu2dEmoji. */
#ifndef RAE_RUNTIME_EMOJI_H
#define RAE_RUNTIME_EMOJI_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rae_runtime.h"

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <dlfcn.h>
#include <pthread.h>

/* CoreText's functions this file calls, from the framework loaded at run time */
typedef struct {
  CTFontRef (*createFont)(CFStringRef, CGFloat, const CGAffineTransform*);
  CTLineRef (*createLine)(CFAttributedStringRef);
  double (*lineBounds)(CTLineRef, CGFloat*, CGFloat*, CGFloat*);
  void (*drawLine)(CTLineRef, CGContextRef);
  const CFStringRef* fontAttribute;
} RaeEmojiCoreText;

static RaeEmojiCoreText rae_emoji_core_text;
static pthread_once_t rae_emoji_core_text_once = PTHREAD_ONCE_INIT;

static void rae_emoji_load_core_text(void) {
  void* framework = dlopen("/System/Library/Frameworks/CoreText.framework/CoreText", RTLD_LAZY);
  if (!framework) return;
  rae_emoji_core_text.createFont = (CTFontRef (*)(CFStringRef, CGFloat, const CGAffineTransform*))dlsym(framework, "CTFontCreateWithName");
  rae_emoji_core_text.createLine = (CTLineRef (*)(CFAttributedStringRef))dlsym(framework, "CTLineCreateWithAttributedString");
  rae_emoji_core_text.lineBounds = (double (*)(CTLineRef, CGFloat*, CGFloat*, CGFloat*))dlsym(framework, "CTLineGetTypographicBounds");
  rae_emoji_core_text.drawLine = (void (*)(CTLineRef, CGContextRef))dlsym(framework, "CTLineDraw");
  rae_emoji_core_text.fontAttribute = (const CFStringRef*)dlsym(framework, "kCTFontAttributeName");
}

/* Draw `text` with the system's color emoji font, centered in `pixels` x `pixels`, into `out`
 * (pixels * pixels * 4 bytes: RGBA, premultiplied alpha, rows from the top). 1 when something was
 * drawn, 0 when not (no font, no color in the result). */
int64_t rae_ext_Gpu2dEmoji_rasterize(rae_String text, int64_t pixels, uint8_t* out) {
  if (pixels <= 0 || !out || text.len <= 0) return 0;
  pthread_once(&rae_emoji_core_text_once, rae_emoji_load_core_text);
  RaeEmojiCoreText* core = &rae_emoji_core_text;
  if (!core->createFont || !core->createLine || !core->lineBounds || !core->drawLine || !core->fontAttribute) return 0;
  size_t size = (size_t)pixels * (size_t)pixels * 4;
  memset(out, 0, size);
  CFStringRef string = CFStringCreateWithBytes(NULL, text.data, (CFIndex)text.len, kCFStringEncodingUTF8, false);
  if (!string) return 0;
  /* the emoji's box at a font size is about 1.17 em wide: this size fills the square */
  CTFontRef font = core->createFont(CFSTR("AppleColorEmoji"), (CGFloat)pixels / 1.17, NULL);
  int64_t drawn = 0;
  if (font) {
    CFStringRef keys[1] = {*core->fontAttribute};
    CFTypeRef values[1] = {font};
    CFDictionaryRef attributes = CFDictionaryCreate(NULL, (const void**)keys, (const void**)values, 1,
                                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed = CFAttributedStringCreate(NULL, string, attributes);
    CTLineRef line = core->createLine(attributed);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(out, (size_t)pixels, (size_t)pixels, 8, (size_t)pixels * 4, space,
                                                 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (context && line) {
      CGFloat ascent = 0, descent = 0, leading = 0;
      double width = core->lineBounds(line, &ascent, &descent, &leading);
      CGFloat x = ((CGFloat)pixels - (CGFloat)width) * 0.5;
      CGFloat y = ((CGFloat)pixels - (ascent + descent)) * 0.5 + descent;
      CGContextSetTextPosition(context, x, y);
      core->drawLine(line, context);
      CGContextFlush(context);
      const unsigned char* bytes = (const unsigned char*)out;
      for (size_t i = 3; i < size; i += 4) {
        if (bytes[i]) { drawn = 1; break; }
      }
    }
    if (context) CGContextRelease(context);
    CGColorSpaceRelease(space);
    if (line) CFRelease(line);
    CFRelease(attributed);
    CFRelease(attributes);
    CFRelease(font);
  }
  CFRelease(string);
  return drawn;
}
#elif defined(__ANDROID__)
#include <jni.h>
#include <pthread.h>
#include <SDL3/SDL_system.h>

/* RaeEmoji is looked up once through the activity's class loader: a thread that did not start in
 * Java finds only the system's classes with FindClass */
static jclass rae_emoji_class = NULL;
static jmethodID rae_emoji_draw_method = NULL;
static pthread_mutex_t rae_emoji_class_lock = PTHREAD_MUTEX_INITIALIZER;

static int rae_emoji_failed_call(JNIEnv* env) {
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    return 1;
  }
  return 0;
}

static int rae_emoji_find_class(JNIEnv* env) {
  pthread_mutex_lock(&rae_emoji_class_lock);
  if (!rae_emoji_class) {
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (activity) {
      jclass activityClass = (*env)->GetObjectClass(env, activity);
      jmethodID getLoader = (*env)->GetMethodID(env, activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
      jobject loader = (*env)->CallObjectMethod(env, activity, getLoader);
      jclass loaderClass = (*env)->GetObjectClass(env, loader);
      jmethodID loadClass = (*env)->GetMethodID(env, loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
      jstring name = (*env)->NewStringUTF(env, "org.rae.app.RaeEmoji");
      jclass found = (jclass)(*env)->CallObjectMethod(env, loader, loadClass, name);
      if (!rae_emoji_failed_call(env) && found) {
        rae_emoji_draw_method = (*env)->GetStaticMethodID(env, found, "draw", "(Ljava/lang/String;I)[B");
        if (!rae_emoji_failed_call(env) && rae_emoji_draw_method) rae_emoji_class = (jclass)(*env)->NewGlobalRef(env, found);
        (*env)->DeleteLocalRef(env, found);
      }
      (*env)->DeleteLocalRef(env, name);
      (*env)->DeleteLocalRef(env, loaderClass);
      (*env)->DeleteLocalRef(env, loader);
      (*env)->DeleteLocalRef(env, activityClass);
      (*env)->DeleteLocalRef(env, activity);
    }
  }
  int found = rae_emoji_class != NULL;
  pthread_mutex_unlock(&rae_emoji_class_lock);
  return found;
}

/* As on Apple: `text` drawn by the system's emoji font into `out`; 1 when something was drawn */
int64_t rae_ext_Gpu2dEmoji_rasterize(rae_String text, int64_t pixels, uint8_t* out) {
  if (pixels <= 0 || !out || text.len <= 0) return 0;
  JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
  if (!env || !rae_emoji_find_class(env)) return 0;
  size_t size = (size_t)pixels * (size_t)pixels * 4;
  memset(out, 0, size);
  char* utf8 = (char*)malloc((size_t)text.len + 1);
  if (!utf8) return 0;
  memcpy(utf8, text.data, (size_t)text.len);
  utf8[text.len] = 0;
  jstring string = (*env)->NewStringUTF(env, utf8);
  free(utf8);
  if (rae_emoji_failed_call(env) || !string) return 0;
  jbyteArray drawn = (jbyteArray)(*env)->CallStaticObjectMethod(env, rae_emoji_class, rae_emoji_draw_method, string,
                                                               (jint)pixels);
  (*env)->DeleteLocalRef(env, string);
  if (rae_emoji_failed_call(env) || !drawn) return 0;
  int64_t result = 0;
  if ((size_t)(*env)->GetArrayLength(env, drawn) == size) {
    (*env)->GetByteArrayRegion(env, drawn, 0, (jsize)size, (jbyte*)out);
    for (size_t i = 3; i < size; i += 4) {
      if (out[i]) { result = 1; break; }
    }
  }
  (*env)->DeleteLocalRef(env, drawn);
  return result;
}
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>

/* The browser's text stack draws the cluster with the page's emoji font: a 2D canvas (an
 * OffscreenCanvas where there is one) and fillText, read back with getImageData. 2 when something
 * was drawn (getImageData's bytes are straight alpha), 0 when not. */
EM_JS(int, rae_emoji_draw_canvas, (const char* text, int pixels, unsigned char* out), {
  try {
    var canvas = (typeof OffscreenCanvas !== "undefined") ? new OffscreenCanvas(pixels, pixels)
      : document.createElement("canvas");
    canvas.width = pixels;
    canvas.height = pixels;
    var context = canvas.getContext("2d", { willReadFrequently: true });
    if (!context) return 0;
    context.font = (pixels / 1.17) + 'px "Apple Color Emoji", "Segoe UI Emoji", "Noto Color Emoji", sans-serif';
    context.textAlign = "center";
    context.textBaseline = "alphabetic";
    var cluster = UTF8ToString(text);
    var box = context.measureText(cluster);
    var ascent = box.actualBoundingBoxAscent || pixels * 0.8;
    var descent = box.actualBoundingBoxDescent || pixels * 0.2;
    context.fillStyle = "#000";
    context.fillText(cluster, pixels / 2, (pixels + ascent - descent) / 2);
    var data = context.getImageData(0, 0, pixels, pixels).data;
    HEAPU8.set(data, out);
    for (var i = 3; i < data.length; i += 4) {
      if (data[i] !== 0) return 2;
    }
    return 0;
  } catch (error) {
    return 0;
  }
});

/* As on Apple, but the bytes are straight alpha: 2 when something was drawn */
int64_t rae_ext_Gpu2dEmoji_rasterize(rae_String text, int64_t pixels, uint8_t* out) {
  if (pixels <= 0 || !out || text.len <= 0) return 0;
  char* utf8 = (char*)malloc((size_t)text.len + 1);
  if (!utf8) return 0;
  memcpy(utf8, text.data, (size_t)text.len);
  utf8[text.len] = 0;
  int drawn = rae_emoji_draw_canvas(utf8, (int)pixels, out);
  free(utf8);
  return (int64_t)drawn;
}
#else
int64_t rae_ext_Gpu2dEmoji_rasterize(rae_String text, int64_t pixels, uint8_t* out) {
  (void)text; (void)pixels; (void)out;
  return 0;
}
#endif

#endif /* RAE_RUNTIME_EMOJI_H */
