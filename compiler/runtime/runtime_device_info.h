/* The platform's facts about the device for lib/DeviceInfo (docs/device-info.md): the
 * hardware model, the physical memory, the GPU's name, the OS version and the
 * preferred locale.
 *
 * Platform reason: each is a system call or a platform API, one per function:
 * sysctlbyname on Apple, a system property on Android (__system_property_get),
 * sysconf for the memory pages elsewhere, CoreFoundation's preferred languages on
 * Apple, SDL's preferred locales on Android (Java's Locale, which SDL already
 * reaches), and Vulkan's physical device properties on Android. Which name to ask
 * for, the units and every fallback are Rae's (lib/DeviceInfo.rae).
 *
 * The Vulkan device name is the one sequence here (load the loader, make an
 * instance, list the devices, read the first one's properties, destroy the
 * instance): Vulkan has no single call for it, and it has no choices in it. It
 * costs a few milliseconds, so it is read once (pthread_once) and kept.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports lib/DeviceInfo. */
#ifndef RAE_RUNTIME_DEVICE_INFO_H
#define RAE_RUNTIME_DEVICE_INFO_H

#include <stdint.h>
#include <string.h>
#include "rae_runtime.h"

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <sys/sysctl.h>
#include <CoreFoundation/CoreFoundation.h>

/* A sysctl's text value ("hw.machine": "iPhone16,1"; "hw.model": "Mac15,9";
 * "kern.osproductversion": "18.1"), "" when there is none */
rae_String rae_ext_DeviceInfo_sysctlText(rae_String name) {
  char key[64];
  size_t keyLength = (size_t)name.len < sizeof(key) - 1 ? (size_t)name.len : sizeof(key) - 1;
  memcpy(key, name.data, keyLength);
  key[keyLength] = 0;
  char value[256];
  size_t size = sizeof(value);
  if (sysctlbyname(key, value, &size, NULL, 0) != 0 || size == 0) return rae_ext_rae_str_from_cstr("");
  value[sizeof(value) - 1] = 0;
  return rae_ext_rae_str_from_cstr(value);
}

/* A sysctl's 64-bit number ("hw.memsize": bytes), -1 when there is none */
int64_t rae_ext_DeviceInfo_sysctlInt(rae_String name) {
  char key[64];
  size_t keyLength = (size_t)name.len < sizeof(key) - 1 ? (size_t)name.len : sizeof(key) - 1;
  memcpy(key, name.data, keyLength);
  key[keyLength] = 0;
  int64_t value = 0;
  size_t size = sizeof(value);
  if (sysctlbyname(key, &value, &size, NULL, 0) != 0) return -1;
  return value;
}

/* The first of the person's preferred languages ("en-FI"), "" when there is none */
rae_String rae_ext_DeviceInfo_preferredLanguage(void) {
  char text[64] = "";
  CFArrayRef languages = CFLocaleCopyPreferredLanguages();
  if (languages) {
    if (CFArrayGetCount(languages) > 0) {
      CFStringRef first = (CFStringRef)CFArrayGetValueAtIndex(languages, 0);
      if (!CFStringGetCString(first, text, sizeof(text), kCFStringEncodingUTF8)) text[0] = 0;
    }
    CFRelease(languages);
  }
  return rae_ext_rae_str_from_cstr(text);
}
#endif /* Apple */

#if !defined(_WIN32) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <unistd.h>

/* The physical memory's pages and the page's size (sysconf), -1 when not known */
int64_t rae_ext_DeviceInfo_physicalPages(void) { return (int64_t)sysconf(_SC_PHYS_PAGES); }

int64_t rae_ext_DeviceInfo_pageSize(void) { return (int64_t)sysconf(_SC_PAGESIZE); }
#endif

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/system_properties.h>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

/* A system property ("ro.product.model": "Pixel 8"; "ro.build.version.release":
 * "15"), "" when there is none */
rae_String rae_ext_DeviceInfo_systemProperty(rae_String name) {
  char key[PROP_NAME_MAX];
  size_t keyLength = (size_t)name.len < sizeof(key) - 1 ? (size_t)name.len : sizeof(key) - 1;
  memcpy(key, name.data, keyLength);
  key[keyLength] = 0;
  char value[PROP_VALUE_MAX] = "";
  __system_property_get(key, value);
  return rae_ext_rae_str_from_cstr(value);
}

/* The first preferred locale as a language tag ("en-FI", or "en" without a
 * country), "" when there is none */
rae_String rae_ext_DeviceInfo_sdlLocale(void) {
  char text[64] = "";
  int count = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&count);
  if (locales) {
    if (count > 0 && locales[0] && locales[0]->language) {
      if (locales[0]->country) snprintf(text, sizeof(text), "%s-%s", locales[0]->language, locales[0]->country);
      else snprintf(text, sizeof(text), "%s", locales[0]->language);
    }
    SDL_free(locales);
  }
  return rae_ext_rae_str_from_cstr(text);
}

static pthread_once_t rae_device_gpu_once = PTHREAD_ONCE_INIT;
static char rae_device_gpu_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "";

static void rae_device_read_gpu_name(void) {
  void* loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!loader) return;
  PFN_vkGetInstanceProcAddr getProc = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance createInstance = getProc ? (PFN_vkCreateInstance)getProc(NULL, "vkCreateInstance") : NULL;
  VkInstance instance = VK_NULL_HANDLE;
  VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &application;
  if (createInstance && createInstance(&info, NULL, &instance) == VK_SUCCESS) {
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)getProc(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)getProc(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)getProc(instance, "vkDestroyInstance");
    uint32_t count = 1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkResult listed = enumerate ? enumerate(instance, &count, &device) : VK_ERROR_INITIALIZATION_FAILED;
    if ((listed == VK_SUCCESS || listed == VK_INCOMPLETE) && count > 0 && device && properties) {
      VkPhysicalDeviceProperties found;
      properties(device, &found);
      memcpy(rae_device_gpu_name, found.deviceName, sizeof(rae_device_gpu_name));
      rae_device_gpu_name[sizeof(rae_device_gpu_name) - 1] = 0;
    }
    if (destroy) destroy(instance, NULL);
  }
  dlclose(loader);
}

/* The first Vulkan device's name ("Adreno (TM) 740", "Mali-G57"), "" when there is
 * no Vulkan; read once */
rae_String rae_ext_DeviceInfo_vulkanDeviceName(void) {
  pthread_once(&rae_device_gpu_once, rae_device_read_gpu_name);
  return rae_ext_rae_str_from_cstr(rae_device_gpu_name);
}
#endif /* Android */

#endif /* RAE_RUNTIME_DEVICE_INFO_H */
