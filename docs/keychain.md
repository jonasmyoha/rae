# lib/Keychain

A small secret (bytes: a device's private key, a token) kept in the system
Keychain on iOS and macOS, by service and account name. Every failure is a
`KeychainError { kind, status, message }`: `notFound`, `duplicate`,
`authenticationFailed` (cancelled or failed Face ID / passcode),
`interactionNotAllowed`, `missingEntitlement`, `locked`, `unsupported`,
`other`, with the OSStatus and the system's words.

```rae
let stored: KeychainError = keychainStore(service: "org.example.sync", account: "device-key", secret: privateKey, requireUserPresence: false)
let read: KeychainRead = keychainRead(service: "org.example.sync", account: "device-key", options: keychainDefaultReadOptions())
if keychainFailed(error: read.error) { ... } else { use(read.bytes) }
let deleted: KeychainError = keychainDelete(service: "org.example.sync", account: "device-key")
```

- `keychainStore` replaces what was there. Without `requireUserPresence` the
  secret is readable after the first unlock since boot (background sync can
  use it), on this device only: never synced to iCloud, not in a backup that
  restores to another device.
- With `requireUserPresence`, each read asks for Face ID, Touch ID or the
  device passcode (`KeychainReadOptions.prompt` says why); the secret is
  readable while the device is unlocked, on this device only. A read with
  `interactive: false` fails with `interactionNotAllowed` instead of asking.
  On macOS such a secret lives in the data-protection keychain, which only a
  signed app with a keychain-access-groups entitlement may use: a command-line
  program gets `missingEntitlement`. (On iOS every app has it.)
- Off Apple every call answers `unsupported`.

The C side, `compiler/runtime/runtime_keychain_apple.h` (platform reason:
Security.framework's SecItem calls take CoreFoundation dictionaries), is one
SecItem call per function; it is header-only and included only by programs
that import `Keychain`, which alone link `-framework Security`. Case 1291 runs
against the real login Keychain on macOS under a random account name (a new
binary per `rae run` would otherwise meet an earlier run's item, and macOS asks
before letting another program read it).
