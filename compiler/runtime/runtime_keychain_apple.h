/* The Keychain for lib/Keychain (docs/keychain.md): a small secret (bytes)
 * stored by service and account name on iOS and macOS, optionally readable
 * only after Face ID / Touch ID / the device passcode.
 *
 * Platform reason: Security.framework's SecItem calls, which take
 * CoreFoundation dictionaries Rae cannot build. Each function makes ONE
 * SecItem call (building its query dictionary is the ABI glue around it) and
 * answers the OSStatus (0 = errSecSuccess; errors are negative). Storing
 * (delete, then add), what an error means and which options to ask for are
 * Rae's.
 *
 * Header-only: included (`cheader`) by a program whose Rae imports
 * lib/Keychain; the compiler then links -framework Security. */
#ifndef RAE_RUNTIME_KEYCHAIN_APPLE_H
#define RAE_RUNTIME_KEYCHAIN_APPLE_H

#include <stdint.h>

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <TargetConditionals.h>
#include <string.h>

static CFStringRef rae_keychain_string(const uint8_t* textZ) {
  return CFStringCreateWithCString(kCFAllocatorDefault, (const char*)textZ, kCFStringEncodingUTF8);
}

/* The query of one generic password: its class, service and account; with
 * `protectedItem` the data-protection keychain (always on iOS; on macOS it
 * is what an access control needs, and wants a signed app with a keychain
 * entitlement) */
static CFMutableDictionaryRef rae_keychain_query(const uint8_t* serviceZ, const uint8_t* accountZ, int protectedItem) {
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = rae_keychain_string(serviceZ);
  CFStringRef account = rae_keychain_string(accountZ);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
#if TARGET_OS_OSX
  if (protectedItem) CFDictionarySetValue(query, kSecUseDataProtectionKeychain, kCFBooleanTrue);
#else
  (void)protectedItem;
#endif
  CFRelease(service);
  CFRelease(account);
  return query;
}

/* Add the item (SecItemAdd): `count` bytes of `bytes` from `offset`.
 * `userPresence` makes reading it ask for Face ID / Touch ID / the passcode
 * (and keeps it on this device, readable while unlocked); otherwise it is
 * readable after the first unlock, on this device only. */
int64_t rae_ext_Keychain_add(uint8_t* serviceZ, uint8_t* accountZ, uint8_t* bytes, int64_t offset, int64_t count,
                             int64_t userPresence) {
  CFMutableDictionaryRef query = rae_keychain_query(serviceZ, accountZ, userPresence != 0);
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, count > 0 ? bytes + offset : (const uint8_t*)"",
                                (CFIndex)(count > 0 ? count : 0));
  CFDictionarySetValue(query, kSecValueData, data);
  SecAccessControlRef access = NULL;
  if (userPresence) {
    CFErrorRef error = NULL;
    access = SecAccessControlCreateWithFlags(kCFAllocatorDefault, kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
                                             kSecAccessControlUserPresence, &error);
    if (!access) {
      if (error) CFRelease(error);
      CFRelease(data);
      CFRelease(query);
      return errSecParam;
    }
    CFDictionarySetValue(query, kSecAttrAccessControl, access);
  } else {
    CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
  }
  OSStatus status = SecItemAdd(query, NULL);
  if (access) CFRelease(access);
  CFRelease(data);
  CFRelease(query);
  return status;
}

/* Delete the item (SecItemDelete) from the keychain `protectedItem` names */
int64_t rae_ext_Keychain_delete(uint8_t* serviceZ, uint8_t* accountZ, int64_t protectedItem) {
  CFMutableDictionaryRef query = rae_keychain_query(serviceZ, accountZ, protectedItem != 0);
  OSStatus status = SecItemDelete(query);
  CFRelease(query);
  return status;
}

/* Read the item (SecItemCopyMatching) into `into` (up to `capacity` bytes):
 * the item's length (copy again with a larger buffer when it exceeds
 * `capacity`), or the negative OSStatus. A protected item asks for Face ID /
 * the passcode, showing `promptZ`; `interactive` 0 fails instead of asking. */
int64_t rae_ext_Keychain_read(uint8_t* serviceZ, uint8_t* accountZ, int64_t protectedItem, uint8_t* promptZ,
                              int64_t interactive, uint8_t* into, int64_t capacity) {
  CFMutableDictionaryRef query = rae_keychain_query(serviceZ, accountZ, protectedItem != 0);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFStringRef prompt = NULL;
  if (promptZ && promptZ[0]) {
    prompt = rae_keychain_string(promptZ);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFDictionarySetValue(query, kSecUseOperationPrompt, prompt);
    if (!interactive) CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
  } else if (!interactive) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
  }
  CFTypeRef result = NULL;
  OSStatus status = SecItemCopyMatching(query, &result);
  if (prompt) CFRelease(prompt);
  CFRelease(query);
  if (status != errSecSuccess) return status < 0 ? status : -(int64_t)status;
  int64_t length = 0;
  if (result && CFGetTypeID(result) == CFDataGetTypeID()) {
    CFDataRef data = (CFDataRef)result;
    length = (int64_t)CFDataGetLength(data);
    if (length <= capacity && length > 0) memcpy(into, CFDataGetBytePtr(data), (size_t)length);
  }
  if (result) CFRelease(result);
  return length;
}

/* The system's words for an OSStatus into `into` (up to `capacity` bytes):
 * their length (0 when there are none) */
int64_t rae_ext_Keychain_errorMessage(int64_t status, uint8_t* into, int64_t capacity) {
  CFStringRef message = SecCopyErrorMessageString((OSStatus)status, NULL);
  if (!message) return 0;
  char text[512] = "";
  CFStringGetCString(message, text, sizeof text, kCFStringEncodingUTF8);
  CFRelease(message);
  int64_t length = (int64_t)strlen(text);
  if (length > capacity) length = capacity;
  memcpy(into, text, (size_t)length);
  return length;
}
#endif

#endif
